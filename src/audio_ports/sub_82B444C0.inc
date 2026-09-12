// sub_82B444C0  role: fill one 256-frame block for a mixer object -- repeatedly ask every source in the r4 table for frames (descending pass through the per-source `+4` entry to negotiate a length, ascending pass through `+8` to render, falling back to sub_82B443F8 when a source declines), copy what each round produced into the assembly buffers at +36, advance the running position at +16, and at the end copy the assembled block to +32 and swap it with +28 so the caller always reads from +28   lifted: skate3_recomp.68.cpp:26749 (524 lines)
// STATUS: gate-1 (both source entries are function pointers read out of the table the caller passes: sub_82B444C0 indirect call, `bctrl` at 0x82B4457C and 0x82B44618)
// calls/session: boot 205116  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B444C0.md
namespace port_82B444C0 {

// The block length this function always fills, and the value it leaves at mixer+48.
constexpr uint32_t kBlockFrames = 256;

// The mixer object (r3/r26). Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kPosition = 16;       // f64, advanced by frames/rate every round
constexpr uint32_t kReadyBuffers = 28;   // u32 -> descriptor array the CALLER reads; swapped at exit
constexpr uint32_t kSpareBuffers = 32;   // u32 -> the other half of the pair
constexpr uint32_t kAssembly = 36;       // u32 -> descriptor array each round is copied into
constexpr uint32_t kFormat = 40;         // u32 -> format block; its f32 at +12 is the sample rate
constexpr uint32_t kLastLength = 48;     // u32, the length the last round produced; 256 at exit
constexpr uint32_t kLevel = 52;          // f32, read at +52 and written back at +52
constexpr uint32_t kLevelReset = 56;     // f32, stamped with a pool constant every round
constexpr uint32_t kChannels = 60;       // u8, how many channel buffers the round touched

// A buffer descriptor, the elements of the three arrays above.
constexpr uint32_t kData = 4;    // u32 -> the float data
constexpr uint32_t kStride = 14; // u16, distance between channels, in floats

// The stream object (r5/r24).
constexpr uint32_t kStreamGain = 44;   // f32, reset to the zero constant when source 0 declines
constexpr uint32_t kExtraLimit = 69;   // u8, sources above this index get the flag in r5 set
constexpr uint32_t kLastSource = 70;   // u8, the LAST source index -- both walks run 0..this
constexpr uint32_t kSourceSlots = 80;  // u32 array, indexed 4*(index+20) off the stream

// The per-source descriptor in the r4 table (8-byte entries; only the first word is used here).
constexpr uint32_t kNegotiate = 4;  // u32 function pointer: how many frames can you take
constexpr uint32_t kRender = 8;     // u32 function pointer: produce them

// The object each source slot names; +36 is a running cost counter this function maintains.
constexpr uint32_t kElapsed = 36;

// Both constants computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
// lis r11,-32234 -> 0x82160000 ; lfs f31,23056(r11) -- the same pool zero sub_82B2DAC8 uses.
constexpr uint32_t kZeroFloat = 0x82165A10;
// lis r11,-32206 -> 0x82320000 ; lfs f29,-22460(r11)
constexpr uint32_t kLevelResetValue = 0x8231A844;

constexpr uint32_t kFrameBytes = 256;  // stwu r1,-256(r1)
constexpr uint32_t kSpillSlot = 80;    // std r11,80(r1) ; lfd f13,80(r1) -- inside the frame

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

double LoadDouble(uint8_t* base, uint32_t addr) {  // lfd
  PPCRegister temp{};
  temp.u64 = REX_LOAD_U64(addr);
  return temp.f64;
}

void StoreDouble(uint8_t* base, uint32_t addr, double value) {  // stfd
  PPCRegister temp{};
  temp.f64 = value;
  REX_STORE_U64(addr, temp.u64);
}

// `rlwinm rX,rY,2,0,29` then `add`: the index is truncated to 32 bits and scaled by four (the mask
// clears bits the shift already cleared), and the base is added in 64 bits.
uint64_t FloatSlot(uint64_t array, uint64_t index) {
  return ((uint64_t(uint32_t(index) << 2)) & 0xFFFFFFFCull) + array;
}

// subfc r9,r31,r11 ; eqv r8,r31,r11 ; rlwinm r7,r8,1,31,31 ; addze r10,r7 ; clrlwi r5,r10,31.
// Branchless `(int32_t)limit < (int32_t)index`: the carry out of limit-index answers it whenever
// the sign bits agree, and the eqv term flips the answer when they do not. Both operands are a
// zero-extended byte and a small loop counter here, so it is the plain compare.
uint32_t ExtraFlag(uint32_t limit, uint32_t index) {
  return uint32_t(int32_t(limit) < int32_t(index));
}

// bl 0x82b1f7e8 ; lwz r11,36(r30) ; subf r10,r25,r11 ; add r9,r3,r10 ; stw r9,36(r30).
// The counter at +36 takes (now - started) on top of whatever it holds; every intermediate is a
// 64-bit value and only the store truncates. sub_82B1F7E8 takes no arguments.
void AccumulateElapsed(PPCContext& ctx, uint8_t* base, uint32_t sink, uint64_t started) {
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t now = ctx.r3.u64;
  const uint64_t held = REX_LOAD_U32(sink + kElapsed);
  REX_STORE_U32(sink + kElapsed, uint32_t(now + (held - started)));
}

REX_FUNC(Native) {
  // __savegprlr_16 spills r16-r31 and the link register, and three `stfd`s spill f29-f31, all at
  // negative offsets from the ENTRY r1, i.e. inside the 256 bytes the stwu below claims: not
  // windows. Everything the original keeps in r16-r31 and f29-f31 is a local here, so no
  // callee-saved register is touched at all and the epilogue has nothing to put back.
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;
  ctx.fpscr.disableFlushModeUnconditional();  // stfd f29,-160(r1)

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-256(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t mixer = ctx.r3.u32;    // mr r26,r3
  const uint32_t sources = ctx.r4.u32;  // mr r19,r4 -- the 8-byte-entry table
  const uint32_t stream = ctx.r5.u32;   // mr r24,r5

  const double zero_const = LoadSingle(base, kZeroFloat);           // lfs f31,23056(r11)
  const double level_reset = LoadSingle(base, kLevelResetValue);    // lfs f29,-22460(r11)
  double level = zero_const;     // fmr f30,f31 -- only reloaded from +52 on a round that mixes
  uint64_t produced = 0;         // r22, frames delivered so far
  uint64_t rounds = 0;           // r18, completed descending passes
  uint64_t status = 0;           // r21, the result
  uint64_t channels = 0;         // r20, the channel count the last mixing round used
  uint64_t direct_block = 0;     // r16, set when round 0 delivered all 256 frames by itself
  uint64_t last_request = 0;     // r23, the length handed to the LAST negotiate call

  for (;;) {
    // loc_82B44510 -- one round.
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, mixer + kLevelReset, level_reset);  // stfs f29,56(r26)
    const uint32_t last_source = REX_LOAD_U8(stream + kLastSource);  // lbz r31,70(r24)
    // subfic r29,r22,256 -- how many frames are still wanted.
    uint64_t request = uint64_t(kBlockFrames) - produced;
    last_request = 0;  // mr r23,r17
    // cmpwi cr6,r31,0 ; blt -- a zero-extended byte is never negative, so this guard is dead and
    // the descending walk always runs; it runs last_source+1 times, index last_source down to 0.

    // loc_82B4453C -- descending: ask each source how much it can take, narrowing the request.
    for (int32_t source = int32_t(last_source); source >= 0; source--) {
      skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);  // bl 0x82b1f7e8
      const uint64_t started = ctx.r3.u64;                // mr r25,r3
      const uint32_t limit = REX_LOAD_U8(stream + kExtraLimit);  // lbz r11,69(r24)
      // add r27,r11,r19 / add r28,r10,r24, then addi -8 / -4 per pass: the descriptor table is
      // 8 bytes a slot and the stream's slot array 4, both walked downwards from `last_source`.
      const uint64_t desc_slot = ((uint64_t(uint32_t(source) << 3)) & 0xFFFFFFF8ull) + sources;
      const uint64_t stream_slot =
          ((uint64_t(uint32_t(source + 20) << 2)) & 0xFFFFFFFCull) + stream;
      const uint32_t descriptor = REX_LOAD_U32(uint32_t(desc_slot));  // lwz r10,0(r27)
      const uint32_t sink = REX_LOAD_U32(uint32_t(stream_slot));      // lwz r30,0(r28)
      ctx.ctr.u64 = REX_LOAD_U32(descriptor + kNegotiate);  // lwz r11,4(r10) ; mtctr r11
      ctx.r3.u64 = sink;                                    // mr r3,r30
      ctx.r4.u64 = mixer;                                   // mr r4,r26
      ctx.r5.u64 = ExtraFlag(limit, uint32_t(source));      // clrlwi r5,r10,31
      ctx.r6.u64 = request;                                 // mr r6,r29
      last_request = request;                               // mr r23,r29
      ctx.lr = 0x82B44580;                                  // bctrl
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      // mr r29,r3 ; cmpwi cr6,r3,256 ; blt ; li r29,256 -- a SIGNED clamp, so a negative answer
      // is carried forward unchanged and becomes a huge unsigned request next pass.
      request = ctx.r3.u64;
      if (int32_t(ctx.r3.s32) >= int32_t(kBlockFrames)) request = kBlockFrames;

      // cmplwi cr6,r18,0 ; bne -- only the first round clears the cost counter.
      if (rounds == 0) REX_STORE_U32(sink + kElapsed, 0);  // stw r17,36(r30)
      AccumulateElapsed(ctx, base, sink, started);
    }

    // loc_82B445C0
    rounds += 1;  // addi r18,r18,1
    // cmplwi cr6,r11,0 ; blt -- dead again (an unsigned compare cannot be less than zero), so the
    // ascending walk always runs at least once.

    // loc_82B445D4 -- ascending: render, and take the fallback when a source declines.
    for (uint64_t source = 0;; source++) {
      skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);  // bl 0x82b1f7e8
      const uint64_t started = ctx.r3.u64;                // mr r29,r3
      const uint32_t limit = REX_LOAD_U8(stream + kExtraLimit);  // lbz r10,69(r24)
      // lwzx r6,r11,r19 and lwzx r30,r3,r24 -- the same two arrays, addressed by index this time,
      // and the lwzx sums wrap at 32 bits.
      const uint32_t descriptor =
          REX_LOAD_U32(uint32_t((uint32_t(source) << 3) & 0xFFFFFFF8u) + sources);
      const uint32_t sink =
          REX_LOAD_U32(uint32_t((uint32_t(source + 20) << 2) & 0xFFFFFFFCu) + stream);
      ctx.ctr.u64 = REX_LOAD_U32(descriptor + kRender);  // lwz r9,8(r6) ; mtctr r9
      ctx.r3.u64 = sink;                                 // mr r3,r30
      ctx.r4.u64 = mixer;                                // mr r4,r26
      ctx.r5.u64 = ExtraFlag(limit, uint32_t(source));   // clrlwi r5,r10,31
      ctx.lr = 0x82B4461C;                               // bctrl
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      status = ctx.r3.u64;  // mr r21,r3

      // cmpwi cr6,r3,0 ; bne cr6,0x82b44684
      if (ctx.r3.s32 == 0) {
        // The source produced nothing: the fallback fills in for it, with the length the LAST
        // negotiate call was given.
        skate3::audio::GuestCall(ctx, base, sub_82B443F8, mixer, stream, sink, last_request);
        status = ctx.r3.u64;  // mr r21,r3
        // cmpwi cr6,r3,0 ; bne cr6,0x82b44690
        if (ctx.r3.s32 == 0) {
          // loc_82B4464C -- the fallback declined too: charge the time and leave the walk.
          AccumulateElapsed(ctx, base, sink, started);
          break;
        }
      } else if (source == 0) {
        // loc_82B44684 -- cmpwi cr6,r31,0 ; bne. Only source 0 resets the stream's gain.
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, stream + kStreamGain, zero_const);  // stfs f31,44(r24)
      }

      // loc_82B44690
      AccumulateElapsed(ctx, base, sink, started);
      // lbz r8,70(r24) ; cmpw cr6,r31,r8 ; ble -- the bound is RE-READ after every render.
      if (int32_t(source + 1) > int32_t(REX_LOAD_U8(stream + kLastSource))) break;
    }

    // loc_82B4465C
    // cmpwi cr6,r21,1 ; bne cr6,0x82b44768 -- anything but 1 ends the block.
    if (int32_t(status) != 1) {
      // loc_82B44768 -- cmplwi cr6,r22,0 ; beq: nothing produced at all leaves the buffers alone.
      // Every compare against r22 is `cmplwi`, i.e. on the LOW WORD of a running 64-bit sum.
      if (uint32_t(produced) != 0) {
        const uint32_t assembly = REX_LOAD_U32(mixer + kAssembly);  // lwz r30,36(r26)
        // cmplwi cr6,r20,0 ; beq cr6,0x82b447b8
        if (channels != 0) {
          // subfic r11,r22,256 ; rlwinm r29,r11,2,0,29 -- the byte count of the unfilled tail.
          const uint64_t tail_bytes =
              (uint64_t(uint32_t(uint64_t(kBlockFrames) - produced) << 2)) & 0xFFFFFFFCull;
          for (uint64_t channel = 0; channel < channels; channel++) {  // cmplw r31,r20 ; blt
            const uint64_t stride = REX_LOAD_U16(assembly + kStride);      // lhz r11,14(r30)
            const uint64_t data = REX_LOAD_U32(assembly + kData);          // lwz r10,4(r30)
            // mullw r11,r11,r31 ; add r9,r11,r22 -- 64-bit product, 64-bit sum, truncated by the
            // rlwinm that scales it.
            const uint64_t index =
                uint64_t(int64_t(int32_t(stride)) * int64_t(int32_t(channel))) + produced;
            // bl 0x82ee5e80 -- memset(dst, 0, bytes)
            skate3::audio::GuestCall(ctx, base, sub_82EE5E80, uint32_t(FloatSlot(data, index)), 0,
                                     uint32_t(tail_bytes));
          }
        }
        // loc_82B447B8
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, mixer + kLevel, level);           // stfs f30,52(r26)
        status = 1;                                         // li r21,1
        REX_STORE_U8(mixer + kChannels, uint8_t(channels)); // stb r20,60(r26)
      }
      break;  // -> loc_82B447C4
    }

    const uint32_t round_channels = REX_LOAD_U8(mixer + kChannels);   // lbz r28,60(r26)
    const uint32_t round_frames = REX_LOAD_U32(mixer + kLastLength);  // lwz r25,48(r26)
    bool mix = true;
    // cmplwi cr6,r22,0 ; bne ; cmpwi cr6,r25,256 ; bne -- the first round delivering a whole block
    // means nothing was ever staged, so every copy below is skipped for the rest of the call.
    if (uint32_t(produced) == 0 && int32_t(round_frames) == int32_t(kBlockFrames)) {
      direct_block = 1;  // li r16,1
    } else if (int32_t(round_frames) == 0) {
      mix = false;  // loc_82B446B8 -> loc_82B44758
    }

    if (mix) {
      // loc_82B446C0 -- clrlwi r11,r16,24 ; bne cr6,0x82b44724
      if ((direct_block & 0xFF) == 0) {
        const uint32_t from = REX_LOAD_U32(mixer + kReadyBuffers);  // lwz r30,28(r26)
        const uint32_t into = REX_LOAD_U32(mixer + kAssembly);      // lwz r29,36(r26)
        // rlwinm r27,r25,2,0,29 -- this round's frames, in bytes.
        const uint64_t round_bytes = (uint64_t(round_frames << 2)) & 0xFFFFFFFCull;
        // cmplwi cr6,r28,0 ; beq cr6,0x82b44724
        for (uint64_t channel = 0; channel < round_channels; channel++) {  // cmplw r31,r28 ; blt
          const uint64_t into_stride = REX_LOAD_U16(into + kStride);  // lhz r11,14(r29)
          const uint64_t from_stride = REX_LOAD_U16(from + kStride);  // lhz r8,14(r30)
          const uint64_t from_data = REX_LOAD_U32(from + kData);      // lwz r10,4(r30)
          const uint64_t into_data = REX_LOAD_U32(into + kData);      // lwz r9,4(r29)
          const uint64_t into_index =
              uint64_t(int64_t(int32_t(into_stride)) * int64_t(int32_t(channel))) + produced;
          const uint64_t from_index =
              uint64_t(int64_t(int32_t(from_stride)) * int64_t(int32_t(channel)));
          // bl 0x82edf460 -- memcpy(dst, src, bytes); the destination is offset by what earlier
          // rounds already produced, the source always starts at the buffer's own origin.
          skate3::audio::GuestCall(ctx, base, sub_82EDF460, uint32_t(FloatSlot(into_data, into_index)),
                                   uint32_t(FloatSlot(from_data, from_index)), uint32_t(round_bytes));
        }
      }

      // loc_82B44724 -- advance the position by this round's duration.
      const uint32_t format = REX_LOAD_U32(mixer + kFormat);  // lwz r10,40(r26)
      ctx.fpscr.disableFlushModeUnconditional();
      const double position = LoadDouble(base, mixer + kPosition);  // lfd f0,16(r26)
      channels = round_channels;                                    // mr r20,r28
      // extsw r11,r25 ; std r11,80(r1) ; lfd f13,80(r1) ; fcfid f12,f13 -- the int-to-double path
      // spills through this call's OWN frame, which is not a window.
      REX_STORE_U64(ctx.r1.u32 + kSpillSlot, uint64_t(int64_t(int32_t(round_frames))));
      PPCRegister spill{};
      spill.u64 = REX_LOAD_U64(ctx.r1.u32 + kSpillSlot);
      const double frames_f = double(spill.s64);
      level = LoadSingle(base, mixer + kLevel);                   // lfs f30,52(r26)
      const double rounded = double(float(frames_f));             // frsp f11,f12
      const double rate = LoadSingle(base, format + 12);          // lfs f10,12(r10)
      const double seconds = double(float(rounded / rate));       // fdivs f9,f11,f10
      // fadd, NOT fadds: the sum keeps double precision.
      StoreDouble(base, mixer + kPosition, seconds + position);   // stfd f8,16(r26)
    }

    // loc_82B44758 -- add r22,r25,r22 ; cmplwi cr6,r22,256 ; blt
    produced = uint64_t(round_frames) + produced;
    if (uint32_t(produced) >= kBlockFrames) break;
  }

  // loc_82B447C4 -- clrlwi r11,r16,24 ; bne cr6,0x82b44838
  if ((direct_block & 0xFF) == 0) {
    const uint32_t count = REX_LOAD_U8(mixer + kChannels);      // lbz r11,60(r26)
    const uint32_t from = REX_LOAD_U32(mixer + kAssembly);      // lwz r30,36(r26)
    const uint32_t into = REX_LOAD_U32(mixer + kSpareBuffers);  // lwz r29,32(r26)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b44828 -- the bound is re-read at the bottom of every pass,
    // so the first read only decides whether the loop is entered at all.
    if (count != 0) {
      for (uint64_t channel = 0;; channel++) {
        const uint64_t from_stride = REX_LOAD_U16(from + kStride);  // lhz r11,14(r30)
        const uint64_t into_stride = REX_LOAD_U16(into + kStride);  // lhz r9,14(r29)
        const uint64_t from_data = REX_LOAD_U32(from + kData);      // lwz r8,4(r30)
        const uint64_t into_data = REX_LOAD_U32(into + kData);      // lwz r10,4(r29)
        const uint64_t from_index =
            uint64_t(int64_t(int32_t(from_stride)) * int64_t(int32_t(channel)));
        const uint64_t into_index =
            uint64_t(int64_t(int32_t(into_stride)) * int64_t(int32_t(channel)));
        // bl 0x82edf460 -- memcpy of a whole 256-frame channel, li r5,1024.
        skate3::audio::GuestCall(ctx, base, sub_82EDF460, uint32_t(FloatSlot(into_data, into_index)),
                                 uint32_t(FloatSlot(from_data, from_index)), 1024);
        // lbz r5,60(r26) ; cmplw cr6,r31,r5 ; blt
        if (channel + 1 >= REX_LOAD_U8(mixer + kChannels)) break;
      }
    }
    // loc_82B44828 -- swap the ready pair with the spare pair, both re-read here.
    const uint32_t spare = REX_LOAD_U32(mixer + kSpareBuffers);  // lwz r11,32(r26)
    const uint32_t ready = REX_LOAD_U32(mixer + kReadyBuffers);  // lwz r10,28(r26)
    REX_STORE_U32(mixer + kReadyBuffers, spare);                 // stw r11,28(r26)
    REX_STORE_U32(mixer + kSpareBuffers, ready);                 // stw r10,32(r26)
  }

  // loc_82B44838
  REX_STORE_U32(mixer + kLastLength, kBlockFrames);  // li r11,256 ; stw r11,48(r26)
  ctx.r3.u64 = status;                               // mr r3,r21
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;             // addi r1,r1,256
  ctx.fpscr.disableFlushModeUnconditional();         // lfd f29,-160(r1) and its two neighbours
  ctx.lr = entry_lr;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B444C0
SKATE3_PORT(82B444C0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
