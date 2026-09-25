// sub_82B38B68  role: dispatch one allpass stage: run it, or clear its accumulator instead   lifted: skate3_recomp.67.cpp:65117 (46 lines)
// STATUS: verified
// calls/session: boot 25285  play 29947  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B38B68.md
namespace port_82B38B68 {

// The stage descriptor in r7. Nothing in docs/rw_audio_structs.h names it; the letters match
// port_82B389A0's naming of the same four buffers.
constexpr uint32_t kTapSource = 0;     // lwz r6,0(r11)  -> the callee's c
constexpr uint32_t kTapDelay = 4;      // lwz r7,4(r7)   -> the callee's t, read unaligned
constexpr uint32_t kBypass = 8;        // lwz r9,8(r7)   nonzero: clear the accumulator instead
constexpr uint32_t kStageOut = 16;     // lwz r8,16(r7)  -> the callee's d
constexpr uint32_t kAccumulator = 20;  // lwz r9,20(r7)  -> the callee's acc, and the clear target

// The coefficient pair in r3.
constexpr uint32_t kTapGain = 16;    // lfs f1,16(r6) -- the callee's a
constexpr uint32_t kStageGain = 20;  // lfs f2,20(r3) -- the callee's g

// port_82B389A0's own guard, carried over: it writes 12 bytes per sample, so the harness budget
// rejects anything past about 2700 samples long before this matters.
constexpr int32_t kMaxEnumerableSamples = 0x01000000;

REX_FUNC(Native) {
  const uint64_t coeffs = ctx.r3.u64;  // mr r6,r3
  const uint64_t count = ctx.r4.u64;
  const uint64_t primed = ctx.r5.u64;  // mr r10,r5
  const uint64_t stage = ctx.r7.u64;   // mr r11,r7
  const uint64_t bypass = REX_LOAD_U32(uint32_t(stage) + kBypass);  // lwz r9,8(r7)
  PPCRegister temp{};

  // cmplwi cr6,r9,0 ; bne cr6,0x82b38ba0 -- an unsigned compare of the low word.
  if (uint32_t(bypass) == 0) {
    ctx.fpscr.disableFlushModeUnconditional();
    temp.u32 = REX_LOAD_U32(uint32_t(coeffs) + kStageGain);  // lfs f2,20(r3)
    ctx.f2.f64 = double(temp.f32);
    const uint64_t acc = REX_LOAD_U32(uint32_t(stage) + kAccumulator);   // lwz r9,20(r7)
    temp.u32 = REX_LOAD_U32(uint32_t(coeffs) + kTapGain);                // lfs f1,16(r6)
    ctx.f1.f64 = double(temp.f32);
    const uint64_t out = REX_LOAD_U32(uint32_t(stage) + kStageOut);      // lwz r8,16(r7)
    const uint64_t delay = REX_LOAD_U32(uint32_t(stage) + kTapDelay);    // lwz r7,4(r7)
    const uint64_t source = REX_LOAD_U32(uint32_t(stage) + kTapSource);  // lwz r6,0(r11)

    // b 0x82b389a0 -- a tail branch, so LR still points at our own caller. r11 is not an argument
    // register and GuestCall does not carry it, but the original enters the callee with r11 still
    // holding the descriptor, so set it rather than leave something else behind.
    ctx.r11.u64 = stage;
    skate3::audio::GuestCall(ctx, base, sub_82B389A0, count /*r3 = mr r3,r4*/, count /*r4 kept*/,
                             primed /*r5 kept*/, source, delay, out, acc, primed /*r10*/);
    return;
  }

  // loc_82B38BA0: memset(stage->accumulator, 0, count * 4).
  // rlwinm r5,r4,2,0,29 -- a 32-bit rotate whose low two bits the mask then clears.
  const uint64_t bytes = __builtin_rotateleft32(uint32_t(count), 2) & 0xFFFFFFFCu;
  const uint64_t target = REX_LOAD_U32(uint32_t(stage) + kAccumulator);  // lwz r3,20(r11)
  ctx.r11.u64 = stage;  // mr r11,r7, still live at the branch
  // b 0x82f52040 -- memset, also a tail branch. It reads only r3, r4 and r5; r6..r10 are handed
  // over as the original leaves them so the two entries are the same machine state.
  skate3::audio::GuestCall(ctx, base, sub_82F52040, target, uint64_t(0) /*li r4,0*/, bytes,
                           coeffs /*r6 = mr r6,r3*/, stage /*r7 untouched here*/, ctx.r8.u64,
                           bypass /*r9*/, primed /*r10*/);
}

// port_82B389A0's write set, restated for the arguments this function hands it. That port is
// verified; the derivation of these expressions lives in its note, not here.
bool StageWindows(skate3::audio::PortSpec& spec, int32_t n, int32_t primed, uint32_t source,
                  uint32_t delay, uint32_t out, uint32_t acc) {
  if (n < 0) return false;  // the dcbzl clear would run over gigabytes
  if (n == 0) return true;  // both loops take their early exit: no writes at all
  if (n > kMaxEnumerableSamples) return false;
  if (primed == 0) {
    const uint32_t pass_bytes = (static_cast<uint32_t>(n) << 2) & 0xFFFFFFFCu;
    const uint32_t lines = (pass_bytes + 127) / 128;
    spec.write(acc & ~127u, lines * 128);  // dcbzl, whole cache lines
  }
  const uint32_t quads = static_cast<uint32_t>(n / 4);
  if (quads != 0) {
    const uint32_t bytes = (((quads - 1) >> 2) + 1) * 64;
    spec.write(out & ~0xFu, bytes);
    spec.write(acc & ~0xFu, bytes);
    spec.read(source & ~0xFu, bytes);
    spec.read(acc & ~0xFu, bytes);
    spec.read(delay & ~0xFu, bytes + 16);  // the unaligned tap reaches one line past the run
  }
  return true;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t stage = ctx.r7.u32;
  const uint32_t coeffs = ctx.r3.u32;
  if (stage == 0) return false;  // the original loads through it unconditionally
  spec.read(stage, 24);          // +0, +4, +8, +16, +20

  if (REX_LOAD_U32(stage + kBypass) != 0) {
    // sub_82F52040 is memset (skate3_recomp.106.cpp:64780): it writes exactly [r3, r3 + r5),
    // nothing else, and r5 == 0 writes nothing.
    const uint32_t target = REX_LOAD_U32(stage + kAccumulator);
    const uint32_t bytes = __builtin_rotateleft32(ctx.r4.u32, 2) & 0xFFFFFFFCu;
    if (target + bytes < target) return false;  // would wrap the address space
    spec.write(target, bytes);
    return true;
  }

  if (coeffs == 0) return false;  // lfs f2,20(r3) would load through null
  spec.read(coeffs + kTapGain, 8);
  return StageWindows(spec, ctx.r4.s32, ctx.r5.s32, REX_LOAD_U32(stage + kTapSource),
                      REX_LOAD_U32(stage + kTapDelay), REX_LOAD_U32(stage + kStageOut),
                      REX_LOAD_U32(stage + kAccumulator));
}

}  // namespace port_82B38B68
SKATE3_PORT(82B38B68, skate3::audio::kPortVerified,
            skate3::audio::Gpr(3) | skate3::audio::Vr(2))
