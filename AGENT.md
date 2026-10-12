# Agent notes: Android performance work

Notes for an agent picking up performance work on this fork. Everything here
was measured on the one device the fork targets, on 2026-10-12. Re-measure
before relying on a number.

## Scope

- The fork only has to work on one phone: OPPO CPH2765 (SM8850, Adreno 840,
  Android 16 / ColorOS, 120 Hz display). Hardcoding for it is fine.
- The owner accepts 30 FPS but not lower image quality. Do not propose
  resolution, draw distance, density or effect cuts.
- Do not commit unless asked.

## The device

- Seven cores, none of them efficiency cores: cpu0-4 max 3.63 GHz, cpu5-6
  (prime) max 4.61 GHz.
- The phone throttles hard. In gameplay cpu5-6 are held at 2.2-2.7 GHz and
  cpu0-4 at 1.1-2.5 GHz within 30 seconds of loading the world. After several
  minutes, plugged into USB, cpu5-6 dropped to 0.77 GHz and cpu0-4 to 1.0 GHz.
- An app cannot raise these limits. ColorOS game mode and the ADPF performance
  hint session (accepted by the system) did not lift them.
- `AThermal_getCurrentThermalStatus` works: it read 3 in normal play and 4
  under the hardest throttling. `AThermal_getThermalHeadroom` stays near 0.0-0.1
  whatever the state; ignore it.
- The kgsl GPU sysfs files are not readable (`gpu-*=?` in the `thermal:` line).
- USB charging while measuring adds heat; results taken on the cable are worse
  than normal play.

## How the frame is produced

Four threads carry a frame, in sequence:

| Thread | Role | Notes |
| --- | --- | --- |
| `XThread67FF8480` (name varies per run) | title thread that submits the frame | A third of its CPU is a busy-wait, see below |
| `SDLThread` (the busiest one) | command worker | About 40% of its time is `memmove` and `memcmp` |
| `GtaRecorder` | records Vulkan commands | The real bottleneck once pacing is fixed |
| `XThread5F7F8480` | game logic | Recompiled game code only |

- The title allows two frames in flight. `sub_828BF420` polls `sub_82A46D70`
  in a loop without sleeping while two are pending. That spin is why the title
  thread always ranks as "busiest" and takes a prime core from thread pinning.
- `gta4_frame_wait_sleep=true` turns the spin into a sleep. It freed about 26%
  of a prime core with no change in frame rate. It is off by default because
  of a suspected game-side crash noted in `android_frame_wait_hook.cpp`.
- `gta4_extra_frames_in_flight` 1 or 2 made no difference to the frame rate.

## Findings

1. **Frame limiter backpressure cost frames when below the cap.** With
   `gta4_frame_limit=60` a heavy scene ran at 19.8 FPS; with the limit at 0 the
   same scene ran at 26.0 FPS. The producer waited in
   `FramePublicationGate::Wait` for the host present of each frame even when
   the frame was already late. `src/ui/presenter.cpp` now skips that wait when
   the time since the previous publication exceeds the limit's period by more
   than an eighth. **Built and installed, not yet measured on the device.**
2. **After that, `GtaRecorder` is the bottleneck**: 95% of a core, about
   36 ms per frame at the throttled clocks, in a scene of about 4,000 draws.
   It has not been profiled by function yet. That is the next step.
3. **Shader hitches are fixed.** Saved pipeline recipes with variants above 3
   were dropped on load (`Valid()` in `native_pipeline_recipe.h`), and the
   cache was saved only every 30 seconds. With both fixed, 278 of 278 recipes
   load and one new pipeline was compiled in a whole drive.
4. **Things already optimal, do not revisit:** game code is built `-O3` with
   thin LTO; index reordering for the vertex cache is done once per mesh in the
   background; guest spinlock retries are about 1,900 per second; gameplay
   logging is a few lines per second.

## Changes made in this session (uncommitted)

- `src/ui/presenter.cpp`, `include/rex/ui/presenter.h`: skip producer
  backpressure when below the frame limit.
- `gta4-recomp/src/android/android_performance_hint.cpp` (new): ADPF session
  and the `performance-hint: core:ms/f=` log line.
- `gta4-recomp/src/android/android_thread_pinning.cpp`: `gta4_thread_pinning`
  defaults to 2.
- `gta4-recomp/src/gta4_app.cpp`: `thermal:` line covers every core and adds
  `android-status/headroom`.
- `src/graphics/gta4_native/native_pipeline_recipe.h`, `graphics_system.cpp`:
  recipe variant limit, 5-second cache checkpoint, prebuild log lines.
- `src/core/threading_posix.cpp`: guard against signalling a finished thread
  on exit.

## How to measure

All scripts are in `os/android/scripts` and need the phone on adb with the
game running.

- **Change a setting live:** `live_cvar.sh --name=value`. Works for
  `gta4_frame_limit`, `gta4_frame_wait_sleep`, `gta4_extra_frames_in_flight`,
  `gta4_present_mode`. `vsync` is rejected live.
- **Frame rate:** count `pipeline: 120 frames` lines in logcat; FPS is 120
  divided by the seconds between two lines.
- **Per-thread CPU:** `cpu_frame.sh` prints nothing on this phone, because the
  Android 16 `dumpsys SurfaceFlinger --list` format no longer matches its
  layer pattern. Read `/proc/<pid>/task/*/stat` through `run-as` twice
  instead (fields 14, 15 and 39) and divide by the frame count from the log.
- **Function profile:** `simpleperf record --app com.libertyrecomp -e
  cpu-clock -f 2000 -g --duration 12` on the device, then `simpleperf report
  --tids <tid> --sort dso,symbol`. The app is profileable. Renderer symbols
  are stripped on the device; resolve addresses against
  `glue/rexglue-sdk-main/out/linux-arm64/librexgpu-gta4-native.so` with
  `llvm-addr2line`, as `rec_profile.sh` does.
- **Reading thread CPU numbers:** a thread's CPU time per frame includes any
  spinning. The title thread's figure overstates its work by about a third.

## Building

From Git Bash at the repo root:

```
STRIP=1 bash os/android/scripts/build_native.sh
bash os/android/scripts/build_apk.sh
bash os/android/scripts/install.sh
```

`install.sh` restarts the game, so the tester has to load the save and return
to the test spot afterwards.
