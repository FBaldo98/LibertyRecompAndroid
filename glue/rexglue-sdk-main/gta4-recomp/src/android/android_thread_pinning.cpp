// Keeps the busiest threads of the process on dedicated big cores.
//
// Six threads carry the frame (guest render thread, recorder, command worker,
// and three lighter ones) and the scheduler moves them freely: the guest
// render thread was seen on a little core while the prime core idled, and the
// recorder spent ~30% of each frame off-CPU. Every two seconds this ranks the
// process's threads by CPU time over the last interval, gives the top ones a
// core each (the fastest under the current clock limits first) and confines
// every other thread to the remaining cores. Threads are re-ranked continuously, so roles that move
// between threads (loading, cutscenes) follow.
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include <android/log.h>

#include <rex/cvar.h>

// Two by default, for the OPPO CPH2765 this fork targets: its two prime cores
// (cpu5-6) were clocked to 2.2 GHz while the five others sat at 1.1-1.4 GHz,
// and the thread that sets the frame time was mostly found on the slow ones.
REXCVAR_DEFINE_UINT32(gta4_thread_pinning, 2, "GTA IV/Performance",
                      "Give the N busiest threads a big core each and keep the others off them "
                      "(0 = leave scheduling to Android)")
    .range(0, 4);

namespace {

uint64_t ThreadCpuTicks(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return 0;
  char buffer[512];
  const size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  buffer[length] = 0;
  // Fields after the parenthesized name: state is field 3, utime 14, stime 15.
  const char* p = std::strrchr(buffer, ')');
  if (!p) return 0;
  unsigned long long utime = 0, stime = 0;
  int field = 2;
  for (const char* c = p + 1; *c && field < 15; ++c) {
    if (*c == ' ') {
      ++field;
      if (field == 14) utime = std::strtoull(c + 1, nullptr, 10);
      if (field == 15) stime = std::strtoull(c + 1, nullptr, 10);
    }
  }
  return utime + stime;
}

long ReadCpuFrequency(int cpu, const char* name) {
  char path[96];
  std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, name);
  long frequency = 0;
  if (FILE* file = std::fopen(path, "r")) {
    if (std::fscanf(file, "%ld", &frequency) != 1) frequency = 0;
    std::fclose(file);
  }
  return frequency;
}

// Cores by the clock they may reach right now, fastest first. The thermal
// limits move per cluster and can invert the hardware order: at thermal status
// 4 the OPPO CPH2765 held its prime pair (4.61 GHz) at 0.77 GHz and the five
// other cores at 1.11 GHz, and the two busiest threads sat on the slowest
// cores of the phone. Limits within a tenth of the best count as equal, so
// near-equal clusters do not trade the threads back and forth; the hardware
// maximum then decides.
std::vector<int> CoresByClockLimit(long& best_limit) {
  struct Core {
    long limit;
    long maximum;
    int cpu;
  };
  std::vector<Core> cores;
  const long count = sysconf(_SC_NPROCESSORS_CONF);
  long best = 0;
  for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
    const long maximum = ReadCpuFrequency(cpu, "cpuinfo_max_freq");
    long limit = ReadCpuFrequency(cpu, "scaling_max_freq");
    if (!limit) limit = maximum;
    best = std::max(best, limit);
    cores.push_back({limit, maximum, cpu});
  }
  best_limit = best;
  for (Core& core : cores) {
    if (core.limit >= best - best / 10) core.limit = best;
  }
  std::stable_sort(cores.begin(), cores.end(), [](const Core& a, const Core& b) {
    return a.limit != b.limit ? a.limit > b.limit : a.maximum > b.maximum;
  });
  std::vector<int> result;
  for (const Core& core : cores) result.push_back(core.cpu);
  return result;
}

void SetAffinity(pid_t tid, const cpu_set_t& set) {
  syscall(__NR_sched_setaffinity, tid, sizeof(set), &set);
}

std::atomic<bool> g_clock_floor{false};

// The phone's hardest throttling: no core may pass about 1 GHz (cpu0-4 at
// 0.998 GHz and the prime pair at 0.77 GHz on the OPPO CPH2765). Thermal
// status 4 is not that state by itself: at status 4 and 36 degrees C the phone
// still gave a cluster 2.2 GHz while one of its threads was at full load.
constexpr long kClockFloorKhz = 1'050'000;

bool IsThread(pid_t tid, const char* name) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return false;
  char comm[32] = {};
  const bool read = std::fgets(comm, sizeof(comm), file) != nullptr;
  std::fclose(file);
  return read && std::strncmp(comm, name, std::strlen(name)) == 0;
}

void PinningLoop() {
  if (sysconf(_SC_NPROCESSORS_CONF) < 6) return;
  std::unordered_map<pid_t, uint64_t> previous;
  uint32_t applied = 0;
  std::vector<int> applied_cores;
  pid_t recorder = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    long best_limit = 0;
    const std::vector<int> cores = CoresByClockLimit(best_limit);
    const bool clock_floor = best_limit && best_limit <= kClockFloorKhz;
    g_clock_floor.store(clock_floor, std::memory_order_relaxed);
    // At the clock floor a core of its own no longer helps a thread: the same
    // scene ran at 21.5, 23.9 and 21.8 FPS with two threads pinned, unpinned
    // and pinned again.
    const uint32_t pinned =
        clock_floor ? 0 : std::min<uint32_t>(REXCVAR_GET(gta4_thread_pinning), 4);
    std::vector<std::pair<uint64_t, pid_t>> usage;
    std::unordered_map<pid_t, uint64_t> current;
    if (DIR* directory = opendir("/proc/self/task")) {
      while (dirent* entry = readdir(directory)) {
        const pid_t tid = pid_t(std::atoi(entry->d_name));
        if (tid <= 0) continue;
        const uint64_t ticks = ThreadCpuTicks(tid);
        current[tid] = ticks;
        const auto old = previous.find(tid);
        usage.emplace_back(old != previous.end() && ticks >= old->second ? ticks - old->second : 0,
                           tid);
      }
      closedir(directory);
    }
    previous.swap(current);
    if (!pinned && !applied) continue;
    std::sort(usage.rbegin(), usage.rend());
    // The recorder sets the frame time and comes first whatever its rank. The
    // phone sets a cluster's clock limit from that cluster's load, and it is
    // the title thread's spin (the busiest thread) that holds the limit up:
    // the two have to share a cluster, which the first two cores always do.
    if (!recorder || !previous.count(recorder) || !IsThread(recorder, "GtaRecorder")) {
      recorder = 0;
      for (const auto& [ticks, tid] : usage) {
        if (IsThread(tid, "GtaRecorder")) {
          recorder = tid;
          break;
        }
      }
    }
    if (recorder) {
      const auto found = std::find_if(usage.begin(), usage.end(),
                                      [&](const auto& entry) { return entry.second == recorder; });
      if (found != usage.end()) std::rotate(usage.begin(), found, found + 1);
    }
    cpu_set_t rest;
    CPU_ZERO(&rest);
    for (size_t i = pinned; i < cores.size(); ++i) CPU_SET(cores[i], &rest);
    for (size_t i = 0; i < usage.size(); ++i) {
      cpu_set_t set;
      CPU_ZERO(&set);
      if (!pinned) {
        for (int cpu : cores) CPU_SET(cpu, &set);
      } else if (i < pinned) {
        CPU_SET(cores[i], &set);
      } else {
        set = rest;
      }
      SetAffinity(usage[i].second, set);
    }
    const std::vector<int> pinned_cores(cores.begin(), cores.begin() + pinned);
    if (pinned != applied || pinned_cores != applied_cores) {
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                          "thread-pinning: %u threads on cores %d,%d,%d best-limit-khz=%ld", pinned,
                          cores[0], cores[1], cores[2], best_limit);
    }
    applied = pinned;
    applied_cores = pinned_cores;
  }
}

const bool g_pinning_started = [] {
  std::thread(PinningLoop).detach();
  return true;
}();

}  // namespace

// Whether every core was held at the clock floor two seconds ago, for the
// other Android hooks.
extern "C" bool liberty_android_clock_floor() {
  return g_clock_floor.load(std::memory_order_relaxed);
}
