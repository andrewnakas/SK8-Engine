// sub_82F52FB8  role: memcpy(r3 dst, r4 src, r5 len) -- single bytes until dst is word-aligned, then whole words (one lwz when src is aligned too, four lbz assembled big-endian otherwise), then the tail bytes, all forward; r3 comes back unchanged   lifted: skate3_recomp.106.cpp:68031 (101 lines)
// STATUS: pending -- written 2026-09-13, not yet compared under the shadow harness
// calls/session: boot ?  play ?  map ?          thread: first called on Main XThread (docs/audio-executed-set.txt); reached from sub_82B2DBA8, sub_82B42C98, sub_82B427D8 and sub_82B43340
// note: outside the 216 -- ported because four verified audio bodies call it and none can be ported to Rust without it
namespace port_82F52FB8 {

// The copy is forward and chunked: a word chunk reads all four bytes before it stores any, and a
// byte chunk one at a time. An overlapping copy therefore behaves as the original's chunking does,
// not as memmove -- which is the reason this is not simply a call to memcpy.
REX_FUNC(Native) {
  uint32_t dst = ctx.r3.u32;  // ori r6,r3,0 -- r3 itself is never written
  uint32_t src = ctx.r4.u32;
  uint64_t len = ctx.r5.u64;
  uint64_t ctr = len + 1;     // addi r0,r5,1 ; mtctr r0
  for (;;) {                  // loc_82F52FDC
    const bool aligned = (dst & 3u) == 0;  // andi. r0,r6,3
    ctr -= 1;                              // bdnzf eq,0x82f52fc8
    if (static_cast<uint32_t>(ctr) == 0 || aligned) break;
    len -= 1;                              // addi r5,r5,-1
    const uint8_t byte = REX_LOAD_U8(src);  // lbz r0,0(r4)
    src += 1;
    REX_STORE_U8(dst, byte);               // stb r0,0(r6)
    dst += 1;
  }
  const uint32_t words = static_cast<uint32_t>(len) >> 2;  // rlwinm. r0,r5,30,2,31
  if (words != 0) {
    if ((src & 3u) == 0) {
      for (uint32_t k = 0; k < words; k++) {  // loc_82F52FF8
        const uint32_t word = REX_LOAD_U32(src);  // lwz r7,0(r4)
        src += 4;
        REX_STORE_U32(dst, word);                 // stw r7,0(r6)
        dst += 4;
      }
    } else {
      for (uint32_t k = 0; k < words; k++) {  // loc_82F53030
        const uint32_t b3 = REX_LOAD_U8(src + 3);  // lbz r7,3(r4)
        const uint32_t b2 = REX_LOAD_U8(src + 2);  // lbz r8,2(r4) ; rlwimi
        const uint32_t b1 = REX_LOAD_U8(src + 1);  // lbz r9,1(r4) ; rlwimi
        const uint32_t b0 = REX_LOAD_U8(src + 0);  // lbz r10,0(r4) ; rlwimi
        src += 4;
        REX_STORE_U32(dst, (b0 << 24) | (b1 << 16) | (b2 << 8) | b3);  // stw r7,0(r6)
        dst += 4;
      }
    }
  }
  const uint32_t tail = static_cast<uint32_t>(len) & 3u;  // andi. r0,r5,3 ; beqlr
  for (uint32_t k = 0; k < tail; k++) {                   // loc_82F53018
    const uint8_t byte = REX_LOAD_U8(src);
    src += 1;
    REX_STORE_U8(dst, byte);
    dst += 1;
  }
}

// The byte count, from entry state: the alignment prefix (at most three bytes, and never more than
// the counter allows), then the low word of what is left, as whole words plus the tail.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  uint64_t len = ctx.r5.u64;
  uint64_t ctr = len + 1;
  uint32_t d = dst;
  uint64_t prefix = 0;
  for (;;) {
    const bool aligned = (d & 3u) == 0;
    ctr -= 1;
    if (static_cast<uint32_t>(ctr) == 0 || aligned) break;
    len -= 1;
    d += 1;
    prefix += 1;
  }
  const uint32_t low = static_cast<uint32_t>(len);
  const uint64_t total = prefix + uint64_t((low >> 2) * 4u) + uint64_t(low & 3u);
  if (total == 0) return true;
  if (total > skate3::audio::kPortWatchCap) return false;
  if (uint64_t(dst) + total > 0x100000000ull || uint64_t(src) + total > 0x100000000ull) return false;
  spec.write(dst, static_cast<uint32_t>(total));
  spec.read(src, static_cast<uint32_t>(total));
  return true;
}

}  // namespace port_82F52FB8
SKATE3_PORT(82F52FB8, skate3::audio::kPortPending, skate3::audio::kReturnR3)
