// sub_82B23B50  role: per-channel gain ramp over a double-buffered block -- ramp every channel from the applied gain (+56) toward the target (+52) with sub_82B3C098, then swap the descriptor pair at +28/+32 and latch applied = target   lifted: skate3_recomp.67.cpp:14525 (119 lines)
// STATUS: verified
// calls/session: boot 292165  play 435555  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B23B50.md
namespace port_82B23B50 {

// Arguments: r3 = the gain object, r4 = the descriptor pair, r5 = a byte flag (non-zero means
// "start from the target", i.e. no ramp this block). Nothing in docs/rw_audio_structs.h names
// either object, so these constants describe the arithmetic only -- not a recovered layout.
constexpr uint32_t kChannelCount = 42;  // lbz r27,42(r30) -- u8, the loop trip count
constexpr uint32_t kTargetGain = 52;    // lfs f0,52(r3)
constexpr uint32_t kAppliedGain = 56;   // lfs f13,56(r30) / stfs f0,56(r30)
constexpr uint32_t kSourceDesc = 28;    // lwz r29,28(r4) -- read side this block
constexpr uint32_t kDestDesc = 32;      // lwz r28,32(r4) -- write side this block
constexpr uint32_t kDescBuffer = 4;     // lwz r10,4(r29) -- channel 0's float run
constexpr uint32_t kDescStride = 14;    // lhz r11,14(r29) -- u16 singles between channels

// The per-sample step scale, a rodata single. Computed as ((lis_imm & 0xFFFF) << 16) + offsets,
// never read off by eye: lis r11,-32208 -> 0x82300000 ; addi r10,r11,-31232 -> 0x822F8600 ;
// lfs f0,480(r10) -> 0x822F87E0. The value is loaded through the guest map, never assumed.
constexpr uint32_t kStepScale = 0x822F87E0;

// sub_82B3C098 writes one fixed 1024-byte block per call (eight 128-byte groups) and reads the
// same span from its source. Its own port file enumerates the identical shape.
constexpr uint32_t kBlockBytes = 1024;

// The harness allows 32 spans and 32 KB. Two spans here are the object's own, so at most 30
// channel blocks fit the span count and 28 fit the byte budget; 24 keeps both comfortable.
constexpr uint32_t kMaxEnumerableChannels = 24;

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: round the double back to a single and store its bits. The load/store pair is kept as a
// round trip, not as a word copy, because that is what the original executes.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fsubs / fmuls: double arithmetic rounded to single. Two separate operations -- the lifted lines
// are fsubs then fmuls, never a fused multiply-subtract.
inline double Fsubs(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double Fmuls(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return a < b + blen && b < a + alen;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r30,r3
  const uint32_t pair = ctx.r4.u32;    // mr r26,r4
  const uint32_t restart = ctx.r5.u32 & 0xFF;                        // clrlwi r11,r5,24
  const uint32_t source = REX_LOAD_U32(pair + kSourceDesc);          // lwz r29,28(r4)
  const uint32_t dest = REX_LOAD_U32(pair + kDestDesc);              // lwz r28,32(r4)

  // cmplwi cr6,r11,0 ; beq cr6,0x82b23b80
  if (restart != 0) {
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, object + kAppliedGain,
                LoadSingle(base, object + kTargetGain));  // lfs f0,52(r3) ; stfs f0,56(r3)
  }

  // loc_82B23B80: the ramp step, computed once for the whole loop.
  ctx.fpscr.disableFlushModeUnconditional();
  const double target = LoadSingle(base, object + kTargetGain);    // lfs f0,52(r30)
  const double applied = LoadSingle(base, object + kAppliedGain);  // lfs f13,56(r30)
  const uint32_t channels = REX_LOAD_U8(object + kChannelCount);   // lbz r27,42(r30)
  const double span = Fsubs(target, applied);                      // fsubs f12,f0,f13
  const double scale = LoadSingle(base, kStepScale);               // lfs f0,480(r10)
  // fmuls f2,f12,f0 -- f2 is sub_82B3C098's per-sample increment argument and is set ONCE, as
  // in the original. The callee writes f0 and f8-f13 but never f1 or f2, so it survives the
  // loop; that is the original's own assumption, reproduced rather than corrected.
  ctx.f2.f64 = Fmuls(span, scale);

  // cmplwi cr6,r27,0 ; beq cr6,0x82b23be8 -- the count is pre-tested, so the do-while below is
  // exactly this for-loop.
  for (uint32_t channel = 0; channel < channels; channel++) {  // loc_82B23BAC .. blt cr6
    const uint32_t source_stride = REX_LOAD_U16(source + kDescStride);  // lhz r11,14(r29)
    ctx.fpscr.disableFlushModeUnconditional();
    // lfs f1,56(r30) -- re-read every iteration, and passed as the callee's starting gain.
    ctx.f1.f64 = LoadSingle(base, object + kAppliedGain);
    const uint32_t dest_stride = REX_LOAD_U16(dest + kDescStride);      // lhz r9,14(r28)
    // mullw r7,r11,r31 / mullw r6,r9,r31: RexGlue's mullw is a 64-bit product of the
    // sign-extended words, so the intermediate stays 64-bit here.
    const int64_t source_index =
        static_cast<int64_t>(static_cast<int32_t>(source_stride)) * static_cast<int64_t>(static_cast<int32_t>(channel));
    const uint32_t source_buffer = REX_LOAD_U32(source + kDescBuffer);  // lwz r10,4(r29)
    const uint32_t dest_buffer = REX_LOAD_U32(dest + kDescBuffer);      // lwz r8,4(r28)
    const int64_t dest_index =
        static_cast<int64_t>(static_cast<int32_t>(dest_stride)) * static_cast<int64_t>(static_cast<int32_t>(channel));
    // rlwinm r9,r7,2,0,29 / rlwinm r11,r6,2,0,29: the low word shifted left by two, the rotate's
    // wrap masked away, zero-extended into the 64-bit register.
    const uint64_t source_offset =
        (static_cast<uint64_t>(static_cast<uint32_t>(source_index)) << 2) & 0xFFFFFFFCull;
    const uint64_t dest_offset =
        (static_cast<uint64_t>(static_cast<uint32_t>(dest_index)) << 2) & 0xFFFFFFFCull;
    // add r4,r9,r10 / add r3,r11,r8: a 64-bit add of two zero-extended words. A carry into bit
    // 32 is invisible in memory but lives in the register the callee receives, so the argument
    // is passed as the full 64-bit sum.
    const uint64_t source_run = source_offset + static_cast<uint64_t>(source_buffer);
    const uint64_t dest_run = dest_offset + static_cast<uint64_t>(dest_buffer);
    // bl 0x82b3c098 -- through GuestCall on the live context, so the callee's own hook (its port
    // is pending) behaves exactly as under the original caller.
    skate3::audio::GuestCall(ctx, base, sub_82B3C098, dest_run, source_run);
  }

  // loc_82B23BE8: swap the pair, then latch the gain. Both words are read before either store,
  // as lifted.
  const uint32_t swapped_source = REX_LOAD_U32(pair + kDestDesc);  // lwz r11,32(r26)
  ctx.r3.s64 = 1;                                                  // li r3,1
  const uint32_t swapped_dest = REX_LOAD_U32(pair + kSourceDesc);  // lwz r10,28(r26)
  REX_STORE_U32(pair + kSourceDesc, swapped_source);               // stw r11,28(r26)
  REX_STORE_U32(pair + kDestDesc, swapped_dest);                   // stw r10,32(r26)
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, object + kAppliedGain,
              LoadSingle(base, object + kTargetGain));  // lfs f0,52(r30) ; stfs f0,56(r30)
}

// Every byte the call writes, from ENTRY state only: the applied gain, the two swapped
// descriptor pointers, and one 1024-byte block per channel through sub_82B3C098. The callee's
// shape is fixed (eight dcbzl'd 128-byte groups from its r3), so each block is
// [dest & ~127, (dest & ~0xF) + 1024) -- the same single span its own port declares.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  const uint32_t source = REX_LOAD_U32(pair + kSourceDesc);
  const uint32_t dest = REX_LOAD_U32(pair + kDestDesc);
  const uint32_t channel_count = REX_LOAD_U8(object + kChannelCount);
  if (channel_count > kMaxEnumerableChannels) return false;  // over the span and byte budgets

  // Declared unconditionally: the flag path and the tail store the same word.
  spec.write(object + kAppliedGain, 4);
  spec.write(pair + kSourceDesc, 8);  // +28 and +32, swapped

  const uint32_t source_stride = REX_LOAD_U16(source + kDescStride);
  const uint32_t dest_stride = REX_LOAD_U16(dest + kDescStride);
  const uint32_t source_buffer = REX_LOAD_U32(source + kDescBuffer);
  const uint32_t dest_buffer = REX_LOAD_U32(dest + kDescBuffer);

  // The loop re-reads both descriptors and the applied gain on every iteration, so if a block
  // covered any of those words the later addresses would be produced during the call and this
  // enumeration would be wrong. Predicted here rather than assumed away.
  for (uint32_t channel = 0; channel < channel_count; channel++) {
    const uint32_t block = dest_buffer + ((dest_stride * channel) << 2);
    const uint32_t zero_begin = block & ~127u;
    const uint32_t store_end = (block & ~0xFu) + kBlockBytes;
    if (store_end < zero_begin) return false;  // a block within 1 KB of the top of the map
    const uint32_t length = store_end - zero_begin;
    if (Overlaps(zero_begin, length, object + kTargetGain, 8) ||
        Overlaps(zero_begin, length, pair + kSourceDesc, 8) ||
        Overlaps(zero_begin, length, source + kDescBuffer, 12) ||
        Overlaps(zero_begin, length, dest + kDescBuffer, 12)) {
      return false;  // a written block aliases a word the loop re-reads
    }
    spec.write(zero_begin, length);
    spec.read(source_buffer + ((source_stride * channel) << 2), kBlockBytes);
  }

  // The cells sub_82B3C098 reads. Copied here rather than referenced across ports, which the
  // lint forbids and which would break if that port changed: if sub_82B3C098's constants move,
  // move these too. Declared so this port's vectors carry what its callee reaches.
  spec.read(0x82257308, 4);   // the step scale
  spec.read(0x820ED958, 4);   // the ramp span
  spec.read(0x82060C50, 4);   // lane 2's scale
  spec.read(0x82063B08, 4);   // lane 3's scale
  spec.read(0x8231BA20, 64);  // the group multipliers 5..8
  spec.read(0x8231BA60, 64);  // the group multipliers 1..4
  spec.read(object + kChannelCount, 1);
  spec.read(object + kTargetGain, 8);
  spec.read(pair + kSourceDesc, 8);
  spec.read(source, 16);
  spec.read(dest, 16);
  spec.read(kStepScale, 4);
  return true;
}

}  // namespace port_82B23B50
SKATE3_PORT(82B23B50, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
