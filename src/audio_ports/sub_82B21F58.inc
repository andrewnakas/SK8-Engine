// sub_82B21F58  role: frame wrapper around the 5.1 mix-and-clamp pass, returning 1   lifted: skate3_recomp.67.cpp:10031 (25 lines)
// STATUS: verified
// calls/session: boot 15953  play 18300  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B21F58.md
namespace port_82B21F58 {

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)

// The object in r3, as sub_82B21D98 reads it. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kDescHolder = 44;  // lwz r10,44(r3)
constexpr uint32_t kSrcDesc = 28;     // lwz r8,28(r10)   eight planar source channels
constexpr uint32_t kDstDesc = 32;     // lwz r30,32(r10)  the output planes
constexpr uint32_t kModeGain = 52;    // lfs f13,52(r31)  compared against a constant
constexpr uint32_t kBlockBase = 84;   // lwz r10,84(r31)
constexpr uint32_t kBlockIndex = 92;  // lwz r11,92(r31)

// The plane descriptor both of those point at, named as port_82B46B30 names them.
constexpr uint32_t kPlaneBase = 4;     // lwz r10,4(r8)
constexpr uint32_t kPlaneFrames = 14;  // lhz r11,14(r8) -- the plane stride / 4

constexpr uint32_t kBlockBytes = 6144;  // addi r11,r31,6144 -- 256 frames of 24 bytes
constexpr uint32_t kPlaneBytes = 1024;  // li r6,256 floats, forwarded as the mix count
constexpr uint32_t kSrcPlanes = 8;      // the pointer array at sp+80 always has eight entries
constexpr uint32_t kMaxOutputs = 8;     // the array at sp+112 is the same size
constexpr uint32_t kMaxTableRun = 64;   // refuse to walk a longer routing run than this

// Addresses computed as ((imm & 0xFFFF) << 16) + offset, never by eye.
constexpr uint32_t kChannelCountByte = 0x8306705D;  // lis -31994 ; lbz 28765 -- output planes
constexpr uint32_t kRampFlagByte = 0x83067057;      // lis -31994 ; lbz/stb 28759 -- "ramp once"
constexpr uint32_t kPairTable = 0x820ED6CC;         // lis -32241 ; addi -10548
constexpr uint32_t kOpTable = 0x820ED6DC;           // lis -32241 ; addi -10532

REX_FUNC(Native) {
  // mflr r12 ; stw r12,-8(r1) -- the LR save into this function's own frame. Not reproduced: the
  // brief excludes a function's own frame, LR is not compared, and the only reader is the
  // matching reload below.
  const uint32_t entry_sp = ctx.r1.u32;

  // stwu r1,-96(r1). REPRODUCED. sub_82B21D98 builds the two pointer arrays its mixing tree
  // dereferences inside its own 176-byte frame and spills r29-r31 and LR below that through
  // __savegprlr_29, so with the move skipped its frame would land 96 bytes higher -- on top of
  // our caller's locals, outside any window.
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // bl 0x82b21d98 -- the object stays in r3; no argument is rewritten.
  skate3::audio::GuestCall(ctx, base, sub_82B21D98);

  ctx.r3.s64 = 1;                         // li r3,1 -- unconditional, on every path
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,96, on the full 64-bit register
}

// Windows for the callee. sub_82B21D98(object) does, in order:
//
//   1. fills sp+80..108 with eight source plane pointers, [srcDesc+4] + 4*k*u16[srcDesc+14];
//   2. if the output count is non-zero, fills sp+112.. with that many destination plane
//      pointers, [dstDesc+4] + i*4*u16[dstDesc+14];
//   3. sub_82B426D0(sp+112, sp+80, outputs, 256, pairTable + 2*outputs - 2, opTable) -- the
//      routing pass: for each byte of the run the pair names, mix source plane (b >> 3) & 7 into
//      destination plane b & 7 (sub_82B3BED8 on first touch, sub_82B44B20 after), then memset
//      every destination plane the run never named;
//   4. sub_82B46B30(block, dstDesc) -- interleave into the 6144-byte block;
//   5. if the ramp flag byte is set, sub_82B20E18(block) and clear the flag;
//   6. clamp all 1536 words of the block in place, between two constants.
//
// So the write set is the mix block, the flag byte, and one 1024-byte span per destination plane.
// Every deep callee is bounded by its own verified port: sub_82B3BED8 and sub_82B44B20 write
// [r3, r3 + 4*r6) with r6 = 256, sub_82EE5E80 is memset and gets the same 1024, sub_82B46B30
// writes 24*256 bytes from r3, sub_82B20E18 writes 512 * (that same channel-count byte) which at
// eight outputs or fewer is inside the block. sub_82B426D0's own stores are its frame and an
// eight-byte touched-flag array in it.
//
// The two things that must be predicted from entry state: the output count cannot exceed the
// eight entries of the sp+112 array, and the routing run must not name a destination plane past
// the count -- past either, the original reads an uninitialised frame word as a pointer and
// stores 1024 bytes through it, which is not an address Windows() can name.
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

  const uint32_t output_count = REX_LOAD_U8(kChannelCountByte);
  if (output_count > kMaxOutputs) return false;  // would index past the sp+112 array

  // The mix block. rlwinm r11,r11,11,0,20 ; add -- the 64-bit add's carry cannot reach the
  // address, which is the store's r10.u32, so this is deliberately 32-bit arithmetic.
  const uint32_t block = REX_LOAD_U32(object + kBlockBase) +
                         ((3u * REX_LOAD_U32(object + kBlockIndex)) << 11 & 0xFFFFF800u);
  if (block + kBlockBytes < block) return false;  // cmplw r31,r11 wraps: not enumerable
  spec.write(block, kBlockBytes);  // sub_82B46B30, sub_82B20E18, then the clamp loop in place
  spec.read(block, kBlockBytes);

  // stb r11,28759(r5) -- written only on the path that read it non-zero.
  if (REX_LOAD_U8(kRampFlagByte) != 0) spec.write(kRampFlagByte, 1);

  // The destination planes: mixed into by sub_82B426D0, or memset by it.
  const uint32_t dst_base = REX_LOAD_U32(dst_desc + kPlaneBase);
  const uint32_t dst_stride = 4u * REX_LOAD_U16(dst_desc + kPlaneFrames);
  spec.read(dst_desc + kPlaneBase, 12);
  for (uint32_t i = 0; i < output_count; i++) {
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
  if (first <= last) {  // cmplw r31,r11 ; bgt -- otherwise the routing loop never runs
    if (last - first >= kMaxTableRun) return false;
    for (uint32_t i = first; i <= last; i++) {
      if ((REX_LOAD_U8(kOpTable + i) & 7u) >= output_count) return false;
    }
    spec.read(kOpTable + first, last - first + 1);
  }
  return true;
}

}  // namespace port_82B21F58
SKATE3_PORT(82B21F58, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
