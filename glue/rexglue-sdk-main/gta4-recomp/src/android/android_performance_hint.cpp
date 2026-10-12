// Reports the frame's CPU work to Android's performance hint service (ADPF).
//
// Some devices clock the CPU down for an app whose load they do not
// understand: an OPPO CPH2765 (SM8850) held its performance cores at 1.1 GHz
// of 3.6 GHz through gameplay and the game missed 30 FPS on them. A hint
// session names the threads that carry the frame, the time they have per
// frame and the time they actually took, so the system can raise their clocks
// while they are over budget and lower them again when there is slack.
//
// The busiest threads are re-ranked every two seconds, like the thread
// pinning does, so roles that move between threads follow. The work of a
// frame is taken as the CPU time of the busiest of those threads: the frame's
// stages run in parallel, and the slowest one sets the frame time.
//
// The API is Android 13's (session thread updates Android 14's); the library
// is built against API 28, so it is looked up at run time.
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <android/log.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(android_performance_hint, true, "GTA IV/Performance",
                    "Report the frame's CPU work to Android's performance hint service, so the "
                    "system clocks the busiest threads for the frame rate limit");

namespace {

std::atomic<uint32_t> g_presented_frames{0};

constexpr size_t kMaxThreads = 6;
// CPU time over a ranking interval a thread needs to belong to the session.
constexpr int64_t kMinRankedNanos = 100'000'000;
constexpr auto kReportInterval = std::chrono::milliseconds(100);
constexpr auto kRankInterval = std::chrono::seconds(2);
constexpr auto kLogInterval = std::chrono::seconds(10);

struct HintApi {
  void* (*get_manager)() = nullptr;
  void* (*create_session)(void* manager, const int32_t* threads, size_t count,
                          int64_t target_nanos) = nullptr;
  int (*update_target)(void* session, int64_t target_nanos) = nullptr;
  int (*report_actual)(void* session, int64_t actual_nanos) = nullptr;
  void (*close_session)(void* session) = nullptr;
  // Android 14; without it a changed thread set needs a new session.
  int (*set_threads)(void* session, const int32_t* threads, size_t count) = nullptr;

  bool usable() const {
    return get_manager && create_session && update_target && report_actual && close_session;
  }
};

HintApi LoadHintApi() {
  HintApi api;
  void* library = dlopen("libandroid.so", RTLD_NOW);
  if (!library) return api;
  const auto find = [library](const char* name) { return dlsym(library, name); };
  api.get_manager = reinterpret_cast<decltype(api.get_manager)>(find("APerformanceHint_getManager"));
  api.create_session =
      reinterpret_cast<decltype(api.create_session)>(find("APerformanceHint_createSession"));
  api.update_target = reinterpret_cast<decltype(api.update_target)>(
      find("APerformanceHint_updateTargetWorkDuration"));
  api.report_actual = reinterpret_cast<decltype(api.report_actual)>(
      find("APerformanceHint_reportActualWorkDuration"));
  api.close_session =
      reinterpret_cast<decltype(api.close_session)>(find("APerformanceHint_closeSession"));
  api.set_threads = reinterpret_cast<decltype(api.set_threads)>(find("APerformanceHint_setThreads"));
  return api;
}

// CPU time of another thread of this process; 0 once it has exited.
int64_t ThreadCpuNanos(pid_t tid) {
  // The kernel's per-thread CPU clock id, as pthread_getcpuclockid builds it.
  const clockid_t clock = (~static_cast<clockid_t>(tid) << 3) | 6;
  timespec time{};
  if (clock_gettime(clock, &time) != 0) return 0;
  return int64_t(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

// The core a thread last ran on (field 39 of its stat file); -1 if unknown.
int ThreadCore(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  FILE* file = std::fopen(path, "r");
  if (!file) return -1;
  char buffer[1024];
  const size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  buffer[length] = 0;
  // The name may hold spaces; the fields after its closing parenthesis start
  // at 3 (state).
  const char* c = std::strrchr(buffer, ')');
  if (!c) return -1;
  int field = 2;
  for (++c; *c; ++c) {
    if (*c == ' ' && ++field == 39) return std::atoi(c + 1);
  }
  return -1;
}

// The threads that used the most CPU since the previous call, sorted by id.
std::vector<int32_t> BusiestThreads(std::unordered_map<pid_t, int64_t>& previous) {
  std::vector<std::pair<int64_t, pid_t>> usage;
  std::unordered_map<pid_t, int64_t> current;
  if (DIR* directory = opendir("/proc/self/task")) {
    while (dirent* entry = readdir(directory)) {
      const pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;
      const int64_t nanos = ThreadCpuNanos(tid);
      current[tid] = nanos;
      const auto old = previous.find(tid);
      if (old != previous.end() && nanos - old->second >= kMinRankedNanos) {
        usage.emplace_back(nanos - old->second, tid);
      }
    }
    closedir(directory);
  }
  previous.swap(current);
  std::sort(usage.rbegin(), usage.rend());
  if (usage.size() > kMaxThreads) usage.resize(kMaxThreads);
  std::vector<int32_t> threads;
  for (const auto& [nanos, tid] : usage) threads.push_back(tid);
  std::sort(threads.begin(), threads.end());
  return threads;
}

// The CPU time a thread may use per frame. A stage that needs the whole
// frame interval already misses frames whenever it is descheduled, so the
// system is asked to keep a fifth of it free.
int64_t TargetNanos() {
  const uint32_t limit = rex::cvar::Query<uint32_t>("gta4_frame_limit");
  return 1'000'000'000 / (limit ? limit : 120) * 4 / 5;
}

void HintLoop() {
  std::this_thread::sleep_for(kRankInterval);
  const HintApi api = LoadHintApi();
  void* manager = api.usable() ? api.get_manager() : nullptr;
  if (!manager) {
    __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                        "performance-hint: unavailable (needs Android 13 and device support)");
    return;
  }

  void* session = nullptr;
  std::vector<int32_t> threads;
  std::vector<int64_t> reported;
  std::unordered_map<pid_t, int64_t> ranked;
  int64_t target = 0;
  uint32_t frames_seen = 0;
  uint32_t failures = 0;
  auto next_rank = std::chrono::steady_clock::now();
  auto next_log = next_rank + kLogInterval;
  std::vector<int64_t> logged_nanos;  // per session thread, since the last log line
  uint64_t logged_frames = 0;
  const auto rebase = [&] {
    reported.resize(threads.size());
    for (size_t i = 0; i < threads.size(); ++i) reported[i] = ThreadCpuNanos(threads[i]);
    frames_seen = g_presented_frames.load(std::memory_order_relaxed);
  };
  for (;;) {
    std::this_thread::sleep_for(kReportInterval);
    if (!REXCVAR_GET(android_performance_hint)) {
      if (session) {
        api.close_session(session);
        session = nullptr;
        threads.clear();
        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "performance-hint: off");
      }
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= next_rank) {
      next_rank = now + kRankInterval;
      std::vector<int32_t> busiest = BusiestThreads(ranked);
      if (!busiest.empty() && busiest != threads) {
        const bool created = !session;
        if (!session || !api.set_threads ||
            api.set_threads(session, busiest.data(), busiest.size()) != 0) {
          if (session) api.close_session(session);
          target = TargetNanos();
          session = api.create_session(manager, busiest.data(), busiest.size(), target);
        }
        if (!session) {
          // A thread that exited between ranking and here fails once; a
          // device without the hint HAL fails every time.
          threads.clear();
          if (++failures == 3) {
            __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                                "performance-hint: the device refused a session");
            return;
          }
          continue;
        }
        failures = 0;
        threads = std::move(busiest);
        rebase();
        logged_nanos.assign(threads.size(), 0);
        logged_frames = 0;
        if (created) {
          __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                              "performance-hint: session threads=%zu target=%.1fms",
                              threads.size(), double(target) / 1e6);
        }
        continue;
      }
    }
    if (!session) continue;

    const uint32_t frames = g_presented_frames.load(std::memory_order_relaxed) - frames_seen;
    if (!frames) {
      // Loading or paused: that work belongs to no frame.
      rebase();
      continue;
    }
    if (const int64_t wanted = TargetNanos(); wanted != target) {
      target = wanted;
      api.update_target(session, target);
    }
    int64_t busiest_nanos = 0;
    for (size_t i = 0; i < threads.size(); ++i) {
      const int64_t nanos = ThreadCpuNanos(threads[i]);
      busiest_nanos = std::max(busiest_nanos, nanos - reported[i]);
      logged_nanos[i] += nanos - reported[i];
      reported[i] = nanos;
    }
    frames_seen += frames;
    if (busiest_nanos > 0) api.report_actual(session, busiest_nanos / frames);

    // Every ten seconds, each session thread's CPU time per frame and the core
    // it last ran on, busiest first ("6:17.4" is 17.4 ms on cpu6): whether the
    // thread that sets the frame time reaches a fast core is not visible from
    // the clock limits alone.
    logged_frames += frames;
    if (now >= next_log) {
      next_log = now + kLogInterval;
      std::vector<std::pair<int64_t, int>> work;
      for (size_t i = 0; i < threads.size(); ++i) {
        work.emplace_back(logged_nanos[i], ThreadCore(threads[i]));
      }
      std::sort(work.rbegin(), work.rend());
      std::string text;
      for (const auto& [nanos, core] : work) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), "%s%d:%.1f", text.empty() ? "" : " ", core,
                      double(nanos) / double(logged_frames) / 1e6);
        text += entry;
      }
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                          "performance-hint: core:ms/f=%s target=%.1fms", text.c_str(),
                          double(target) / 1e6);
      logged_nanos.assign(threads.size(), 0);
      logged_frames = 0;
    }
  }
}

const bool g_hint_started = [] {
  std::thread(HintLoop).detach();
  return true;
}();

}  // namespace

// Called by the title's present hook, once per title frame.
extern "C" void liberty_android_frame_presented() {
  g_presented_frames.fetch_add(1, std::memory_order_relaxed);
}
