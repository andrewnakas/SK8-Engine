// How much simulation the title may advance in one frame.
//
// Skate 3 advances EXACTLY ONE refresh period of simulation per rendered
// frame, however long that frame actually took, so its speed is
// period / real_frame_time. Its own D3D layer (sub_82B82720) measures real
// elapsed time correctly, expresses it in hundredths of one period, and then
// clamps it at 100:
//
//     r7 = (elapsed_ticks * 100) / (freq / refresh) + 1;
//     if (r7 > 100) r7 = 100;          // one period
//
// Measured on Switch, and the fit is exact: DM Jumpline at p50 = 16.7 ms felt
// right, the career world at p50 = 32-39 ms felt like half speed, and
// 16.7/p50 predicts 43-52%. The irregularity is the character stutter -
// every frame that overruns the period loses a different amount of time.
//
// Both the refresh and the frequency cancel out of the result, so telling the
// title the display is 30 Hz only moved the ceiling from 16.7 ms to 33.3 ms:
// windows that held 30 fps came out at 100% speed and windows at 8-20 fps
// still came out at 27-67%. The clamp is the whole problem.
//
// The cap stays, because removing it is its own bug: a 3-second load stall
// would advance three seconds of skating physics in a single step and put the
// skater through the world. What it should not do is bind during ordinary
// play. The default here is six periods - 100 ms at 60 Hz - which covers the
// p95 of every window measured on this console while still truncating a
// genuine hitch.
//
// The guest side of this is scripts/patch_frame_advance_cap.py; the generated
// tree is shared with the phone builds and compiles a plain 100u there.

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DEFINE_UINT32(
    skate3_frame_advance_cap, 600, "Skate 3",
    "Most simulation the game may advance in one frame, in hundredths of one "
    "display refresh period. 100 is one period and is what the title shipped "
    "with - it is why a frame longer than a period runs the game in slow "
    "motion, because the game measures real elapsed time and then throws away "
    "everything past one period. 600 lets a frame up to six periods long (100 "
    "ms at 60 Hz) advance real time. Raise it if slow motion survives a "
    "frame-rate dip; lower it towards 100 if a hitch throws the skater "
    "through the world.");

extern "C" unsigned int Skate3GuestFrameAdvanceCap(void) {
  // Never below 100: under one period would run the game slower than the
  // stock title everywhere, which is the opposite of the point.
  return std::max<uint32_t>(100u, REXCVAR_GET(skate3_frame_advance_cap));
}
