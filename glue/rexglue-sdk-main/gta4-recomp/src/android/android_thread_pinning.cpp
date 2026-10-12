// Keeps the busiest threads of the process on dedicated big cores.
//
// Six threads carry the frame (guest render thread, recorder, command worker,
// and three lighter ones) and the scheduler moves them freely: the guest
// render thread was seen on a little core while the prime core idled, and the
// recorder spent ~30% of each frame off-CPU. Every two seconds this ranks the
// process's threads by CPU time over the last interval, gives the top ones a
// big core each (fastest first) and confines every other thread to the
// remaining cores. Threads are re-ranked continuously, so roles that move
// between threads (loading, cutscenes) follow.
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
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

std::vector<int> CoresByMaxFrequency() {
  std::vector<std::pair<long, int>> cores;
  const long count = sysconf(_SC_NPROCESSORS_CONF);
  for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
    char path[96];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    long frequency = 0;
    if (FILE* file = std::fopen(path, "r")) {
      if (std::fscanf(file, "%ld", &frequency) != 1) frequency = 0;
      std::fclose(file);
    }
    cores.emplace_back(frequency, cpu);
  }
  std::stable_sort(cores.begin(), cores.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<int> result;
  for (const auto& [frequency, cpu] : cores) result.push_back(cpu);
  return result;
}

void SetAffinity(pid_t tid, const cpu_set_t& set) {
  syscall(__NR_sched_setaffinity, tid, sizeof(set), &set);
}

void PinningLoop() {
  const std::vector<int> cores = CoresByMaxFrequency();
  if (cores.size() < 6) return;
  std::unordered_map<pid_t, uint64_t> previous;
  uint32_t applied = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const uint32_t pinned = std::min<uint32_t>(REXCVAR_GET(gta4_thread_pinning), 4);
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
    if (pinned != applied) {
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "thread-pinning: %u threads on cores %d,%d,%d",
                          pinned, cores[0], cores[1], cores[2]);
    }
    applied = pinned;
  }
}

const bool g_pinning_started = [] {
  std::thread(PinningLoop).detach();
  return true;
}();

}  // namespace
