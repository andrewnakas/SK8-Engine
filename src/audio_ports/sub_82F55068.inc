// sub_82F55068  role: log10(f1) -- the CRT wrapper: call the natural log sub_82F54ED8, multiply its result by log10(e), leave it in f1   lifted: skate3_recomp.106.cpp:74776 (30 lines)
// STATUS: verified
// calls/session: boot 125566  play 169118  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82F55068.md
namespace port_82F55068 {

// lis r11,-32251 ; lfd f0,8368(r11) -- 0x82050000 + 8368, computed as ((imm & 0xFFFF) << 16) +
// offset, not read off. Read out of the dumped image (probe/harness/out/image/g_8200.bin) it is
// 0.4342944819032518, the correctly rounded log10(e); the body loads it rather than assuming,
// since a wrong guess would not be caught by a harness that read the same word.
constexpr uint32_t kLog10OfE = 0x82050000u + 8368;

// Read-only constants sub_82F54ED8 pulls in, declared for replay. The natural log's pool runs
// from +8344 to +8488 (sqrt(2)/2, ln2 split as 0.693359375 - 0.00021219444005469057, the rational
// coefficients, 2^53, DBL_MIN, DBL_MAX) and kLog10OfE sits inside it at +8368.
constexpr uint32_t kLogPool = 0x82050000u + 8344;
constexpr uint32_t kLogPoolBytes = 144;
constexpr uint32_t kZeroOnePool = 0x82010000u + 248;  // f64 0.0 at +248, f64 1.0 at +264
constexpr uint32_t kZeroOneBytes = 24;
constexpr uint32_t kHalf = 0x82010000u + 648;         // f64 0.5
constexpr uint32_t kInfinityPool = 0x82FB0000u + 22720;  // f64 +inf at +22720, f64 NaN at +22728
constexpr uint32_t kInfinityBytes = 16;               // both are negated: log(+0), log(x < 0)

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)

REX_FUNC(Native) {
  // stwu r1,-96(r1). Reproduced -- most ports skip their own frame -- because sub_82F54ED8 spills
  // the argument at 16(r1) and its reduced value at -16(r1) and reads both back through integer
  // loads. Without the frame that scratch would land 96 bytes higher, in the caller's parameter
  // save area, instead of where the original put it.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // bl 0x82f54ed8 -- natural log, argument and result both in f1, no GPR arguments.
  skate3::audio::GuestCall(ctx, base, sub_82F54ED8);

  // lis r11,-32251 ; lfd f0,8368(r11) ; fmul f1,f1,f0 -- a plain double multiply, NOT an FMA:
  // the lifted line is `fmul`, and there is no adjacent add for a compiler to contract with.
  ctx.fpscr.disableFlushModeUnconditional();
  PPCRegister multiplier{};
  multiplier.u64 = REX_LOAD_U64(kLog10OfE);
  ctx.f1.f64 = ctx.f1.f64 * multiplier.f64;

  ctx.r1.u32 = entry_sp;  // addi r1,r1,96
}

// No writes: every store in the closure is this frame or the callee's own scratch inside it, and
// nothing outside is touched. The result is f1 alone -- the call sites read it through `frsp`.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  spec.read(kLogPool, kLogPoolBytes);
  spec.read(kZeroOnePool, kZeroOneBytes);
  spec.read(kHalf, 8);
  spec.read(kInfinityPool, kInfinityBytes);
  return true;
}

}  // namespace port_82F55068
SKATE3_PORT(82F55068, skate3::audio::kPortVerified, skate3::audio::kReturnF1)
