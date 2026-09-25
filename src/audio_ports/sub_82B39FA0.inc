// sub_82B39FA0  role: mix dispatcher over the same descriptor as sub_82B3D4F8 -- when the word at +8 is zero, call the five-gain kernel sub_82B399D0 with three outgoing stack arguments and latch its returned f1 into the gain block's +32; otherwise just memset the +20 buffer to zero   lifted: skate3_recomp.68.cpp:905 (92 lines)
// STATUS: verified
// calls/session: boot 151710  play 179682  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B39FA0.md
namespace port_82B39FA0 {

// Arguments: r3 = the gain block, r4 = the sample count, r5 = an opaque word passed through on
// the stack, r7 = the buffer descriptor. r6 and r8-r10 are not read on entry.
//
// The gain block, read as five singles into f1-f5. +32 is also written: the kernel returns a
// value in f1 and the caller latches it back there, so +32 is running state, not a parameter.
constexpr uint32_t kGain0 = 16;  // lfs f1,16(r31) ; stfs f1,32(r31) latches here
constexpr uint32_t kGain1 = 20;  // lfs f2,20(r31)
constexpr uint32_t kGain2 = 24;  // lfs f3,24(r31)
constexpr uint32_t kGain3 = 28;  // lfs f4,28(r31)
constexpr uint32_t kGainState = 32;  // lfs f5,32(r31) and the store after the call
//
// The descriptor, the same layout sub_82B3D4F8 unpacks. Names describe the use, not a recovered
// struct -- docs/rw_audio_structs.h names none of it.
constexpr uint32_t kDescAddend = 0;   // lwz r9,0(r7)
constexpr uint32_t kDescSource = 4;   // lwz r10,4(r7)
constexpr uint32_t kDescSelect = 8;   // lwz r11,8(r7) -- zero picks the kernel
constexpr uint32_t kDescMixed = 16;   // lwz r8,16(r7) -- also stack argument 84(r1)
constexpr uint32_t kDescCopy = 20;    // lwz r11,20(r7) -- stack argument 92(r1), and the buffer
                                      //   the non-kernel path clears
constexpr uint32_t kDescBytes = 24;

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
constexpr uint32_t kLinkSlot = 8;      // stw r12,-8(r1)
constexpr uint32_t kSaveSlot = 16;     // std r31,-16(r1)
// sub_82B399D0 opens with lwz r11,100(r1) / lwz r26,92(r1) / lwz r8,84(r1) and never does its
// own stwu, so these three slots of this frame are its seventh, eighth and ninth arguments.
constexpr uint32_t kStackArgOpaque = 100;  // stw r5,100(r1)
constexpr uint32_t kStackArgCopy = 92;     // stw r11,92(r1)
constexpr uint32_t kStackArgMixed = 84;    // stw r8,84(r1)

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: round the double back to a single and store its bits.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

REX_FUNC(Native) {
  const uint32_t gains = ctx.r3.u32;     // mr r31,r3
  const uint64_t count = ctx.r4.u64;     // the kernel's r3 / the clear's element count
  const uint64_t opaque = ctx.r5.u64;    // stored to 100(r1), otherwise untouched
  // r6 is not an argument: sub_82B399D0 overwrites it with a lis constant (line 79 of its
  // lifted body) before any read. Passed through only so the callee's register state matches.
  const uint64_t passthrough_r6 = ctx.r6.u64;
  const uint32_t desc = ctx.r7.u32;

  // The frame is reproduced because it carries three outgoing stack arguments (84/92/100) that
  // sub_82B399D0 reads off it. The link slot at frame + 120 and the r31 slot at frame + 112 are
  // this call's own frame, so they are not windows; the callee's own spills land below frame.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                                  // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);      // stw r12,-8(r1)
  REX_STORE_U64(entry_sp - kSaveSlot, ctx.r31.u64);      // std r31,-16(r1) -- the entry r31
  const uint32_t frame = entry_sp - kFrameBytes;         // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t select = REX_LOAD_U32(desc + kDescSelect);  // lwz r11,8(r7)

  // cmplwi cr6,r11,0 ; bne cr6,0x82b3a010
  if (select == 0) {
    const uint32_t copy = REX_LOAD_U32(desc + kDescCopy);      // lwz r11,20(r7)
    const uint32_t mixed = REX_LOAD_U32(desc + kDescMixed);    // lwz r8,16(r7)
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f5.f64 = LoadSingle(base, gains + kGainState);         // lfs f5,32(r31)
    const uint32_t source = REX_LOAD_U32(desc + kDescSource);  // lwz r10,4(r7)
    ctx.f4.f64 = LoadSingle(base, gains + kGain3);             // lfs f4,28(r31)
    const uint32_t addend = REX_LOAD_U32(desc + kDescAddend);  // lwz r9,0(r7)
    ctx.f3.f64 = LoadSingle(base, gains + kGain2);             // lfs f3,24(r31)
    ctx.f2.f64 = LoadSingle(base, gains + kGain1);             // lfs f2,20(r31)
    REX_STORE_U32(ctx.r1.u32 + kStackArgOpaque, static_cast<uint32_t>(opaque));  // stw r5,100(r1)
    ctx.f1.f64 = LoadSingle(base, gains + kGain0);             // lfs f1,16(r31)
    REX_STORE_U32(ctx.r1.u32 + kStackArgCopy, copy);           // stw r11,92(r1)
    REX_STORE_U32(ctx.r1.u32 + kStackArgMixed, mixed);         // stw r8,84(r1)
    // bl 0x82b399d0 -- r4-r7 keep their entry values (the original never reassigns them on this
    // path), so they are passed through unchanged to match the register state exactly.
    skate3::audio::GuestCall(ctx, base, sub_82B399D0, count, count, opaque, passthrough_r6,
                             static_cast<uint64_t>(desc), mixed, addend, source);
    // stfs f1,32(r31) -- the kernel's result, latched into the gain block.
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, gains + kGainState, ctx.f1.f64);
  } else {
    // loc_82B3A010: rlwinm r5,r4,2,0,29 -- four bytes per element, the rotate's wrap masked off.
    const uint64_t length = (static_cast<uint64_t>(ctx.r4.u32) << 2) & 0xFFFFFFFCull;
    const uint32_t target = REX_LOAD_U32(desc + kDescCopy);  // lwz r3,20(r7)
    // li r4,0 ; bl 0x82f52040 -- memset(target, 0, length), through GuestCall like every callee.
    skate3::audio::GuestCall(ctx, base, sub_82F52040, static_cast<uint64_t>(target), 0ull, length);
  }

  // Both paths share this epilogue verbatim.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;                        // addi r1,r1,128
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);           // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                         // mtlr r12
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveSlot);           // ld r31,-16(r1)
}

// The kernel path's write set, mirrored from the callee's own enumeration.
//
// WARNING, and it is the load-bearing sentence of this file: the span shapes below are COPIED
// from `port_82B399D0::Windows()` in recomp/src/audio_ports/sub_82B399D0.inc. A change to that
// port's window arithmetic requires the same change here. The primitives are referenced rather
// than copied -- kPermTable, kBlockBytes, QuarterToZero, Lower, Upper all resolve into that
// namespace (it is included first in ports_68a.manifest.inc), so only the union structure is
// duplicated, and the duplicated part is annotated line by line with its counterpart.
//
// Why a copy and not a call: the callee reads `mixed`, `copy` and the clear flag off
// 84/92/100(r1), which is THIS function's frame and does not exist yet when Windows() runs. The
// three values are nonetheless known from entry state -- they are what the prologue above stores
// into those slots: desc[16], desc[20] and r5. Every other input of that enumeration is an
// argument this function passes: r3 = r4 (count), r9 = desc[0], r10 = desc[4].
//
// This does lean on `sub_82B399D0`'s port being green, and it is: zero divergence over 141,462
// comparisons with skipped=0, so its enumeration has been checked against the lifted callee far
// more thoroughly than a re-derivation from here could be.
bool KernelWindows(uint8_t* base, skate3::audio::PortSpec& spec, uint32_t desc, int32_t count,
                   uint32_t clear) {
  const uint32_t mixed = REX_LOAD_U32(desc + kDescMixed);    // -> 84(r1), the callee's kArgMixed
  const uint32_t copy = REX_LOAD_U32(desc + kDescCopy);      // -> 92(r1), the callee's kArgCopy
  const uint32_t addend = REX_LOAD_U32(desc + kDescAddend);  // -> r9
  const uint32_t source = REX_LOAD_U32(desc + kDescSource);  // -> r10
  constexpr uint32_t kBlock = port_82B399D0::kBlockBytes;

  const uint32_t span = (static_cast<uint32_t>(count) << 2) & 0xFFFFFFFCu;
  const int32_t blocks = port_82B399D0::QuarterToZero(count) - 4;
  const uint32_t iters = blocks > 0 ? ((static_cast<uint32_t>(blocks - 1) >> 2) + 1u) : 0u;
  const int64_t tail = static_cast<int64_t>(blocks) * 16;  // negative for count < 16

  // The eight-word vperm128 table the kernel rebuilds in .data on every call, at a fixed address.
  spec.write(port_82B399D0::kPermTable, port_82B399D0::kPermTableBytes);
  spec.read(port_82B399D0::kPermTable, port_82B399D0::kPermTableBytes);

  // mixed: four stvx128 at the 16-byte floor for the first block, sixteen unmasked stfs plus four
  // masked stvx128 per iteration, then sixteen stfs at mixed + 16*blocks.
  uint64_t lo = static_cast<uint64_t>(mixed) & ~0xFull;
  uint64_t hi = lo + kBlock;
  if (iters != 0) {
    lo = port_82B399D0::Lower(lo, static_cast<uint64_t>(mixed));
    hi = port_82B399D0::Upper(hi, (static_cast<uint64_t>(mixed) & ~0xFull) +
                                      kBlock * (static_cast<uint64_t>(iters) + 1));
    hi = port_82B399D0::Upper(hi,
                              static_cast<uint64_t>(mixed) + kBlock * static_cast<uint64_t>(iters));
  }
  const int64_t tail_mixed = static_cast<int64_t>(mixed) + tail;
  if (tail_mixed < 0) return false;  // count < 16 walks the tail block off the bottom of the map
  lo = port_82B399D0::Lower(lo, static_cast<uint64_t>(tail_mixed));
  hi = port_82B399D0::Upper(hi, static_cast<uint64_t>(tail_mixed) + kBlock);
  if (hi > 0x100000000ull) return false;
  spec.write(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo));
  spec.read(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo));

  // copy: four stvx128 per iteration and four for the tail block, plus -- when the flag is zero --
  // the dcbzl establish, from the 128-byte FLOOR of copy over ceil(count*4 / 128) whole lines.
  const int64_t tail_copy = static_cast<int64_t>(copy) + tail;
  if (tail_copy < 0) return false;
  uint64_t clo = static_cast<uint64_t>(tail_copy) & ~0xFull;
  uint64_t chi = clo + kBlock;
  if (iters != 0) {
    clo = port_82B399D0::Lower(clo, static_cast<uint64_t>(copy) & ~0xFull);
    chi = port_82B399D0::Upper(chi, (static_cast<uint64_t>(copy) & ~0xFull) +
                                        kBlock * static_cast<uint64_t>(iters));
  }
  if (clear == 0 && span != 0) {
    const uint64_t lines = static_cast<uint64_t>((span - 1u) / 128u) + 1u;
    const uint64_t zlo = static_cast<uint64_t>(copy) & ~127ull;
    clo = port_82B399D0::Lower(clo, zlo);
    chi = port_82B399D0::Upper(chi, zlo + 128ull * lines);
  }
  if (chi > 0x100000000ull) return false;  // the clear would wrap the address space
  spec.write(static_cast<uint32_t>(clo), static_cast<uint32_t>(chi - clo));
  spec.read(static_cast<uint32_t>(clo), static_cast<uint32_t>(chi - clo));

  // Read, never written: addend a block at a time, and source one line further because every
  // group is also read a single along.
  const uint64_t alo = static_cast<uint64_t>(addend) & ~0xFull;
  const uint64_t ahi = alo + kBlock * (static_cast<uint64_t>(iters) + 1);
  if (ahi <= 0x100000000ull) spec.read(static_cast<uint32_t>(alo), static_cast<uint32_t>(ahi - alo));
  const uint64_t slo = static_cast<uint64_t>(source) & ~0xFull;
  const uint64_t shi = slo + kBlock * (static_cast<uint64_t>(iters) + 1) + 16;
  if (shi <= 0x100000000ull) spec.read(static_cast<uint32_t>(slo), static_cast<uint32_t>(shi - slo));
  return true;
}

// Every byte the call writes, from ENTRY state only. The selector at +8 picks the path and is
// readable before the call, so both paths are decided here, not discovered.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t desc = ctx.r7.u32;
  spec.read(desc, kDescBytes);

  if (REX_LOAD_U32(desc + kDescSelect) == 0) {
    // The kernel path. Its own store is one single -- stfs f1,32(r31) -- and everything else is
    // sub_82B399D0's, enumerated by KernelWindows above from the values this function is about to
    // hand it. The five gains are read; only the fifth is written.
    const uint32_t gains = ctx.r3.u32;
    spec.read(gains + kGain0, 20);  // lfs f1..f5 at +16..+32
    spec.write(gains + kGainState, 4);
    return KernelWindows(base, spec, desc, ctx.r4.s32, ctx.r5.u32);
  }

  // The clear path: memset writes exactly its length, no overshoot (byte-align prologue, then
  // 16-byte blocks, then (len>>2)&3 words, then len&3 bytes -- 16*(n>>4) + 4*((n>>2)&3) + (n&3)
  // == n). Over 32 KB the harness's own budget check takes over and runs the original.
  const uint32_t target = REX_LOAD_U32(desc + kDescCopy);
  const uint32_t length = (ctx.r4.u32 << 2) & 0xFFFFFFFCu;
  if (static_cast<uint64_t>(target) + length > 0xFFFFFFFFull) return false;  // wraps the map
  spec.write(target, length);
  return true;
}

}  // namespace port_82B39FA0
SKATE3_PORT(82B39FA0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
