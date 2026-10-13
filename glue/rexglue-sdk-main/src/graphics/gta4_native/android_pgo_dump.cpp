// Profile-guided optimization for the gta4-native renderer, recording build only.
// Android never runs atexit for an app process, so the counters are written
// from a background thread every minute; each write replaces the file with
// the cumulative counters of this process. Pull the .profraw files from the
// app's files/pgo directory and merge them with llvm-profdata.
#if defined(LIBERTY_PGO_GENERATE)

#include <android/log.h>
#include <sys/stat.h>

#include <chrono>
#include <thread>

extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);

// Atomic replacements for the NDK profile runtime's non-atomic helpers; see
// gta4-recomp/src/android/android_pgo_dump.cpp. This library links its own
// copy of the runtime, so it needs its own pair (-Wl,--wrap in CMakeLists.txt).
extern "C" {
__attribute__((used, visibility("hidden"))) unsigned __wrap_lprofBoolCmpXchg(void** ptr, void* old_value,
                                                                             void* new_value) {
  return __atomic_compare_exchange_n(ptr, &old_value, new_value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
__attribute__((used, visibility("hidden"))) void* __wrap_lprofPtrFetchAdd(void** mem, long byte_increment) {
  return reinterpret_cast<void*>(__atomic_fetch_add(reinterpret_cast<unsigned long*>(mem),
                                                    static_cast<unsigned long>(byte_increment), __ATOMIC_SEQ_CST));
}
}

namespace {

struct PgoDumper {
  PgoDumper() {
    constexpr const char* kDirectory = "/data/data/com.libertyrecomp/files/pgo";
    mkdir(kDirectory, 0700);
    __llvm_profile_set_filename("/data/data/com.libertyrecomp/files/pgo/rexgpu-%p.profraw");
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        const int result = __llvm_profile_write_file();
        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "pgo: profile written (%d)", result);
      }
    }).detach();
  }
};

PgoDumper pgo_dumper;

}  // namespace

#endif
