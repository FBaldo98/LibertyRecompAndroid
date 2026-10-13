// sub_82A46D70(device r3, out r4) writes {0, submitted frame, submitted -
// completed} for the device. Its only caller, sub_828BF420, polls it in a
// loop without sleeping while two frames are pending, i.e. while the title
// waits for the renderer to submit a frame: a busy-wait that held a big core
// at ~7% of the main game thread's samples. Sleep briefly when the title is
// about to spin; the game's own loop still decides when to stop waiting.
//
// The same two-frame limit caps throughput: one frame passes the command
// worker, the recorder and the GPU in sequence (~75 ms at the bridge), so two
// in flight give ~38 ms per frame while every stage has slack. Reporting
// fewer pending frames lets the title run that many frames further ahead; the
// renderer's own queue still bounds the total.
#include <time.h>

#include <rex/cvar.h>

#include "gta4_init.h"

// The spin is a quarter of a core (27% of the title thread's samples), but on
// an OPPO CPH2765 it is also what keeps the clocks up: the phone sets the
// clock limit of a cluster from its load, and without a thread near full load
// it held cpu0-4 at 1.2-1.4 GHz instead of 2.5 GHz. Measured on 2026-10-13 in
// one scene, alternating the setting live:
//   thermal status 2: spinning 29.9 and 30.0 FPS, sleeping 27.4 and 21.8 FPS
//   thermal status 4: spinning 24.0 and 24.4 FPS, sleeping 23.9 and 24.7 FPS
// In that second measurement every core was at its clock floor (cpu0-4 at
// 0.998 GHz, cpu5-6 at 0.77 GHz) whatever the load, so the spin bought nothing
// and only added heat. Status 4 alone is not that state: at status 4 and
// 36 degrees C, sleeping let cpu0-4 fall to 1.2 GHz and the game to 24.5 FPS.
// The hook therefore sleeps at the clock floor, not at a thermal status.
//
// Always sleeping is therefore off by default. It was off before for the
// game-code crash in sub_8296E480 (stale object pointer), which first appeared
// in builds that had it: changing the main thread's timing can expose
// game-side races. gta4_stale_object_guard covers that crash now.
REXCVAR_DEFINE_BOOL(gta4_frame_wait_sleep, false, "GTA IV/Performance",
                    "Always sleep instead of spinning while the title waits for a frame slot "
                    "(cooler, but a phone that clocks by load then drops below the frame limit)");
REXCVAR_DEFINE_BOOL(gta4_frame_wait_sleep_throttled, true, "GTA IV/Performance",
                    "Sleep instead of spinning while every core is held at its clock floor");
// For finding the least load that still holds the clocks: untested.
REXCVAR_DEFINE_UINT32(gta4_frame_wait_spin_percent, 100, "GTA IV/Performance",
                      "Share of the wait for a frame slot that is spent spinning; the rest sleeps")
    .range(0, 100);
REXCVAR_DEFINE_UINT32(gta4_extra_frames_in_flight, 0, "GTA IV/Performance",
                      "Frames the title may run ahead beyond its own limit of two pending "
                      "frames (more throughput, one frame of input latency each)")
    .range(0, 2);

extern "C" bool liberty_android_clock_floor();

extern "C" void sub_82A46D70(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82A46D70(ctx, base);
  uint32_t pending = REX_LOAD_U32(out + 8);
  if (pending >= 0x80000000u) return;
  const uint32_t extra = REXCVAR_GET(gta4_extra_frames_in_flight);
  if (extra) {
    pending = pending > extra ? pending - extra : 0;
    REX_STORE_U32(out + 8, pending);
  }
  if (pending < 2) return;
  long sleep_nanos = 0;
  if (REXCVAR_GET(gta4_frame_wait_sleep) ||
      (REXCVAR_GET(gta4_frame_wait_sleep_throttled) && liberty_android_clock_floor())) {
    sleep_nanos = 250 * 1000;
  } else if (const uint32_t spin = REXCVAR_GET(gta4_frame_wait_spin_percent); spin < 100) {
    // Each millisecond starts with the sleeping share and ends with the spin.
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long phase = now.tv_nsec % 1'000'000;
    const long sleeping = long(100 - spin) * 10'000;
    if (phase < sleeping) sleep_nanos = sleeping - phase;
  }
  if (sleep_nanos) {
    const timespec delay{0, sleep_nanos};
    nanosleep(&delay, nullptr);
  }
}
