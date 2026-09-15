// sub_82B46810  role: run the (in-channels -> out-channels) downmix program -- for each routing byte, ramp-mix one source channel into one destination channel   lifted: skate3_recomp.68.cpp:32239 (106 lines)
// STATUS: verified
// calls/session: boot 20586  play 26356  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B46810.md
namespace port_82B46810 {

// Arguments: r3 = eight destination channel pointers, r4 = eight source channel pointers,
// r7 = destination channel count, r8 = source channel count, f1 = the TARGET gain, f2 = the
// CURRENT gain. Both pointer arrays live in the caller's frame (sub_82B31838 builds them at
// r1+112 and r1+80); the channel runs they point at are 1024 bytes each, matching
// sub_82B44D18's block exactly.

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, never read off
// by eye, then confirmed by reading the bytes out of the dumped image
// (probe/harness/out/image). The three tables are contiguous rodata.
constexpr uint32_t kRampRate = 0x822F8A00;    // lis -32208 ; addi -31232 ; lfs 1024 == 1/65
constexpr uint32_t kGainTable = 0x820ED6C0;   // lis -32241 ; addi -10560: {1.0, 0.707, 0.5}
constexpr uint32_t kBoundTable = 0x820ED700;  // lis -32241 ; addi -10496: 64 (first,last) pairs
constexpr uint32_t kProgram = 0x820ED780;     // lis -32241 ; addi -10368: the routing bytes

// A routing byte: bits 0-2 the destination channel, bits 3-5 the source channel, bits 6-7 an
// index into kGainTable. Each rlwinm already scales its field by four, into a word offset.
constexpr uint32_t kDstShift = 2, kDstMask = 0x1Cu;         // rlwinm r8,r11,2,27,29
constexpr uint32_t kSrcShift = 31, kSrcMask = 0x1Cu;        // rlwinm r9,r11,31,27,29
constexpr uint32_t kGainShift = 28, kGainMask = 0xFFFFFFCu; // rlwinm r10,r11,28,4,29
constexpr uint32_t kChannels = 8;             // three bits of index, so eight of each array
constexpr uint32_t kPointerArrayBytes = 4 * kChannels;

// sub_82B44D18 writes [(dst & ~0xF), +1024): eight blocks of eight 16-byte stores. Mirrored from
// that port's own Windows(), not re-derived.
constexpr uint32_t kMixBlockBytes = 1024;

// The first and last program index are u8, so the walk is at most 256 bytes long.
constexpr uint32_t kMaxOps = 256;

// stwu r1,-144(r1). Reproduced: sub_82B44D18 materialises two vectors through its own frame at
// r1-112..r1-81, so without the move those stores land on this function's own spill slots and
// the native run's out-of-window footprint stops matching the original's.
constexpr uint32_t kFrameSize = 144;

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

double MulSingle(double a, double b) { return double(float(a * b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }

// rlwinm: a 32-bit rotate then a mask, kept in that form rather than simplified to a shift, so
// each line still reads against its mnemonic.
inline uint32_t Rlwinm(uint32_t value, uint32_t sh, uint32_t mask) {
  return __builtin_rotateleft32(value, sh) & mask;
}

// The (source, destination) pair's byte offset into kBoundTable: two bytes per combination, row
// by source count. The add is 64-bit on a zero-extended rlwinm result and r7's whole register,
// and only the final rlwinm truncates, so the arithmetic is kept in 64 bits to that point.
inline uint32_t PairOffset(uint32_t in_channels, uint64_t out_channels) {
  const uint64_t row = uint64_t((in_channels << 3) & 0xFFFFFFF8u);  // rlwinm r11,r8,3,0,28
  const uint64_t entry = row + out_channels - 9;  // add r11,r11,r7 ; addi r11,r11,-9
  return Rlwinm(uint32_t(entry), 1, 0xFFFFFFFEu);  // rlwinm r10,r11,1,0,30
}

REX_FUNC(Native) {
  const uint32_t dst_array = ctx.r3.u32;     // mr r28,r3
  const uint32_t src_array = ctx.r4.u32;     // mr r27,r4
  const uint64_t out_channels = ctx.r7.u64;  // r7, consumed once as a 64-bit addend
  const uint32_t in_channels = ctx.r8.u32;   // r8
  const double target = ctx.f1.f64;
  const double current = ctx.f2.f64;         // fmr f7,f2
  // __savegprlr_26 spills r26-r31 and lr, and __restgprlr_26 puts them back, so the original's
  // exit values are its entry values. The harness compares r13-r31; the callee is ABI-correct
  // (__savegprlr_24), so this is belt and braces, but it is exactly right.
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister entry_r1 = ctx.r1;

  // stwu r1,-144(r1): the back chain, written with the value the original writes, then r1 moves.
  const uint32_t frame = entry_r1.u32 - kFrameSize;
  REX_STORE_U32(frame, entry_r1.u32);
  ctx.r1.u32 = frame;

  ctx.fpscr.disableFlushModeUnconditional();
  const double delta = SubSingle(target, current);  // fsubs f13,f1,f2
  // fmuls f6,f13,f0 with f0 = 1/65: the per-sample increment of a 64-sample ramp, before the
  // per-entry gain. sub_82B44D18 advances by f2 for 64 samples and then holds f1 + 64*f2, so the
  // ramp lands at 64/65 of the way to the target, not on it. Reproduced, not corrected.
  const double ramp = MulSingle(delta, LoadSingle(base, kRampRate));

  // lbzx r31,r10,r11 and lbz r6,1(r30): the first and last program index for this channel pair.
  const uint32_t bounds = kBoundTable + PairOffset(in_channels, out_channels);
  uint32_t index = REX_LOAD_U8(bounds);
  const uint32_t last = REX_LOAD_U8(bounds + 1);

  // bgt cr6,0x82b468b8 -- an empty range writes nothing at all.
  if (index <= last) {
    uint32_t cursor = kProgram + index - 1;  // addi r29,r11,-1, walked by lbzu
    do {                                     // loc_82B46880
      cursor += 1;                           // lbzu r11,1(r29): load at r29+1, then r29 = ea
      const uint32_t op = REX_LOAD_U8(cursor);
      const uint32_t gain_off = Rlwinm(op, kGainShift, kGainMask);
      const uint32_t src_off = Rlwinm(op, kSrcShift, kSrcMask);
      const uint32_t dst_off = Rlwinm(op, kDstShift, kDstMask);

      ctx.fpscr.disableFlushModeUnconditional();
      const double scale = LoadSingle(base, kGainTable + gain_off);  // lfsx f0,r10,r26
      const uint32_t src_ptr = REX_LOAD_U32(src_array + src_off);    // lwzx r4,r9,r27
      const double step = MulSingle(scale, ramp);                    // fmuls f2,f0,f6
      const uint32_t dst_ptr = REX_LOAD_U32(dst_array + dst_off);    // lwzx r3,r8,r28
      const double start = MulSingle(scale, current);                // fmuls f1,f0,f7

      // bl 0x82b44d18 -- the gain-ramp mix-add, itself a verified port, so it goes through
      // GuestCall on the live context and is exercised by its own hook. GuestCall leaves the
      // float registers alone, so f1 and f2 are set first.
      ctx.f1.f64 = start;
      ctx.f2.f64 = step;
      skate3::audio::GuestCall(ctx, base, sub_82B44D18, uint64_t(dst_ptr), uint64_t(src_ptr));

      index += 1;  // addi r31,r31,1
      // lbz r7,1(r30) ; cmplw cr6,r31,r7 ; ble -- the last index is RELOADED every iteration.
    } while (index <= REX_LOAD_U8(bounds + 1));
  }

  // loc_82B468B8
  ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,144
  ctx.r26 = saved_r26;                   // b 0x82f52160 -- __restgprlr_26
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// This function stores nothing itself: the whole write set is sub_82B44D18's, one 1024-byte block
// per destination channel the program names. Both the program bytes and the pointer arrays are
// readable at entry -- the tables are rodata and the arrays sit in the caller's frame -- so the
// set is enumerable even though it comes entirely from a callee.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t dst_array = ctx.r3.u32;
  const uint32_t src_array = ctx.r4.u32;
  if (dst_array == 0 || src_array == 0) return false;

  const uint32_t bounds = kBoundTable + PairOffset(ctx.r8.u32, ctx.r7.u64);
  spec.read(bounds, 2);
  spec.read(kGainTable, 16);
  spec.read(kRampRate, 4);
  // sub_82B44D18's constant pool, as its own Windows() declares it, so a recorded vector carries
  // everything the callee reads. For replay only; reads cost nothing against the budget.
  spec.read(0x82257308u, 4);        // its kStepScale
  spec.read(0x820ED958u, 4);        // kRampSpan
  spec.read(0x82060C50u, 4);        // kLane2Scale
  spec.read(0x82063B08u, 4);        // kLane3Scale
  spec.read(0x8231BA20u, 16 * 8);   // kScale5..kScaleStep, then kScale1..kScale4
  spec.read(dst_array, kPointerArrayBytes);
  spec.read(src_array, kPointerArrayBytes);

  const uint32_t first = REX_LOAD_U8(bounds);
  const uint32_t last = REX_LOAD_U8(bounds + 1);
  if (first > last) return true;  // the loop never runs, and nothing else stores
  spec.read(kProgram + first, last - first + 1);

  // Eight possible destination indices and eight sources, so both sets are collected as bit
  // masks and the spans emitted once each: at most eight 1024-byte writes, 8 KB of the budget.
  uint32_t dst_seen = 0, src_seen = 0;
  uint32_t begins[kChannels];
  uint32_t dst_count = 0;
  for (uint32_t i = 0; i <= last - first && i < kMaxOps; i++) {
    const uint32_t op = REX_LOAD_U8(kProgram + first + i);
    const uint32_t src_index = Rlwinm(op, kSrcShift, kSrcMask) / 4;
    if (!(src_seen & (1u << src_index))) {
      src_seen |= 1u << src_index;
      spec.read(REX_LOAD_U32(src_array + 4 * src_index), kMixBlockBytes);
    }
    const uint32_t dst_index = Rlwinm(op, kDstShift, kDstMask) / 4;
    if (dst_seen & (1u << dst_index)) continue;
    dst_seen |= 1u << dst_index;
    const uint32_t ptr = REX_LOAD_U32(dst_array + 4 * dst_index);
    if (ptr == 0) return false;  // sub_82B44D18 would accumulate through null
    const uint32_t begin = ptr & ~0xFu;  // its stvx128 masks the address to 16 bytes
    if (begin + kMixBlockBytes < begin) return false;  // a channel within 1 KB of the top
    begins[dst_count++] = begin;
  }

  // The program bytes, the last-index byte and both pointer arrays are all re-read INSIDE the
  // loop, so a write span covering any of them would make this enumeration a fiction. The
  // reachable program range is kProgram..kProgram+255, a u8 index from the bound table.
  struct Span {
    uint32_t addr;
    uint32_t len;
  };
  const Span reread[3] = {{kGainTable, (kProgram + kMaxOps) - kGainTable},
                          {dst_array, kPointerArrayBytes},
                          {src_array, kPointerArrayBytes}};
  for (uint32_t i = 0; i < dst_count; i++) {
    for (uint32_t k = 0; k < 3; k++) {
      if (uint64_t(reread[k].addr) < uint64_t(begins[i]) + kMixBlockBytes &&
          uint64_t(begins[i]) < uint64_t(reread[k].addr) + reread[k].len) {
        return false;
      }
    }
  }

  for (uint32_t i = 0; i < dst_count && i < kChannels; i++) spec.write(begins[i], kMixBlockBytes);
  return true;
}

}  // namespace port_82B46810
SKATE3_PORT(82B46810, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
