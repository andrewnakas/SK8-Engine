// sub_82B22678  role: hard clipper -- clamp 256 samples/channel of the source buffer into [-level, level] in the dest buffer, then swap the pair   lifted: skate3_recomp.67.cpp:11130 (119 lines)
// STATUS: verified
// calls/session: boot 49732  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B22678.md
namespace port_82B22678 {

// Offsets as this function reads them; docs/rw_audio_structs.h names none of these objects.
// r3 (effect state):
constexpr uint32_t kChannelCount = 42;  // u8   lbz r31,42(r3)
constexpr uint32_t kClipLevel = 52;     // f32  lfs f13,52(r3)
// r4 (buffer pair): two buffer-descriptor pointers, swapped on exit.
constexpr uint32_t kSourceBuffer = 28;  // lwz r5,28(r4) ; stw r6,28(r4)
constexpr uint32_t kDestBuffer = 32;    // lwz r6,32(r4) ; stw r5,32(r4)
// buffer descriptor:
constexpr uint32_t kBufferData = 4;     // u32  float* samples
constexpr uint32_t kBufferStride = 14;  // u16  channel stride, in samples

// Pool singles (values read from the dumped image, probe/harness/out/image/g_8200.bin; the body
// loads them).
constexpr uint32_t kCeiling = 0x820F0000u - 10884;  // lis r11,-32241 ; -10884(r11): f32 100.0
constexpr uint32_t kFloorScale = 0x82170000u - 8480; // lis r11,-32233 ; -8480(r11): f32 -1.0

constexpr uint32_t kFrameSamples = 256;               // li r11,256 ; mtctr r11
constexpr uint32_t kFrameBytes = kFrameSamples * 4;
// 31 channel spans + the 8-byte pair swap = 32 spans, 31,752 bytes: the most the budget holds.
constexpr uint32_t kMaxChannels = 31;

// lfs / lfsx: a single widened to double, as the lifted `double(temp.f32)`.
inline double LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return double(f);
}
// stfs: the double rounded to single, as the lifted `temp.f32 = float(fN)`.
inline void StoreF32(uint8_t* base, uint32_t addr, double value) {
  const float f = float(value);
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  REX_STORE_U32(addr, bits);
}
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t state = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;

  ctx.fpscr.disableFlushModeUnconditional();
  const double level = LoadF32(base, state + kClipLevel);  // lfs f13,52(r3)
  const double ceiling = LoadF32(base, kCeiling);          // lfs f0,-10884(r11)
  // fcmpu cr6,f13,f0 ; bge cr6,0x82b2272c   (NaN is unordered: not lt, takes the branch)
  if (level < ceiling) {
    const uint32_t channels = REX_LOAD_U8(state + kChannelCount);  // lbz r31,42(r3)
    const uint32_t source = REX_LOAD_U32(pair + kSourceBuffer);    // lwz r5,28(r4)
    const uint32_t dest = REX_LOAD_U32(pair + kDestBuffer);        // lwz r6,32(r4)
    const double scale = LoadF32(base, kFloorScale);               // lfs f0,-8480(r11)
    const double floor_level = double(float(level * scale));       // fmuls f12,f13,f0
    // cmplwi cr6,r31,0 ; beq cr6,0x82b22724 -- then a do-while on
    // addi r7,r7,1 ; cmplw cr6,r7,r31 ; blt cr6,0x82b226b4
    for (uint32_t channel = 0; channel < channels; channel++) {
      // loc_82B226B4: the descriptors are reloaded every channel, after the previous channel's
      // stores, exactly as the original does.
      const uint32_t src_stride = REX_LOAD_U16(source + kBufferStride);  // lhz r10,14(r5)
      const uint32_t dst_stride = REX_LOAD_U16(dest + kBufferStride);    // lhz r8,14(r6)
      // mullw r3,r10,r7 ; mullw r11,r8,r7 ; rlwinm rN,rN,2,0,29 -- 32-bit byte offsets
      const uint32_t src_offset = (src_stride * channel) << 2;
      const uint32_t dst_offset = (dst_stride * channel) << 2;
      const uint32_t src_data = REX_LOAD_U32(source + kBufferData);  // lwz r10,4(r5)
      const uint32_t dst_data = REX_LOAD_U32(dest + kBufferData);    // lwz r9,4(r6)
      // add r10,r8,r10 ; add r11,r11,r9 ; subf r10,r11,r10 -- lfsx reads r10+r11 (mod 2^32),
      // which is the source cursor; r11 is the dest cursor.
      const uint32_t in = src_offset + src_data;
      uint32_t out = dst_offset + dst_data;
      for (uint32_t k = 0; k < kFrameSamples; k++) {
        // loc_82B226E8
        ctx.fpscr.disableFlushModeUnconditional();
        const double sample = LoadF32(base, in + (k << 2));  // lfsx f0,r10,r11
        // fcmpu cr6,f0,f13 ; ble cr6,0x82b226fc
        if (sample > level) {
          StoreF32(base, out, level);  // stfs f13,0(r11) ; b 0x82b22710
        } else {
          // loc_82B226FC
          ctx.fpscr.disableFlushModeUnconditional();
          // fcmpu cr6,f0,f12 ; bge cr6,0x82b2270c
          if (sample < floor_level) {
            StoreF32(base, out, floor_level);  // stfs f12,0(r11) ; b 0x82b22710
          } else {
            // loc_82B2270C
            ctx.fpscr.disableFlushModeUnconditional();
            StoreF32(base, out, sample);  // stfs f0,0(r11)
          }
        }
        out = out + 4;  // addi r11,r11,4 ; bdnz 0x82b226e8
      }
    }
    // loc_82B22724
    REX_STORE_U32(pair + kDestBuffer, source);  // stw r5,32(r4)
    REX_STORE_U32(pair + kSourceBuffer, dest);  // stw r6,28(r4)
  }
  // loc_82B2272C
  ctx.r3.s64 = 1;  // li r3,1 ; blr   (r31 untouched: the original's std/ld pair restores it)
}

// Writes, when level < ceiling: the pair's two pointers {r4+28, 8}, and per channel the 1 KB
// dest span at dst_data + channel*dst_stride*4. Otherwise nothing. The loop reloads both
// descriptors after earlier channels' stores, so the entry-state addresses hold only if no dest
// span touches a descriptor field (+4 data, +14 stride) -- if one does, return false.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t state = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  spec.read(state + kChannelCount, kClipLevel + 4 - kChannelCount);
  spec.read(kCeiling, 4);
  if (!(LoadF32(base, state + kClipLevel) < LoadF32(base, kCeiling))) return true;

  spec.read(kFloorScale, 4);
  spec.write(pair + kSourceBuffer, 8);  // stw r6,28(r4) ; stw r5,32(r4)
  const uint32_t channels = REX_LOAD_U8(state + kChannelCount);
  if (channels > kMaxChannels) return false;  // over the 32-span / 32 KB budget
  if (channels == 0) return true;

  const uint32_t source = REX_LOAD_U32(pair + kSourceBuffer);
  const uint32_t dest = REX_LOAD_U32(pair + kDestBuffer);
  spec.read(source + kBufferData, kBufferStride + 2 - kBufferData);
  spec.read(dest + kBufferData, kBufferStride + 2 - kBufferData);
  const uint32_t src_data = REX_LOAD_U32(source + kBufferData);
  const uint32_t dst_data = REX_LOAD_U32(dest + kBufferData);
  const uint32_t src_stride = REX_LOAD_U16(source + kBufferStride);
  const uint32_t dst_stride = REX_LOAD_U16(dest + kBufferStride);
  for (uint32_t channel = 0; channel < channels && channel < kMaxChannels; channel++) {
    const uint32_t out = ((dst_stride * channel) << 2) + dst_data;
    if (uint64_t(out) + kFrameBytes > 0x100000000ull) return false;  // span wraps the space
    if (Overlaps(out, kFrameBytes, source + kBufferData, 4) ||
        Overlaps(out, kFrameBytes, source + kBufferStride, 2) ||
        Overlaps(out, kFrameBytes, dest + kBufferData, 4) ||
        Overlaps(out, kFrameBytes, dest + kBufferStride, 2)) {
      return false;  // a store would move a later channel's addresses mid-call
    }
    spec.write(out, kFrameBytes);
    spec.read(((src_stride * channel) << 2) + src_data, kFrameBytes);
  }
  return true;
}

}  // namespace port_82B22678
SKATE3_PORT(82B22678, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
