// sub_82B43AF8  role: a biquad over a run of singles -- y[k] = x[k]*b0 + x[k-1]*b1 + x[k-2]*b2 + bias - y[k-1]*a1 - y[k-2]*a2, eight samples per pass with the four-single history in r3; a count that is not a nonzero multiple of eight is handed to sub_82B43978 unchanged   lifted: skate3_recomp.68.cpp:25296 (278 lines)
// STATUS: verified
// calls/session: boot 1152252  play 1592578  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B43AF8.md
namespace port_82B43AF8 {

// Arguments, read straight off the lifted entry: r3 the history, r4 the destination run, r5 the
// source run, r6 five coefficients, r7 the sample count. docs/rw_audio_structs.h names none of
// this, so there are no field names beyond what the code does with each slot.
constexpr uint32_t kHistoryX1 = 0;   // lfs f7,0(r3)   x[k-1] carried in, x[7] carried out
constexpr uint32_t kHistoryX2 = 4;   // lfs f4,4(r3)   x[k-2] carried in, x[6] carried out
constexpr uint32_t kHistoryY1 = 8;   // lfs f6,8(r3)   y[k-1]
constexpr uint32_t kHistoryY2 = 12;  // lfs f3,12(r3)  y[k-2]
constexpr uint32_t kCoeffA1 = 0;     // lfs f0,0(r6)   scales y[k-1]; subtracted
constexpr uint32_t kCoeffA2 = 4;     // lfs f13,4(r6)  scales y[k-2]; subtracted
constexpr uint32_t kCoeffB0 = 8;     // lfs f12,8(r6)  scales x[k]
constexpr uint32_t kCoeffB1 = 12;    // lfs f11,12(r6) scales x[k-1]
constexpr uint32_t kCoeffB2 = 16;    // lfs f10,16(r6) scales x[k-2]

constexpr uint32_t kFrameBytes = 192;   // stwu r1,-192(r1)
constexpr uint32_t kBlockSamples = 8;   // the unrolled pass; the entry test is r7 % 8
constexpr uint32_t kBlockBytes = 4 * kBlockSamples;

// lis -32208 ; addi -31232 -> 0x822F8600, the pool sub_82B2FEA8 names kPool. Computed as
// ((imm & 0xFFFF) << 16) + offset, never read off by eye: one misread digit in sub_82B2FE00 cost
// this project's first shadow divergence.
constexpr uint32_t kPool = 0x822F8600;
constexpr uint32_t kDenormBias = kPool + 432;  // lfs f9,432(r8) -- added to every feed-forward sum
constexpr uint32_t kWindowCap = 32 * 1024;     // skate3::audio::kPortWatchCap

// lfs: the word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: the double rounded back to single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// Operand order is kept exactly as the lifted lines have it: float multiply and add are not
// NaN-commutative, and which operand's payload survives cannot be derived from source
// (docs/vmx128-exactness.md rule 4). Every one rounds to single, once.
inline double MulSingle(double a, double b) {  // fmuls
  return static_cast<double>(static_cast<float>(a * b));
}
inline double AddSingle(double a, double b) {  // fadds
  return static_cast<double>(static_cast<float>(a + b));
}
inline double FmaSingle(double a, double b, double c) {  // fmadds
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}
inline double NmsubSingle(double a, double b, double c) {  // fnmsubs: c - a*b, fused
  return static_cast<double>(static_cast<float>(-std::fma(a, b, -c)));
}

REX_FUNC(Native) {
  const uint32_t history = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const uint32_t in = ctx.r5.u32;
  const uint32_t coeffs = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;

  // stw r12,-8(r1) ; __savefpr_20 ; stwu r1,-192(r1). Native spills neither the link register nor
  // f20-f31 -- all of its state is in C++ locals -- so the back chain is the only thing it writes
  // in the frame, and the frame is this call's own and never windowed. r1 still has to move:
  // sub_82B43978 spills f31 at -8(r1), and it has to land below this frame, where the original
  // put it.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);  // stwu r1,-192(r1)
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // clrlwi r11,r7,29 ; bne, then cmplwi cr6,r7,0 ; beq -- anything but a nonzero multiple of
  // eight is handed to the generic routine with r3..r7 exactly as they arrived, all 64 bits of
  // each, since only the low 32 of any of them are ever used downstream.
  if ((count & 7u) != 0 || count == 0) {
    skate3::audio::GuestCall(ctx, base, sub_82B43978, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64,
                             ctx.r6.u64, ctx.r7.u64);  // bl 0x82b43978
    ctx.r1.u32 = entry_sp;                             // addi r1,r1,192
    return;
  }

  // rlwinm r11,r7,2,0,29 ; add r11,r11,r5 -- the byte span, and one past the last input word.
  const uint32_t span = (count << 2) & 0xFFFFFFFCu;
  const uint32_t in_end = span + in;

  // Every load below happens before the branch in the original, and before any store on either
  // side of it.
  ctx.fpscr.disableFlushModeUnconditional();
  const double a1 = LoadSingle(base, coeffs + kCoeffA1);   // lfs f0,0(r6)
  const double a2 = LoadSingle(base, coeffs + kCoeffA2);   // lfs f13,4(r6)
  const double b0 = LoadSingle(base, coeffs + kCoeffB0);   // lfs f12,8(r6)
  const double b1 = LoadSingle(base, coeffs + kCoeffB1);   // lfs f11,12(r6)
  const double b2 = LoadSingle(base, coeffs + kCoeffB2);   // lfs f10,16(r6)
  double x1 = LoadSingle(base, history + kHistoryX1);      // lfs f7,0(r3)
  double x2 = LoadSingle(base, history + kHistoryX2);      // lfs f4,4(r3)
  double y1 = LoadSingle(base, history + kHistoryY1);      // lfs f6,8(r3)
  double y2 = LoadSingle(base, history + kHistoryY2);      // lfs f3,12(r3)

  // cmplw cr6,r5,r11 ; bge 0x82b43c8c -- a span that wraps the address space skips the whole
  // loop and writes the history back untouched. It cannot be empty here: the count is a nonzero
  // multiple of eight.
  if (in < in_end) {
    // addi r9,r11,-1 ; rlwinm r9,r9,27,5,31 ; addi r9,r9,1 ; mtctr r9
    const uint32_t blocks = ((span - 1) >> 5) + 1;
    const double bias = LoadSingle(base, kDenormBias);  // lfs f9,432(r8)
    uint32_t src = in;   // addi r11,r5,-4, then read through +4(r11) .. lfsu 32(r11)
    uint32_t dst = out;  // addi r10,r4,-4, then written through +4(r10) .. stfsu 32(r10)
    for (uint32_t block = 0; block < blocks; block++) {
      ctx.fpscr.disableFlushModeUnconditional();
      // All eight inputs are read before any output is written, which is what lets a caller run
      // this in place (r4 == r5). The order is kept for that reason, not for the values.
      double x[kBlockSamples];
      for (uint32_t k = 0; k < kBlockSamples; k++) {
        x[k] = LoadSingle(base, src + 4 * k);  // lfs f2,4(r11) .. lfsu f5,32(r11)
      }
      src += kBlockBytes;

      // The feed-forward half: fmuls, two fmadds, fadds per sample. The first two samples reach
      // into the history and round their three taps in a different order from the other six, so
      // they are spelled out rather than folded into the loop.
      double t[kBlockSamples];
      t[0] = AddSingle(FmaSingle(x1, b1, FmaSingle(x2, b2, MulSingle(x[0], b0))), bias);
      t[1] = AddSingle(FmaSingle(x1, b2, FmaSingle(x[0], b1, MulSingle(x[1], b0))), bias);
      for (uint32_t k = 2; k < kBlockSamples; k++) {
        t[k] = AddSingle(
            FmaSingle(x[k], b0, FmaSingle(x[k - 2], b2, MulSingle(x[k - 1], b1))), bias);
      }

      // The recursive half: two fnmsubs and a store per sample, in order -- each output is the
      // next sample's y[k-1], so this chain cannot be reassociated.
      for (uint32_t k = 0; k < kBlockSamples; k++) {
        const double y = NmsubSingle(y2, a2, NmsubSingle(y1, a1, t[k]));  // fnmsubs ; fnmsubs
        StoreSingle(base, dst + 4 * k, y);                                // stfs / stfsu
        y2 = y1;
        y1 = y;
      }
      dst += kBlockBytes;
      x2 = x[kBlockSamples - 2];  // fmr f4,f8 -- x[6] becomes the next pass's x[k-2]
      x1 = x[kBlockSamples - 1];  // fmr f7,f5 -- x[7] becomes its x[k-1]
    }
  }

  // loc_82B43C8C: the history written back. When the loop did not run, these are the four values
  // just loaded, stored back through the same single round trip -- a no-op in value, still a
  // store, so it is reproduced.
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, history + kHistoryX1, x1);  // stfs f7,0(r3)
  StoreSingle(base, history + kHistoryX2, x2);  // stfs f4,4(r3)
  StoreSingle(base, history + kHistoryY1, y1);  // stfs f6,8(r3)
  StoreSingle(base, history + kHistoryY2, y2);  // stfs f3,12(r3)
  ctx.r1.u32 = entry_sp;                        // addi r1,r1,192
}

// Everything this call writes, from entry state: the four-single history and the destination run.
// Both branches write both -- sub_82B43978 has the same write set, [r4, r4 + 4*r7) through its
// own four-at-a-time loop and remainder loop, and the same four stores at r3 -- so the window set
// does not depend on which one runs. Neither side writes anything else: the only other stores in
// either body are the frames (the back chain here, f31 at -8(r1) there).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t history = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const uint32_t in = ctx.r5.u32;
  const uint32_t coeffs = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;
  const uint32_t span = (count << 2) & 0xFFFFFFFCu;  // rlwinm r11,r7,2,0,29

  // Read and written unconditionally on both paths.
  if (history == 0 || coeffs == 0) return false;
  if (span != 0) {
    if (out == 0 || in == 0) return false;
    // The loop's exit test is a pointer compare, so a run that wraps the address space either
    // never terminates or writes a set no window can describe.
    if (out + span <= out || in + span <= in) return false;
    if (static_cast<uint64_t>(span) + 16 > kWindowCap) return false;  // past the harness budget
  }
  spec.write(history, 16);
  spec.write(out, span);
  spec.read(history, 16);
  spec.read(coeffs, 20);
  spec.read(in, span);
  spec.read(kDenormBias, 4);
  return true;
}

}  // namespace port_82B43AF8
SKATE3_PORT(82B43AF8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
