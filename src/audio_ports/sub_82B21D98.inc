// sub_82B21D98  role: the 5.1 mix-and-clamp pass -- build the source and destination plane pointer arrays, run the routing mix, interleave into the output block, ramp it once if asked, then clamp all 1536 words in place   lifted: skate3_recomp.67.cpp:9770 (260 lines)
// STATUS: verified
// calls/session: boot 31906  play 36600  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B21D98.md
namespace port_82B21D98 {

constexpr uint32_t kFrameBytes = 176;  // stwu r1,-176(r1)
constexpr uint32_t kSrcArray = 80;     // addi r4,r1,80  -- eight source plane pointers
constexpr uint32_t kDstArray = 112;    // addi r3,r1,112 -- `outputs` destination plane pointers

// The object in r3. Nothing in docs/rw_audio_structs.h names it; these are the same names
// port_82B21F58 (this function's only caller, verified) uses for the same fields.
constexpr uint32_t kDescHolder = 44;  // lwz r10,44(r3)
constexpr uint32_t kSrcDesc = 28;     // lwz r8,28(r10)   eight planar source channels
constexpr uint32_t kDstDesc = 32;     // lwz r30,32(r10)  the output planes
constexpr uint32_t kModeGain = 52;    // lfs f13,52(r31)  compared against a constant, vestigially
constexpr uint32_t kBlockBase = 84;   // lwz r10,84(r31)
constexpr uint32_t kBlockIndex = 92;  // lwz r11,92(r31)

// The plane descriptor both of those point at, named as port_82B46B30 names them.
constexpr uint32_t kPlaneBase = 4;     // lwz r10,4(r8)
constexpr uint32_t kPlaneFrames = 14;  // lhz r11,14(r8) -- the plane stride in floats

constexpr uint32_t kSrcPlanes = 8;       // the sp+80 array always has eight entries
constexpr uint32_t kMaxOutputs = 8;      // the sp+112 array is the same size
constexpr uint64_t kMixFrames = 256;     // li r6,256 -- forwarded to sub_82B426D0 as the count
constexpr uint32_t kPlaneBytes = 1024;   // 256 floats
constexpr uint32_t kBlockBytes = 6144;   // addi r11,r31,6144 -- 256 frames of 24 bytes
constexpr uint32_t kMaxTableRun = 64;    // refuse to walk a longer routing run than this

// The eight source multipliers, spelled out one at a time in the lifted form (rotlwi 1/2/3/4, add,
// rlwinm 2/3, mulli 28) rather than looped, which is how the eight-entry size is known. Each is a
// 32-bit form, but the frame count is a u16 so 28 * frames cannot overflow one: the products are
// exact, and only the adds against the base truncate.
constexpr uint64_t kSrcMultiple[kSrcPlanes] = {0, 4, 8, 12, 16, 20, 24, 28};

// Addresses computed as ((imm & 0xFFFF) << 16) + offset, never by eye.
constexpr uint32_t kChannelCountByte = 0x8306705D;  // lis -31994 ; lbz 28765 -- output planes
constexpr uint32_t kRampFlagByte = 0x83067057;      // lis -31994 ; lbz/stb 28759 -- "ramp once"
constexpr uint32_t kPairTable = 0x820ED6CC;         // lis -32241 ; addi -10548
constexpr uint32_t kOpTable = 0x820ED6DC;           // lis -32241 ; addi -10532
constexpr uint32_t kModeCompare = 0x82257308;       // lis -32219 ; lfs 29448
constexpr uint32_t kClampHigh = 0x8231A844;         // lis -32206 ; lfs -22460 -- f12, upper bound
constexpr uint32_t kClampLow = 0x8216DEE0;          // lis -32233 ; lfs -8480  -- f13, lower bound

// subf r11,r31,r11 ; addi -1 ; rlwinm r11,r11,30,2,31 ; addi 1 -- the difference is always exactly
// 6144 (addi r11,r31,6144), so the trip count is the constant ((6144 - 1) >> 2) + 1.
constexpr uint32_t kClampTrips = ((kBlockBytes - 1) >> 2) + 1;  // 1536

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r31,r3

  // __savegprlr_29 spills r29-r31 into the red zone and the exit restores them; this body keeps
  // all three in C++ locals and never writes ctx.r29-r31, so their entry values stand. f14-f31 are
  // untouched (only f0, f12 and f13 are used).
  const uint32_t entry_sp = ctx.r1.u32;
  // stwu r1,-176(r1). REPRODUCED: the two pointer arrays below live in this frame and their
  // addresses are handed to sub_82B426D0, whose own frame must sit under them.
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;

  const uint32_t holder = REX_LOAD_U32(object + kDescHolder);         // lwz r10,44(r3)
  const uint32_t src_desc = REX_LOAD_U32(holder + kSrcDesc);          // lwz r8,28(r10)
  const uint64_t outputs = REX_LOAD_U8(kChannelCountByte);            // lbz r5,28765(r9)
  const uint32_t dst_desc = REX_LOAD_U32(holder + kDstDesc);          // lwz r30,32(r10)
  const uint64_t src_frames = REX_LOAD_U16(src_desc + kPlaneFrames);  // lhz r11,14(r8)
  const uint64_t src_base = REX_LOAD_U32(src_desc + kPlaneBase);      // lwz r10,4(r8)

  // The eight source plane pointers, ascending: stw r10,80(r1) then +84 .. +108. Every add is
  // 64-bit on zero-extended words and every stw keeps the low one.
  for (uint32_t k = 0; k < kSrcPlanes; k++) {
    REX_STORE_U32(frame + kSrcArray + 4 * k, uint32_t(kSrcMultiple[k] * src_frames + src_base));
  }

  // cmplwi cr6,r5,0 ; beq cr6,0x82b21e5c -- a zero output count leaves the sp+112 array untouched,
  // which is also why the ctr loop below cannot run 2^32 times.
  if (outputs != 0) {
    // lhz r9,14(r30) ; rotlwi r9,r9,2 -- the destination stride in bytes; the u16 cannot overflow.
    const uint64_t dst_stride = __builtin_rotateleft32(REX_LOAD_U16(dst_desc + kPlaneFrames), 2);
    uint64_t pointer = REX_LOAD_U32(dst_desc + kPlaneBase);  // lwz r11,4(r30)
    uint32_t slot = frame + kSrcArray + 4 * (kSrcPlanes - 1);  // addi r10,r1,108
    for (uint64_t i = 0; i < outputs; i++) {  // mtctr r5 ; loc_82B21E50 ; bdnz
      slot += 4;                              // stwu r11,4(r10) -- pre-increment, so sp+112 first
      REX_STORE_U32(slot, uint32_t(pointer));
      pointer = dst_stride + pointer;         // add r11,r9,r11 -- a 64-bit running sum, truncated
    }                                         // at every store
  }

  // loc_82B21E5C
  ctx.fpscr.disableFlushModeUnconditional();
  const double mode_gain = LoadSingle(base, object + kModeGain);  // lfs f13,52(r31)
  const double compared = LoadSingle(base, kModeCompare);         // lfs f0,29448(r11)
  // fcmpu cr6,f13,f0 ; bne cr6,0x82b21e98 -- VESTIGIAL: both arms compute the same pair-table
  // address (lis -32241 ; addi -10548 on each side, only the register allocation differs), and cr6
  // is not in the compared set. The two lfs are kept because they are reads Windows() records.
  const bool vestigial_branch = mode_gain != compared;
  (void)vestigial_branch;
  // rlwinm r5,1,0,30 ; add ; addi r7,r11,-2 -- 2 * outputs into the pair table, minus one entry.
  const uint32_t pair = kPairTable + uint32_t(2 * outputs) - 2;

  // bl 0x82b426d0 -- the routing pass: r3 = destination pointers, r4 = source pointers,
  // r5 = outputs, r6 = 256 frames, r7 = the pair entry, r8 = the routing byte table.
  skate3::audio::GuestCall(ctx, base, sub_82B426D0, uint64_t(ctx.r1.s64 + kDstArray),
                           uint64_t(ctx.r1.s64 + kSrcArray), outputs, kMixFrames, uint64_t(pair),
                           uint64_t(kOpTable));

  const uint64_t block_index = REX_LOAD_U32(object + kBlockIndex);  // lwz r11,92(r31)
  const uint64_t block_base = REX_LOAD_U32(object + kBlockBase);    // lwz r10,84(r31)
  // rlwinm r9,r11,1,0,30 ; add r11,r11,r9 -- 3 * index; the add is 64-bit but the rlwinm that
  // follows reads only the low word, so the carry out of bit 31 is dropped here.
  const uint32_t doubled = (uint32_t(block_index) << 1) & 0xFFFFFFFEu;
  const uint32_t triple = uint32_t(block_index) + doubled;
  // rlwinm r11,r11,11,0,20 -- << 11 with the low eleven bits cleared.
  const uint32_t block_offset = (triple << 11) & 0xFFFFF800u;
  // add r31,r11,r10 -- 64-bit, and r31 is passed on in r3, so the carry is kept here and dropped
  // only where the value is used as an address.
  const uint64_t block = uint64_t(block_offset) + block_base;

  // bl 0x82b46b30 -- interleave the destination planes into the block. mr r4,r30.
  skate3::audio::GuestCall(ctx, base, sub_82B46B30, block, uint64_t(dst_desc));

  // lbz r10,28759(r5) ; cmplwi ; beq -- the ramp flag, reloaded after the interleave.
  if (REX_LOAD_U8(kRampFlagByte) != 0) {
    skate3::audio::GuestCall(ctx, base, sub_82B20E18, block);  // bl 0x82b20e18
    REX_STORE_U8(kRampFlagByte, 0);                            // li r11,0 ; stb r11,28759(r5)
  }

  // loc_82B21EF4: addi r11,r31,6144 ; cmplw cr6,r31,r11 ; bge -- an UNSIGNED 32-bit compare, so a
  // block whose end wraps 2^32 skips the clamp entirely.
  const uint64_t block_end = block + kBlockBytes;
  if (uint32_t(block) < uint32_t(block_end)) {
    ctx.fpscr.disableFlushModeUnconditional();
    const double high = LoadSingle(base, kClampHigh);  // lfs f12,-22460(r9)
    const double low = LoadSingle(base, kClampLow);    // lfs f13,-8480(r11)
    uint32_t word = uint32_t(block);                   // mr r10,r31
    for (uint32_t i = 0; i < kClampTrips; i++) {       // loc_82B21F28 ; bdnz
      ctx.fpscr.disableFlushModeUnconditional();
      const double value = LoadSingle(base, word);  // lfs f0,0(r10)
      // fcmpu cr6,f0,f13 ; bge cr6,0x82b21f3c -- an unordered compare (a NaN word) takes the bge
      // and then the ble below, so a NaN is left exactly as it was.
      if (value < low) {
        StoreSingle(base, word, low);  // stfs f13,0(r10)
      } else {
        // loc_82B21F3C: fcmpu cr6,f0,f12 ; ble cr6,0x82b21f48
        ctx.fpscr.disableFlushModeUnconditional();
        if (value > high) StoreSingle(base, word, high);  // stfs f12,0(r10)
      }
      word += 4;  // addi r10,r10,4
    }
  }

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,176, on the full 64-bit register
}

// The write set, which is the same one port_82B21F58's verified Windows() already enumerates for
// this call -- that file is the sibling to change if either changes:
//
//   * the 6144-byte mix block: sub_82B46B30 writes 24 * 256 bytes from r3, sub_82B20E18 writes
//     512 * the channel-count byte (inside the block at eight outputs or fewer), and the clamp
//     loop rewrites words of it in place;
//   * one 1024-byte span per destination plane: sub_82B426D0 mixes into it through sub_82B3BED8
//     (first touch) or sub_82B44B20 (accumulate), both of which write exactly [r3, r3 + 4*r6) with
//     r6 = the literal 256 passed here, or memsets it through sub_82EE5E80 when the routing run
//     never names that plane;
//   * the ramp flag byte, only on the path that read it non-zero.
//
// Not declared, because they are this call's own frame: the eight source pointers at sp+80, the
// `outputs` destination pointers at sp+112, and sub_82B426D0's own eight-byte touched-flag array.
//
// Two refusals, both predicted from entry state, both inherited from the sibling port: an output
// count past eight would index off the end of the sp+112 array, and a routing byte naming a
// destination plane at or past the count would make the original mix 1024 bytes through an
// uninitialised frame word -- an address this builder cannot name.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;
  const uint32_t holder = REX_LOAD_U32(object + kDescHolder);
  if (holder == 0) return false;
  const uint32_t src_desc = REX_LOAD_U32(holder + kSrcDesc);
  const uint32_t dst_desc = REX_LOAD_U32(holder + kDstDesc);
  if (src_desc == 0 || dst_desc == 0) return false;
  spec.read(object + kDescHolder, 4);
  spec.read(holder + kSrcDesc, 8);  // +28 and +32
  spec.read(object + kModeGain, 4);
  spec.read(object + kBlockBase, 12);  // +84 through +95, so +84 and +92
  spec.read(kChannelCountByte, 1);
  spec.read(kRampFlagByte, 1);
  spec.read(kModeCompare, 4);
  spec.read(kClampHigh, 4);
  spec.read(kClampLow, 4);

  const uint32_t output_count = REX_LOAD_U8(kChannelCountByte);
  if (output_count > kMaxOutputs) return false;  // would index past the sp+112 array

  // The mix block, in 32-bit arithmetic: the lifted add is 64-bit but the value reaches memory
  // only as a store base, and the clamp is skipped outright if the 32-bit end wraps.
  const uint32_t block = REX_LOAD_U32(object + kBlockBase) +
                         ((3u * REX_LOAD_U32(object + kBlockIndex)) << 11 & 0xFFFFF800u);
  if (block + kBlockBytes < block) return false;  // cmplw r31,r11 wraps: not enumerable
  spec.write(block, kBlockBytes);
  spec.read(block, kBlockBytes);

  // stb r11,28759(r5) -- written only on the path that read it non-zero.
  if (REX_LOAD_U8(kRampFlagByte) != 0) spec.write(kRampFlagByte, 1);

  // The destination planes: mixed into by sub_82B426D0, or memset by it.
  const uint32_t dst_base = REX_LOAD_U32(dst_desc + kPlaneBase);
  const uint32_t dst_stride = 4u * REX_LOAD_U16(dst_desc + kPlaneFrames);
  spec.read(dst_desc + kPlaneBase, 12);
  for (uint32_t i = 0; i < output_count && i < kMaxOutputs; i++) {
    // stwu r11,4(r10) stores the 32-bit truncation of a 64-bit running sum, so each pointer is
    // (base + i*stride) mod 2^32 -- reproduced here in 32-bit arithmetic for that reason.
    spec.write(dst_base + i * dst_stride, kPlaneBytes);
    spec.read(dst_base + i * dst_stride, kPlaneBytes);  // sub_82B44B20 accumulates in place
  }

  // The eight source planes, read-only here.
  const uint32_t src_base = REX_LOAD_U32(src_desc + kPlaneBase);
  const uint32_t src_stride = 4u * REX_LOAD_U16(src_desc + kPlaneFrames);
  spec.read(src_desc + kPlaneBase, 12);
  for (uint32_t k = 0; k < kSrcPlanes; k++) spec.read(src_base + k * src_stride, kPlaneBytes);

  // The routing run, and the check that it names no destination past output_count.
  const uint32_t pair = kPairTable + 2 * output_count - 2;
  const uint32_t first = REX_LOAD_U8(pair);
  const uint32_t last = REX_LOAD_U8(pair + 1);
  spec.read(pair, 2);
  if (first <= last) {  // otherwise sub_82B426D0's routing loop never runs
    if (last - first >= kMaxTableRun) return false;
    for (uint32_t i = first; i <= last; i++) {
      if ((REX_LOAD_U8(kOpTable + i) & 7u) >= output_count) return false;
    }
    spec.read(kOpTable + first, last - first + 1);
  }
  return true;
}

}  // namespace port_82B21D98
SKATE3_PORT(82B21D98, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
