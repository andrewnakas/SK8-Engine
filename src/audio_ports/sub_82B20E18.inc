// sub_82B20E18  role: apply a rising per-frame ramp across a 128-frame interleaved block   lifted: skate3_recomp.67.cpp:7467 (143 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 1 comparable calls in the weaker profile, 1 in the stronger, against 143 lifted lines -- one call in each profile. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 3  play 3  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B20E18.md
namespace port_82B20E18 {

// Addresses computed as ((imm & 0xFFFF) << 16) + offset.
constexpr uint32_t kChannelCountByte = 0x8306705D;  // lis -31994 ; lbz 28765
constexpr uint32_t kZeroFloat = 0x82165A10;         // lis -32234 ; lfs 23056
constexpr uint32_t kFrameStep = 0x8231A844;         // lis -32206 ; lfs -22460
constexpr uint32_t kRampScale = 0x820300D4;         // lis -32253 ; lfs 212
constexpr uint32_t kFrames = 128;                   // rotlwi r10,r11,9 is count * 512 bytes

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }

REX_FUNC(Native) {
  uint32_t block = ctx.r3.u32;
  const uint32_t channels = REX_LOAD_U8(kChannelCountByte);  // lbz r11,28765(r11)
  ctx.fpscr.disableFlushModeUnconditional();
  double frame_index = LoadSingle(base, kZeroFloat);          // lfs f13,23056(r10) -- starts at 0
  // rotlwi r10,r11,9 ; add r7,r10,r3 -- the block is channels * 512 bytes, i.e. 128 frames.
  const uint32_t end = static_cast<uint32_t>(__builtin_rotateleft32(channels, 9)) + block;
  if (!(block < end)) {
    return;  // bgelr -- an empty block writes nothing
  }
  const uint32_t stride = (channels * 4u) & 0xFFFFFFFCu;  // rlwinm r6,r11,2,0,29
  const double step = LoadSingle(base, kFrameStep);        // lfs f11,-22460(r11)
  const double scale = LoadSingle(base, kRampScale);       // lfs f12,212(r10)
  do {
    uint32_t done = 0;
    // The four-at-a-time loop, entered only when the channel count reaches four.
    if (static_cast<int32_t>(channels) >= 4) {
      ctx.fpscr.disableFlushModeUnconditional();
      const double gain = MulSingle(frame_index, scale);  // fmuls f0,f13,f12
      const uint32_t trips = ((channels - 4) >> 2) + 1;   // rlwinm r10,r10,30,2,31 ; addi r9,r10,1
      done = trips * 4;
      uint32_t at = block;
      for (uint32_t i = 0; i < trips; i++) {
        // Four loads, four multiplies, then the stores -- the last one is stfsu, which also
        // advances the pointer by 16.
        ctx.fpscr.disableFlushModeUnconditional();
        const double s0 = LoadSingle(base, at + 0);
        const double s1 = LoadSingle(base, at + 4);
        const double s2 = LoadSingle(base, at + 8);
        const double s3 = LoadSingle(base, at + 12);
        StoreSingle(base, at + 0, MulSingle(s0, gain));
        StoreSingle(base, at + 4, MulSingle(s1, gain));
        StoreSingle(base, at + 8, MulSingle(s2, gain));
        StoreSingle(base, at + 12, MulSingle(s3, gain));
        at += 16;
      }
    }
    // The remainder, one channel at a time, with the gain recomputed identically.
    if (static_cast<int32_t>(done) < static_cast<int32_t>(channels)) {
      ctx.fpscr.disableFlushModeUnconditional();
      const double gain = MulSingle(frame_index, scale);  // fmuls f0,f13,f12 again
      uint32_t at = block + done * 4;
      for (uint32_t i = done; i < channels; i++) {
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, at, MulSingle(LoadSingle(base, at), gain));
        at += 4;
      }
    }
    block += stride;                                  // add r3,r6,r3
    ctx.fpscr.disableFlushModeUnconditional();
    frame_index = AddSingle(frame_index, step);        // fadds f13,f13,f11
  } while (block < end);                              // cmplw ; blt
  ctx.r3.u64 = block;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t block = ctx.r3.u32;
  const uint32_t channels = REX_LOAD_U8(kChannelCountByte);
  const uint32_t bytes = static_cast<uint32_t>(__builtin_rotateleft32(channels, 9));
  if (bytes == 0) {
    spec.read(kChannelCountByte, 1);
    return true;  // writes nothing
  }
  // One contiguous span: 128 frames of `channels` floats, written in place. The channel count
  // is read once before the loop and never reloaded, so the span cannot change mid-call.
  if (bytes > skate3::audio::kPortWatchCap) return false;  // more than the window budget
  if (block + bytes < block) return false;                 // would wrap the address space
  spec.write(block, bytes);
  spec.read(block, bytes);
  spec.read(kChannelCountByte, 1);
  spec.read(kZeroFloat, 4);
  spec.read(kFrameStep, 4);
  spec.read(kRampScale, 4);
  return true;
}

}  // namespace port_82B20E18
SKATE3_PORT(82B20E18, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnR3)
