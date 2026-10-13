# Agent notes: Android performance work

Notes for an agent picking up performance work on this fork. Everything here
was measured on the one device the fork targets, on 2026-10-12 and 2026-10-13.
Re-measure before relying on a number.

## Scope

- The fork only has to work on one phone: OPPO CPH2765 (SM8850, Adreno 840,
  Android 16 / ColorOS, 120 Hz display). Hardcoding for it is fine.
- The owner wants a stable 30 FPS, also when the phone is hot, and does not
  accept lower image quality. Do not propose resolution, draw distance,
  density or effect cuts.
- Do not commit unless asked.

## The device

- Seven cores, none of them efficiency cores: cpu0-4 max 3.63 GHz, cpu5-6
  (prime) max 4.61 GHz.
- **The phone sets the clock limit of a cluster (`scaling_max_freq`) from that
  cluster's load, not only from heat.** A cluster with a thread near full load
  is given 2.2-2.6 GHz; a cluster without one is held at 1.0-1.4 GHz. Seen
  three ways:
  - Title thread spinning on cpu0 (see below): cpu0-4 limited to 2.5-2.6 GHz.
    The same thread sleeping instead: 1.2-1.4 GHz, within four seconds.
  - Frame limit 60 at thermal status 4 and 36 degrees C: the recorder ran at
    94% of a core and cpu0-4 were limited to 2.2 GHz. Frame limit 30 in the
    same minute, nothing at full load: 1.2 GHz.
  - The cluster without the loaded thread stays low: prime at 1.0 GHz while
    cpu0-4 had 2.5 GHz, and the reverse in older builds that pinned to prime.
- An app cannot raise these limits directly. ColorOS game mode and the ADPF
  performance hint session (accepted by the system) did not lift them: the
  session reported 25-31 ms against a 26.7 ms target while the limits stayed
  at 1.2-1.4 GHz.
- **The clock floor**: under the hardest throttling cpu0-4 are held at
  0.998 GHz and cpu5-6 at 0.77 GHz whatever the load. It was reached at a
  battery temperature of about 39.6 degrees C, after about 12 minutes of play
  and profiling on USB (from 31.8 degrees C and thermal status 1).
- `AThermal_getCurrentThermalStatus` works, but **status 4 is not the clock
  floor**: status 4 was seen both at the floor and with a cluster at 2.2 GHz.
  Decide from the clock limits, not from the status.
  `AThermal_getThermalHeadroom` stays near 0.0-0.1 whatever the state; ignore
  it.
- The kgsl GPU sysfs files are not readable (`gpu-*=?` in the `thermal:` line),
  so the GPU's load and clock are unknown.
- USB charging while measuring adds heat; results taken on the cable are worse
  than normal play.

## How the frame is produced

| Thread | Role | Share of a core in gameplay |
| --- | --- | --- |
| `XThread...` (address varies per run; the busiest thread) | title thread that submits the frame | 86%, of which 27% is a busy-wait |
| `SDLThread` (`RenderWorkerMain`) | command worker: assembles the frame | 30-45% |
| `SDLThread` (a second one) | retires commands (`NativeCommandPool::RetireBatch`) | about 20% |
| `GtaRecorder` | records the Vulkan commands and presents | 55-95%; sets the frame time |
| another `XThread...` | game logic | 30-55% |
| `Main XThread` | | 40-50% |

About 3.85 cores are busy in total. Smaller ones: `XMA Decoder` about 20%,
three `XThread`s at about 20% each.

- **`GtaRecorder` is the bottleneck.** The worker waits for it
  (`handoff-wait=` in the `pipeline:` line is 15-27 ms per frame when the game
  is below 30 FPS). Its cost per frame at about 3,000 draws, before
  profile-guided optimization: 23.5-26.5 ms at 2.2-2.5 GHz, 34-41 ms at the
  clock floor. After it: 17.7-20 ms at 2.2 GHz, 29.6 ms at 1.2 GHz.
- Its profile is flat, and almost all of it is this project's code. Inclusive
  shares of the recorder thread (2026-10-13, before PGO):
  `RecordNativeFrame` 78%, in it `RecordIndexedPrimitive` 50%, in that
  `BindCommonDrawState` 24%, `GetOrCreateDrawPipeline` 6.5%,
  `UploadBufferResource` 5.5%, `GetOrCreateFrameConstantBuffer` 5.5%;
  `PrepareFrameTextures` 13%; `ResolveRenderingTarget` 8%;
  `GetOrCreateSurfaceImage` 6%. The largest self time of any function is 5.9%.
  XXH3 hashing is about 5.5% in total. The Turnip driver is about a tenth
  (`tu_CmdDrawIndexed` 3.4%, `tu_CmdBindVertexBuffers2` 2.1%): another driver
  would not change the picture.
- The title allows two frames in flight. `sub_828BF420` polls `sub_82A46D70`
  in a loop without sleeping while two are pending. **That spin is what holds
  the clock limit up**, see "The device". `android_frame_wait_hook.cpp` keeps
  it and sleeps only at the clock floor:
  - below the floor (thermal status 2): spinning 29.9 and 30.0 FPS, sleeping
    27.4 and 21.8 FPS (alternated live, 15 seconds each, about 2,800 draws);
  - at the floor: spinning 24.0 and 24.4 FPS, sleeping 23.9 and 24.7 FPS, and
    the title thread fell from 92% to 67% of a core.
  `gta4_frame_wait_sleep=true` forces the sleep (cooler, lower frame rate).
  `gta4_frame_wait_spin_percent` (default 100, live) spins for that share of
  each millisecond of the wait; **untested**, it is there to find the least
  load that still holds the clocks.
- **Thread pinning** (`android_thread_pinning.cpp`, `gta4_thread_pinning=2`):
  every two seconds the recorder (found by its name) and the busiest other
  thread, which is the spinning title thread, get a core each on the cluster
  with the highest current clock limit; the other threads share the rest. The
  two must share a cluster so that the spin raises the recorder's clock. At
  the clock floor nothing is pinned: the same scene ran at 21.5, 23.9 and
  21.8 FPS with pinning 2, 0 and 2. **The current rule (recorder first, floor
  by clock limit) is built and installed but not measured.** Before it, the
  ranking was by CPU time alone and the cores were ranked by hardware maximum,
  which put the two busiest threads on the prime pair even when that was the
  slowest cluster (0.77 GHz against 1.11 GHz).
- `gta4_extra_frames_in_flight` 1 or 2 made no difference to the frame rate.

## What the screen shows against what the game produces

The presenter repaints at the frame limit and repeats the last image when the
game has no new one. SurfaceFlinger showed the game's layer presented exactly
every 33.3 ms (30.1 per second) while the game produced 21.8 new frames per
second. With the limit at 60 an on-screen counter reads 60 for the same
reason. **Repeated frames at an uneven cadence are the stutter the owner
sees**; the cure is production above the limit, not presentation.

- New frames: `fps` in `ab_cvars.sh`, or 120 divided by the seconds between
  two `pipeline: 120 frames` lines.
- Presents: `present_intervals.sh`.

## Findings

1. **Profile-guided optimization is worth about 20-25% on the recorder** and
   was not in use: the machinery existed (`LIBERTY_ANDROID_PGO`), but no
   profile. With a profile recorded in about five minutes of play, the scene
   that produced 21-25 new frames per second gave 30.9 at thermal status 3
   and 46.8 with the limit at 60 (cpu0-4 at 2.2 GHz). See "Building".
2. **The 30 FPS limit works against itself on this phone.** At the limit no
   thread is at full load, the phone lowers the clocks, frames miss, and the
   limiter's waits keep the threads from looking busy. With the limit at 60
   the recorder is at full load and gets 2.2 GHz. The title thread's spin is
   the only thing that counters this at 30.
3. **Frame limiter backpressure cost frames when below the cap.** The producer
   waited in `FramePublicationGate::Wait` for the host present of each frame
   even when the frame was already late. `src/ui/presenter.cpp` skips that
   wait when the time since the previous publication exceeds the limit's
   period by more than an eighth.
4. **Shader hitches are fixed.** Saved pipeline recipes with variants above 3
   were dropped on load, and the cache was saved only every 30 seconds. All
   325 recipes load now and one new pipeline was compiled in a whole drive.
5. **Things already optimal, do not revisit:** the game code and the renderer
   are built `-O3` with thin LTO; index reordering for the vertex cache is
   done once per mesh in the background; guest spinlock retries are about
   1,900 per second; gameplay logging is a few lines per second.

## Open

- Measure the installed build (PGO, recorder-first pinning, sleep at the clock
  floor) at thermal status 4 and at the clock floor. Before PGO the floor gave
  21-24 FPS; the recorder needs about 33 ms per frame there to hold 30.
- If that is not enough: the recorder is one thread on a phone with spare
  cores (3.85 of 7 busy). The shadow map pass is 1,500-2,300 of the 3,000-
  5,500 draws of a frame (`c0x0/d1280x1280` in the `gta4-draw-census` line)
  and a candidate for recording on a second thread into its own command
  buffer. `PrepareFrameTextures` (13%) is a candidate for moving off the
  recorder.
- A way to hold the clocks at the 30 FPS limit that costs less than a spinning
  thread (`gta4_frame_wait_spin_percent` is the first thing to try).

## How to measure

All scripts are in `os/android/scripts`, run from Git Bash, and need the phone
on adb with the game running. On this machine `bash` on PATH is the WSL
launcher; Git Bash is `%USERPROFILE%\scoop\apps\git\current\bin\bash.exe`.

- **Change a setting live:** `live_cvar.sh --name=value`. Works for
  `gta4_frame_limit`, `gta4_frame_wait_sleep`, `gta4_thread_pinning`,
  `gta4_extra_frames_in_flight`, `gta4_present_mode`. `vsync` is rejected
  live. The file `files/live_cvars.txt` stays on the device: delete it after a
  test.
- **Compare settings:** `ab_cvars.sh 15 "a=--x=1" "b=--x=2" "a=--x=1"`. Always
  repeat the first configuration: the thermal state drifts within a minute
  and moves the clocks, and a single A/B cannot tell that from the setting.
- **Presents:** `present_intervals.sh`.
- **Function profile:** `profile.sh syms` once per build, then
  `profile.sh record 10`, `profile.sh threads`, `profile.sh incl GtaRecorder`.
  It gives function names for the stripped libraries on the device.
- `ab_cvars.sh`, `present_intervals.sh` and `profile.sh` were used in this
  form from a scratch folder and then moved here with `env.sh`; **the moved
  versions have not been run**.
- **Per-thread CPU:** `adb shell top -H -b -n 2 -d 10 -p <pid>` (what
  `ab_cvars.sh` uses). `cpu_frame.sh` prints nothing on this phone: Android
  16's `dumpsys SurfaceFlinger --list` format no longer matches its layer
  pattern. `frame_timeline.sh` uses the same pattern and was not tried;
  `present_intervals.sh` has the new one.
- **Reading thread CPU numbers:** a thread's CPU time per frame includes any
  spinning, and its milliseconds depend on the clock limit of the moment.
  Compare runs only at the same `cpu-max-khz` in the `thermal:` line.
- Measuring heats the phone: `simpleperf` and USB charging took it from
  thermal status 1 to the clock floor in about 12 minutes.

## Building

`build_android.bat` at the repo root builds a stripped APK and installs it,
from cmd or PowerShell. It finds Git Bash and Python by itself and downloads a
JDK 21 into `out/host-tools` (Android Studio's bundled JDK 25 does not run
Gradle 8.11). `--no-install` only builds.

**Profile-guided optimization.** The profile is `out/pgo/liberty.profdata`. It
is not in the repository (`out/` is ignored): a fresh clone builds without it
and the recorder is about 20-25% slower. `build_android.bat` uses the profile
when the file exists, in the build folder `out/build/android-arm64-pgo`. To
record one (also after larger changes to the renderer or the game code):

```
BUILD_DIR=$PWD/out/build/android-arm64-pgogen PGO=generate STRIP=1 bash os/android/scripts/build_native.sh
bash os/android/scripts/build_apk.sh
bash os/android/scripts/install.sh debug --no-launch
bash os/android/scripts/pgo.sh --clear
```

Start the game and play for about five minutes through typical scenes (the
recording build is slower), leave it running, then:

```
bash os/android/scripts/pgo.sh
```

and build again with `build_android.bat`.

By hand, from Git Bash at the repo root:

```
STRIP=1 bash os/android/scripts/build_native.sh
bash os/android/scripts/build_apk.sh
bash os/android/scripts/install.sh
```

`install.sh` restarts the game, so the tester has to load the save and return
to the test spot afterwards. The NDK is r30 (30.0.16248370).
