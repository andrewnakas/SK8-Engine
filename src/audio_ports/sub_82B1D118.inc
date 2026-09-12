// sub_82B1D118  role: sum the u8[+0]-counted u32 array at +4 into r3, 64-bit accumulate (register-only)   lifted: skate3_recomp.66.cpp:66119 (74 lines)
// STATUS: verified
// calls/session: boot 151451  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D118.md
namespace port_82B1D118 {

// Object layout, from the body alone (nothing in docs/rw_audio_structs.h names it):
//   u8  +0   element count n
//   u32 +4   element 0 -- always read, even when n == 0
//   u32 +8.. elements 1..n-1
constexpr uint32_t kCountOffset = 0;
constexpr uint32_t kElementsOffset = 4;

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                                  // mr r8,r3
  const uint32_t count = REX_LOAD_U8(object + kCountOffset);           // lbz r4,0(r3)
  // Every add below is a 64-bit add of zero-extended u32 loads, as lifted; the caller's r3 is
  // compared as all 64 bits, so the sum is kept in uint64_t and never truncated. Wraparound
  // addition is associative, so grouping the odd/even partial sums as the original does and
  // folding them in last yields the identical 64-bit value.
  uint64_t sum = REX_LOAD_U32(object + kElementsOffset);               // lwz r3,4(r3)
  ctx.r3.u64 = sum;
  uint64_t odd = 0;                                                    // li r7,0
  uint64_t even = 0;                                                   // li r6,0
  uint32_t next = 1;                                                   // li r5,1
  // cmpwi cr6,r4,1 ; blelr cr6  -- n <= 1 (n is a zero-extended byte, never negative)
  if (!(static_cast<int32_t>(count) > 1)) return;

  // addi r11,r4,-1 ; cmpwi cr6,r11,2 ; blt cr6,loc_82B1D174  -- n == 2 skips the pair loop
  if (!(static_cast<int32_t>(count - 1) < 2)) {
    // addi r10,r11,-2 ; rlwinm r10,r10,31,1,31 ; addi r10,r10,1  -- pairs = ((n-3) >> 1) + 1
    const uint32_t pairs = ((count - 3) >> 1) + 1;
    // rlwinm r9,r10,1,0,30 ; addi r5,r9,1  -- first index the loop did not reach
    next = (pairs << 1) + 1;
    uint32_t cursor = object + kElementsOffset;                        // addi r11,r8,4
    for (uint32_t i = 0; i < pairs; i++) {                             // mtctr r10 ... bdnz
      odd += REX_LOAD_U32(cursor + 4);                                 // lwz r9,4(r11) ; add r7,r9,r7
      cursor += 8;                                                     // lwzu r10,8(r11)
      even += REX_LOAD_U32(cursor);                                    //   ... ; add r6,r10,r6
    }
  }
  // loc_82B1D174: cmpw cr6,r5,r4 ; bge cr6,loc_82B1D18C  -- one trailing element when n is even
  if (static_cast<int32_t>(next) < static_cast<int32_t>(count)) {
    // addi r11,r5,1 ; rlwinm r10,r11,2,0,29 ; lwzx r11,r10,r8 ; add r3,r11,r3
    sum += REX_LOAD_U32(object + ((next + 1) << 2));
  }
  // loc_82B1D18C: add r11,r6,r7 ; add r3,r11,r3 ; blr
  ctx.r3.u64 = sum + (even + odd);
}

// No stores: the result lives in r3 only. Read set: the count byte and every element the
// original touches -- element 0 always, elements 1..n-1 when n >= 2.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  const uint32_t count = REX_LOAD_U8(object + kCountOffset);
  const uint32_t elements = count > 1 ? count : 1;
  spec.read(object, kElementsOffset + 4 * elements);
  return true;
}

}  // namespace port_82B1D118
SKATE3_PORT(82B1D118, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
