// sub_82B50380  role: 16-bit PCM out of a wrapping ring into float32 -- `first` samples from src+offset then `wrap` from src, mono (r7 == 1) into one buffer, or two interleaved channels de-interleaved by vperm128 into two   lifted: skate3_recomp.68.cpp:55928 (1284 lines)
// STATUS: gate-2 (address registers reassigned mid-body; windows not derivable from entry state -- measured, see docs/PLAN.md)
// calls/session: boot 163060  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B50380.md
namespace port_82B50380 {

// Arguments, read straight off the lifted entry. Nothing in docs/rw_audio_structs.h names any of
// these buffers, so there are no offset constants: the structures are "an array of int16" and
// "an array of float".
//
//   r3  dst          channel 0 floats out              (mr r11,r3)
//   r4  dst_second   channel 1 floats out, stereo only (stw r4,-128(r1) ; mr r7,r4)
//   r5  src          the 16-bit ring's base
//   r6  offset       byte offset into the ring where this block starts
//   r7  channels     1 takes the mono path; anything else takes loc_82B505D8
//   r8  first        samples (mono) or frames (stereo) from the offset to the ring's end
//   r9  wrap         ...and the count that wrapped back to the ring base
//
// The measured entry state (probe/harness/out/p17_windows.log, 24 logged calls) is mono every
// time: r7 = 1, r8 = 0x87, r9 = 0 except on the wrap call where r8 = 0x7E and r9 = 0x09, and r6
// stepping by 0x10E = 2*0x87 until it wraps at 0x1800. So the ring is 3072 samples and one call
// converts one 135-sample decode block. Neither the stereo path nor a non-zero `wrap` on the
// vector path has ever been observed running.

// lfs f0,-17516(r8) after lis r8,-32206: ((imm & 0xFFFF) << 16) + offset, never read off by eye.
// 0x8232 << 16 = 0x82320000, less 17516 = 0x8231BB94. Almost certainly 1/32768 -- it is the
// scalar twin of the vcsxwfp128 <-,15 the vector paths use -- but the body loads it rather than
// assuming it.
constexpr uint32_t kScaleWord = ((static_cast<uint32_t>(-32206) & 0xFFFFu) << 16) +
                                static_cast<uint32_t>(-17516);  // 0x8231BB94

// stw r5,36(r1) and stw r9,68(r1). Positive offsets off r1 are still the CALLER's frame: this
// function has no stwu, so these two words are its outgoing-parameter save area, and the stereo
// path reloads src and wrap out of them after clobbering both registers.
constexpr uint32_t kArgSource = 36;
constexpr uint32_t kArgWrap = 68;

// addi r30,r1,-80: the 16-byte vperm128 control the stereo path builds on its own frame.
constexpr uint32_t kControlSlot = 80;

// Bytes per iteration of each vector loop, and the group width every unaligned store pair covers.
constexpr uint32_t kGroup = 16;
constexpr uint32_t kMonoBlockBytes = 32;    // 16 samples in, 64 bytes of float out
constexpr uint32_t kStereoBlockBytes = 128; // 32 frames in, 128 bytes of float out per channel

// ---- one helper per lifted vector mnemonic ---------------------------------------------------

// lvlx128 vD,rA,rB: the bytes from ea up to the next 16-byte boundary, left-justified, with the
// big-endian lane reversal RexGlue applies to every vector memory access.
inline simde__m128i Lvlx(uint8_t* base, uint32_t ea) {
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)),
      simde_mm_load_si128((simde__m128i*)&VectorMaskL[(ea & 0xF) * 16]));
}

// lvrx128 vD,rA,rB: the bytes from the previous 16-byte boundary up to ea, right-justified. An
// already-aligned ea reads nothing at all and yields zero -- the lifted ternary, kept, because
// the aligned case is the one that actually runs.
inline simde__m128i Lvrx(uint8_t* base, uint32_t ea) {
  return (ea & 0xF) ? simde_mm_shuffle_epi8(
                          simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)),
                          simde_mm_load_si128((simde__m128i*)&VectorMaskR[(ea & 0xF) * 16]))
                    : simde_mm_setzero_si128();
}

// lvx128 vD,rA,rB: the aligned line containing ea, same lane reversal.
inline simde__m128i Lvx(uint8_t* base, uint32_t ea) {
  return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)),
                               simde_mm_load_si128((simde__m128i*)VectorMaskL));
}

// stvlx128 vS,rA,rB: ea up to the next 16-byte boundary, taken from the TOP of the register
// downwards -- u8[15] lands at ea, u8[14] at ea+1.
inline void Stvlx(uint8_t* base, uint32_t ea, simde__m128i value) {
  alignas(16) uint8_t bytes[16];
  simde_mm_store_si128((simde__m128i*)bytes, value);
  for (size_t i = 0; i < (16 - (ea & 0xF)); i++) REX_STORE_U8(ea + i, bytes[15 - i]);
}

// stvrx128 vS,rA,rB: the previous 16-byte boundary up to ea, taken from the BOTTOM of the
// register upwards and written backwards from ea-1 -- u8[0] lands at ea-1, u8[1] at ea-2. The
// OPPOSITE lane order to stvlx128, which is the hazard docs/PLAN.md names for this kernel.
inline void Stvrx(uint8_t* base, uint32_t ea, simde__m128i value) {
  alignas(16) uint8_t bytes[16];
  simde_mm_store_si128((simde__m128i*)bytes, value);
  for (size_t i = 0; i < (ea & 0xF); i++) REX_STORE_U8(ea - i - 1, bytes[i]);
}

// vupkhsb128 vD,vS,v96 / vupklsb128 vD,vS,v96: RexGlue lowers both as a 16->32 signed widening,
// of the high half and the low half of the host register. The v96 operand is unused by the
// lowering, so it is not a parameter here.
inline simde__m128i Vupkhsb(simde__m128i s) {
  return simde_mm_cvtepi16_epi32(simde_mm_unpackhi_epi64(s, s));
}
inline simde__m128i Vupklsb(simde__m128i s) { return simde_mm_cvtepi16_epi32(s); }

// vcsxwfp128 vD,vS,15: signed word with 15 fraction bits to single. A multiply by 2^-15
// (0x38000000) and a SEPARATE multiply -- there is no addend and nothing to fuse it with.
inline simde__m128 Vcsxwfp15(simde__m128i s) {
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(s),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x38000000))));
}

// vperm128 vD,vA,vB,vC. Both source operands are the same register at every use here.
inline simde__m128i Vperm(simde__m128i a, simde__m128i b, simde__m128i control) {
  return rex::ppc::simde_mm_perm_epi8_(a, b, control);
}

// dcbz rA,rB: RexGlue models it as a 32-byte memset of the line containing the address. Xenon's
// dcbz establishes 128 bytes, so the lifted model zeroes a quarter of what the hardware does --
// this reproduces the lifted model, which is what executes, not the hardware.
inline void Dcbz(uint8_t* base, uint32_t ea) {
  memset((void*)REX_RAW_ADDR(ea & ~31u), 0, 32);
}

// ---- the two composite idioms the loops are built out of -------------------------------------

// lvlx128 + lvrx128 + vor128: the sixteen bytes at ea, however aligned. All of a loop's loads
// happen before any of its stores, so gathering each group's pair together is the lifted order
// with the independent halves adjacent.
inline simde__m128i LoadGroup(uint8_t* base, uint32_t ea) {
  return simde_mm_or_si128(Lvlx(base, ea), Lvrx(base, ea + kGroup));
}

// stvlx128 vS,r0,rB then stvrx128 vS,rB,r(16): together exactly [ea, ea+16), since the left form
// writes ea..boundary and the right form writes boundary..ea+16. Emitting them as a pair keeps
// the lifted stream order: group g's stvrx and group g+1's stvlx both target ea+16 and are
// adjacent there too.
inline void StoreGroup(uint8_t* base, uint32_t ea, simde__m128 value) {
  const simde__m128i bits = simde_mm_castps_si128(value);
  Stvlx(base, ea, bits);
  Stvrx(base, ea + kGroup, bits);
}

// vupkhsb128/vupklsb128 then vcsxwfp128 <-,15 on each half. Flush mode is already enabled by the
// caller at the lifted point; the two widenings are integer ops and unaffected by it, which is
// what makes emitting them after the toggle instead of before it bit-identical.
struct Halves {
  simde__m128 high;  // vupkhsb128 -- mono: the group's first four samples; stereo: channel 0
  simde__m128 low;   // vupklsb128 -- mono: its second four;                stereo: channel 1
};
inline Halves Convert(simde__m128i group) {
  return {Vcsxwfp15(Vupkhsb(group)), Vcsxwfp15(Vupklsb(group))};
}

// lhz/lhzu + extsh, then std -80(r1) / lfd / fcfid / frsp / fmuls f0. The stack round trip is a
// GPR->FPR move of the sign-extended halfword on this function's own frame, so it is kept in a
// register; fcfid then frsp then a single-precision multiply are reproduced as three steps.
inline double SampleToSingle(int32_t sample, double scale) {
  const double wide = static_cast<double>(static_cast<int64_t>(sample));  // fcfid
  const double narrow = static_cast<double>(static_cast<float>(wide));    // frsp
  return static_cast<double>(static_cast<float>(narrow * scale));         // fmuls
}

// stfs / stfsu: the double rounded back to a single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// lhz then extsh.
inline int32_t LoadSample(uint8_t* base, uint32_t address) {
  return static_cast<int16_t>(REX_LOAD_U16(address));
}

REX_FUNC(Native) {
  const uint32_t sp = ctx.r1.u32;           // unchanged throughout: this function has no stwu
  const uint32_t dst = ctx.r3.u32;          // mr r11,r3
  const uint32_t dst_second = ctx.r4.u32;   // stw r4,-128(r1) ; mr r7,r4
  const uint32_t src = ctx.r5.u32;
  const int32_t channels = ctx.r7.s32;
  const int32_t first = ctx.r8.s32;
  const int32_t wrap = ctx.r9.s32;

  // The two words in the caller's outgoing-parameter area. This body keeps src and wrap in
  // registers and still writes them, because they are real guest writes above r1 that a window
  // builder would have to declare -- unlike the -80(r1) .. -128(r1) scratch below, which is this
  // function's own frame and excluded by the brief.
  REX_STORE_U32(sp + kArgSource, src);                        // stw r5,36(r1)
  REX_STORE_U32(sp + kArgWrap, static_cast<uint32_t>(wrap));   // stw r9,68(r1)

  // srawi r10,r6,1 ; rlwinm r10,r10,1,0,30 ; add r10,r10,r5 -- the read cursor: the offset
  // rounded toward zero to an even byte count. The rotate's wrapped bit 31 is masked off, so the
  // rlwinm is just a masked shift. The lifted add is 64-bit and the result can carry into bit 32,
  // but every use of r10 reads .u32, so a 32-bit cursor is faithful.
  const uint32_t read = src + ((static_cast<uint32_t>(ctx.r6.s32 >> 1) << 1) & 0xFFFFFFFEu);

  // Four counted dcbt loops (loc_82B503D4, loc_82B50404, loc_82B50608, loc_82B50638) are elided.
  // dcbt lifts to nothing, and each loop's only other effect is on r3/r4/r6/r7/r31 and ctr: r31
  // is restored by __restgprlr_25, and every one of the others is written again before it is next
  // read (li r7,0 / li r7,16 on the mono path, li r3,112 at loc_82B50678 on the stereo one).

  // cmpwi cr6,r7,1 ; bne cr6,0x82b505d8
  if (channels == 1) {
    // ---- the mono path: `first` samples at `read`, then `wrap` samples at `src`, one buffer ----

    // loc_82B50414: clrlwi r7,r11,25 ; bne ; add r7,r8,r9 ; cmpwi 32 ; blt. dcbz establishes the
    // destination, but only when it is 128-byte aligned and there are at least 32 samples.
    const uint32_t total = ctx.r8.u32 + ctx.r9.u32;  // add r7,r8,r9
    if ((dst & 0x7Fu) == 0 && static_cast<int32_t>(total) >= 32) {
      // rlwinm r6,r7,27,5,31 ; mtctr r6. total >= 32 makes the count at least one, so the
      // do/while is the bdnz; a count of zero would run 2^32 times in the original too.
      uint32_t lines = (total >> 5) & 0x7FFFFFFu;
      uint32_t at = 0;
      do {
        Dcbz(base, at + dst);  // dcbz r7,r11
        at += 128;             // addi r7,r7,128
      } while (--lines != 0);  // bdnz 0x82b50438
    }

    // loc_82B50444: li r7,16 ; li r6,32 ; li r4,48 ; li r3,64 -- the four index registers every
    // store from here on uses. The census's `ea = r11 + r3` is dst + 64, not dst + the entry r3.
    uint32_t out = dst;    // r11
    uint32_t in = read;    // r10
    int32_t tail = first;  // mr r31,r8, unless the vector loop recomputes it

    // cmpwi cr6,r8,16 ; blt cr6,0x82b504e4
    if (first >= 16) {
      // rlwinm r30,r8,28,4,31 ; rlwinm r31,r8,0,0,27 ; subf r31,r31,r8 ; mtctr r30
      uint32_t blocks = (static_cast<uint32_t>(first) >> 4) & 0xFFFFFFFu;
      tail = first - static_cast<int32_t>(static_cast<uint32_t>(first) & 0xFFFFFFF0u);
      do {
        // loc_82B50470. Two unaligned 16-byte groups in, four float vectors out.
        const simde__m128i g0 = LoadGroup(base, in);           // lvlx128 v63 / lvrx128 v62 / vor
        const simde__m128i g1 = LoadGroup(base, in + kGroup);  // lvlx128 v61 / lvrx128 v60 / vor
        in += kMonoBlockBytes;                                 // addi r10,r10,32

        ctx.fpscr.enableFlushModeUnconditional();  // emitted at the first vcsxwfp128
        const Halves a = Convert(g0);  // vupkhsb128 v57 / vupklsb128 v56 -> v53 / v52
        const Halves b = Convert(g1);  // vupkhsb128 v55 / vupklsb128 v54 -> v51 / v50

        StoreGroup(base, out + 0 * kGroup, a.high);  // stvlx128 v53,r0,r11  / stvrx128 v53,r11,r7
        StoreGroup(base, out + 1 * kGroup, a.low);   // stvlx128 v52,r11,r7  / stvrx128 v52,r11,r6
        StoreGroup(base, out + 2 * kGroup, b.high);  // stvlx128 v51,r11,r6  / stvrx128 v51,r11,r4
        StoreGroup(base, out + 3 * kGroup, b.low);   // stvlx128 v50,r11,r4  / stvrx128 v50,r11,r3
        out += 4 * kGroup;                           // addi r11,r11,64
      } while (--blocks != 0);                       // bdnz 0x82b50470
    }

    // loc_82B504E4: the scale, and the samples the vector loop did not cover.
    ctx.fpscr.disableFlushModeUnconditional();  // emitted by the lfs
    PPCRegister scale_bits{};
    scale_bits.u32 = REX_LOAD_U32(kScaleWord);  // lfs f0,-17516(r8)
    const double scale = static_cast<double>(scale_bits.f32);

    // cmpwi cr6,r31,0 ; beq cr6,0x82b50524 ; mtctr r31 ; addi r10,r10,-2
    if (tail != 0) {
      uint32_t count = static_cast<uint32_t>(tail);
      uint32_t at = in - 2;
      do {
        at += 2;                                              // lhzu r8,2(r10) updates first
        const int32_t sample = LoadSample(base, at);           // extsh r8,r8
        ctx.fpscr.disableFlushModeUnconditional();             // emitted at the lfd
        StoreSingle(base, out, SampleToSingle(sample, scale));  // stfs f10,0(r11)
        out += 4;                                             // addi r11,r11,4
      } while (--count != 0);                                 // bdnz 0x82b504fc
      // r10 is left on the last halfword read and immediately overwritten by mr r10,r5 below.
    }

    // loc_82B50524: mr r10,r5 -- the wrapped run reads from the ring BASE, and its loads are
    // aligned lvx128 rather than the unaligned pairs the first run used.
    in = src;
    int32_t wrap_tail = wrap;  // r9, unless the vector loop recomputes it

    // cmpwi cr6,r9,16 ; blt cr6,0x82b5059c
    if (wrap >= 16) {
      // rlwinm r5,r9,28,4,31 ; rlwinm r31,r9,0,0,27 ; subf r9,r31,r9 ; mtctr r5
      uint32_t blocks = (static_cast<uint32_t>(wrap) >> 4) & 0xFFFFFFFu;
      wrap_tail = wrap - static_cast<int32_t>(static_cast<uint32_t>(wrap) & 0xFFFFFFF0u);
      do {
        // loc_82B50544.
        const simde__m128i g0 = Lvx(base, in);           // lvx128 v49,r0,r10
        const simde__m128i g1 = Lvx(base, in + kGroup);  // lvx128 v47,r10,r8  (r8 = 16)
        in += kMonoBlockBytes;                           // addi r10,r10,32

        ctx.fpscr.enableFlushModeUnconditional();  // emitted at the first vcsxwfp128
        const Halves a = Convert(g0);  // vupkhsb128 v48 / vupklsb128 v46 -> v43 / v42
        const Halves b = Convert(g1);  // vupkhsb128 v45 / vupklsb128 v44 -> v41 / v40

        StoreGroup(base, out + 0 * kGroup, a.high);  // stvlx128 v43,r0,r11 / stvrx128 v43,r11,r7
        StoreGroup(base, out + 1 * kGroup, a.low);   // stvlx128 v42,r11,r7 / stvrx128 v42,r11,r6
        StoreGroup(base, out + 2 * kGroup, b.high);  // stvlx128 v41,r11,r6 / stvrx128 v41,r11,r4
        StoreGroup(base, out + 3 * kGroup, b.low);   // stvlx128 v40,r11,r4 / stvrx128 v40,r11,r3
        out += 4 * kGroup;                           // addi r11,r11,64
      } while (--blocks != 0);                       // bdnz 0x82b50544
    }

    // loc_82B5059C: cmpwi cr6,r9,0 ; beq cr6,0x82b50b70 -- straight to the epilogue.
    ctx.r3.s64 = 64;  // li r3,64 at loc_82B50444, never touched again on this path
    if (wrap_tail == 0) return;

    // mtctr r9 ; addi r9,r11,-4 ; addi r11,r10,-2 -- both cursors run one element behind, because
    // the tail writes through stfsu.
    uint32_t count = static_cast<uint32_t>(wrap_tail);
    uint32_t at_out = out - 4;
    uint32_t at_in = in - 2;
    do {
      at_in += 2;                                    // lhzu r10,2(r11) updates first
      const int32_t sample = LoadSample(base, at_in);  // extsh r8,r10
      ctx.fpscr.disableFlushModeUnconditional();     // emitted at the lfd
      at_out += 4;                                   // stfsu f10,4(r9) updates first
      StoreSingle(base, at_out, SampleToSingle(sample, scale));
    } while (--count != 0);                          // bdnz 0x82b505b0
    return;                                          // b 0x82f5215c
  }

  // ---- loc_82B505D8: the stereo path. `first` and `wrap` count FRAMES, and each 16-byte group
  // of interleaved samples is split by vperm128 into channel 0 (the vupkhsb128 half) and channel
  // 1 (the vupklsb128 half). ----

  // mr r26,r9 ; stw r9,-124(r1) ; mr r7,r4 ; stw r4,-128(r1): r26 and the -124/-128 slots are
  // this function's own frame. See the note for why the frame round trips are kept in registers.

  // loc_82B50648: the same dcbz establish, but the threshold is 16 frames and the count is
  // (first + wrap) >> 4 with a 128-byte step. That reaches dst + 8n - 96 for n frames, while
  // channel 0's converted output is only 4n bytes: the overshoot is part of the write set.
  const uint32_t frames = ctx.r8.u32 + ctx.r9.u32;  // add r6,r8,r9
  if ((dst & 0x7Fu) == 0 && static_cast<int32_t>(frames) >= 16) {
    uint32_t lines = (frames >> 4) & 0xFFFFFFFu;  // rlwinm r4,r6,28,4,31
    uint32_t at = 0;
    do {
      Dcbz(base, at + dst);  // dcbz r6,r11
      at += 128;             // addi r6,r6,128
    } while (--lines != 0);  // bdnz 0x82b5066c
  }

  // loc_82B50678: four stw build the vperm128 control on this frame and one lvx128 reads it
  // back. Unlike the splat cases in the verified ports the four words are NOT identical, so the
  // load's lane reversal is not a no-op on them -- the control is left in guest memory and
  // loaded exactly as the original does rather than hand-assembled from the immediates. In guest
  // byte order it is 00 01 04 05 08 09 0C 0D 02 03 06 07 0A 0B 0E 0F: the even halfwords of the
  // group gathered into one half and the odd ones into the other, i.e. the de-interleave.
  // (Like the lifted lvx128 this relies on the guest r1 being 16-byte aligned; see the note.)
  const uint32_t control_at = sp - kControlSlot;    // addi r30,r1,-80
  REX_STORE_U32(control_at + 0, 0x00010405u);   // lis r6,1     ; ori r6,r6,1029  ; stw r6,-80(r1)
  REX_STORE_U32(control_at + 12, 0x0A0B0E0Fu);  // lis r31,2571 ; ori r6,r31,3599 ; stw r6,-68(r1)
  REX_STORE_U32(control_at + 4, 0x08090C0Du);   // lis r4,2057  ; ori r4,r4,3085  ; stw r4,-76(r1)
  REX_STORE_U32(control_at + 8, 0x02030607u);   // lis r3,515   ; ori r3,r3,1543  ; stw r3,-72(r1)
  const simde__m128i control = Lvx(base, control_at);  // lvx128 v0,r0,r30

  // li r29,16 ; li r28,32 ; li r27,128 ; li r31,48 ; li r30,64 ; li r6,80 ; li r4,96 ; li r3,112
  // -- eight index constants, which is what the census's r3/r4/r6/r27..r31 store operands are
  // from here on. At entry r3, r4 and r6 held the destination, the second destination and the
  // ring offset.
  uint32_t out0 = dst;           // r11, channel 0
  uint32_t out1 = dst_second;    // r7 and the -128(r1) slot, channel 1
  uint32_t in = read;            // r10
  int32_t tail = first;          // r8 at loc_82B50918, unless the vector loop recomputes it

  // cmpwi cr6,r8,32 ; blt cr6,0x82b50918
  if (first >= 32) {
    // rlwinm r26,r8,27,5,31 ; rlwinm r9,r8,0,0,26 ; subf r7,r9,r8 ; stw r7,-80(r1) ; mtctr r26.
    // The remainder is spilled over the first word of the control, which by then is in v0.
    uint32_t blocks = (static_cast<uint32_t>(first) >> 5) & 0x7FFFFFFu;
    tail = first - static_cast<int32_t>(static_cast<uint32_t>(first) & 0xFFFFFFE0u);
    do {
      // loc_82B50720: eight unaligned 16-byte groups, 128 bytes of interleaved samples.
      simde__m128i group[8];
      for (uint32_t g = 0; g < 8; g++) group[g] = LoadGroup(base, in + g * kGroup);
      in += kStereoBlockBytes;  // addi r10,r10,128 (and stw r10,-120(r1), this frame)

      // vperm128 vD,vS,vS,v0 per group, then the two halves. Every load above happens before
      // every store below, which is what makes an overlapping src/dst behave as the original's.
      simde__m128i permuted[8];
      for (uint32_t g = 0; g < 8; g++) permuted[g] = Vperm(group[g], group[g], control);

      ctx.fpscr.enableFlushModeUnconditional();  // emitted at the first vcsxwfp128
      Halves split[8];
      for (uint32_t g = 0; g < 8; g++) split[g] = Convert(permuted[g]);

      // Sixteen stvlx128/stvrx128 for channel 0 -- +0/+16 .. +112/+128 -- then sixteen more for
      // channel 1, in that order. r29/r28/r31/r30/r6/r4/r3/r27 are the 16..128 indices, some of
      // them reloaded mid-block out of -100/-96/-88(r1) with the same values.
      for (uint32_t g = 0; g < 8; g++) StoreGroup(base, out0 + g * kGroup, split[g].high);
      for (uint32_t g = 0; g < 8; g++) StoreGroup(base, out1 + g * kGroup, split[g].low);
      out0 += kStereoBlockBytes;  // addi r11,r11,128
      out1 += kStereoBlockBytes;  // addi r7,r10,128 ; stw r7,-128(r1)
    } while (--blocks != 0);      // bdnz 0x82b50710
  }

  // loc_82B50918. On the blt path r8 is still the entry frame count and r7 is still r4; after the
  // loop they are the spilled remainder and the advanced channel-1 cursor.
  ctx.fpscr.disableFlushModeUnconditional();  // emitted by the lfs
  PPCRegister scale_bits{};
  scale_bits.u32 = REX_LOAD_U32(kScaleWord);  // lfs f0,-17516(r25)
  const double scale = static_cast<double>(scale_bits.f32);

  // cmpwi cr6,r8,0 ; beq cr6,0x82b5097c ; mtctr r8
  if (tail != 0) {
    uint32_t count = static_cast<uint32_t>(tail);
    do {
      const int32_t left = LoadSample(base, in);       // lhz r25,0(r10)  ; extsh r25,r25
      const int32_t right = LoadSample(base, in + 2);  // lhzu r8,2(r10)  ; extsh r8,r8
      in += 4;                                         // the lhzu's update plus addi r10,r10,2
      ctx.fpscr.disableFlushModeUnconditional();       // emitted at the first lfd
      StoreSingle(base, out0, SampleToSingle(left, scale));   // stfs f7,0(r11)
      out0 += 4;                                       // addi r11,r11,4
      StoreSingle(base, out1, SampleToSingle(right, scale));  // stfs f6,0(r7)
      out1 += 4;                                       // addi r7,r7,4
    } while (--count != 0);                            // bdnz 0x82b5092c
  }

  // loc_82B5097C: mr r10,r5 -- the wrapped run, from the ring base, with aligned loads.
  in = src;
  int32_t wrap_tail = wrap;  // r26, unless the vector loop recomputes it

  // cmpwi cr6,r9,32 ; blt cr6,0x82b50b14
  if (wrap >= 32) {
    // rlwinm r8,r9,27,5,31 ; rlwinm r5,r9,0,0,26 ; subf r9,r5,r9 ; stw r9,-124(r1) ; mtctr r8
    uint32_t blocks = (static_cast<uint32_t>(wrap) >> 5) & 0x7FFFFFFu;
    wrap_tail = wrap - static_cast<int32_t>(static_cast<uint32_t>(wrap) & 0xFFFFFFE0u);
    do {
      // loc_82B509AC: the same block, with lvx128 in place of the lvlx128/lvrx128/vor128 triples.
      simde__m128i group[8];
      for (uint32_t g = 0; g < 8; g++) group[g] = Lvx(base, in + g * kGroup);
      in += kStereoBlockBytes;  // addi r10,r10,128

      simde__m128i permuted[8];
      for (uint32_t g = 0; g < 8; g++) permuted[g] = Vperm(group[g], group[g], control);

      ctx.fpscr.enableFlushModeUnconditional();  // emitted at the first vcsxwfp128
      Halves split[8];
      for (uint32_t g = 0; g < 8; g++) split[g] = Convert(permuted[g]);

      for (uint32_t g = 0; g < 8; g++) StoreGroup(base, out0 + g * kGroup, split[g].high);
      for (uint32_t g = 0; g < 8; g++) StoreGroup(base, out1 + g * kGroup, split[g].low);
      out0 += kStereoBlockBytes;  // addi r11,r11,128
      out1 += kStereoBlockBytes;  // addi r7,r7,128
    } while (--blocks != 0);      // bdnz 0x82b509a0
  }

  // loc_82B50B14: cmpwi cr6,r26,0 ; beq cr6,0x82b50b70
  ctx.r3.s64 = 112;  // li r3,112, the last value on this path unless the tail below runs
  if (wrap_tail == 0) return;

  // mtctr r26 ; addi r9,r7,-4 ; addi r11,r11,-4 -- both cursors one single behind, for stfsu.
  uint32_t count = static_cast<uint32_t>(wrap_tail);
  uint32_t at_out0 = out0 - 4;
  uint32_t at_out1 = out1 - 4;
  do {
    const int32_t left = LoadSample(base, in);       // lhz r7,0(r10)  ; extsh r4,r7
    const int32_t right = LoadSample(base, in + 2);  // lhzu r8,2(r10) ; extsh r3,r8
    in += 4;                                         // the lhzu's update plus addi r10,r10,2
    ctx.fpscr.disableFlushModeUnconditional();       // emitted at the first lfd
    // Channel 1 is stored FIRST here, the opposite order to loc_82B5092C above.
    at_out1 += 4;                                    // stfsu f6,4(r9) updates first
    StoreSingle(base, at_out1, SampleToSingle(right, scale));
    at_out0 += 4;                                    // stfsu f7,4(r11) updates first
    StoreSingle(base, at_out0, SampleToSingle(left, scale));
    ctx.r3.s64 = right;                              // extsh r3,r8 -- what r3 is left holding
  } while (--count != 0);                            // bdnz 0x82b50b28
}

// Not enumerable from entry state -- gate 2, and measured rather than inferred.
//
// Every store in the body is based on r11, r10 or r7, indexed by one of r3, r4, r5, r6, r7, r8,
// r9, r27, r28, r29, r30 or r31, and NONE of those registers holds its entry value at the store:
//
//   r11  = r3 at the top (mr r11,r3), then advanced by 64 or 128 per iteration. Its ENTRY value
//          is stale caller scratch -- 0x49600060 in the 24 consecutive boot samples, 0x496017E4 at
//          call 50,000 and 0x496014D4 at call 100,000. An address the function never touches.
//   r10  = src + (offset & ~1) before the first run and src before the second (mr r10,r5), and on
//          the stereo path it is reloaded mid-loop from -120(r1) and -128(r1), frame slots the
//          body itself wrote. Its entry value, 0x707BF900, is stack scratch.
//   r7   = the channel-1 cursor on the stereo path, r10 + 128 taken from -128(r1). At ENTRY r7 is
//          the CHANNEL COUNT (1 in every logged call).
//   r3,r4,r6 = the constants 112, 96 and 80 from loc_82B50678 (64, 48 and 32 on the mono path
//          from loc_82B50444). At entry they are the destination, the second destination and the
//          ring byte offset.
//   r5   = 32 (mr r5,r28 at loc_82B50678, lwz r5,-104(r1) at loc_82B50710), then 48 once the
//          block reloads it from -88(r1); a block count on the mono path (rlwinm r5,r9,28,4,31 at
//          loc_82B50524). At entry it is the ring base, 0xE2E3F000 in the logged calls -- a
//          pointer, but not this store's.
//   r8,r9 = 16, 32, 48, 64 or 128, depending on which spill slot they were last reloaded from.
//          At entry they are the two sample counts.
//   r27..r31 = 128, 32, 16, 64, 48 from loc_82B50678.
//
// So evaluating the census's store expressions on entry registers -- which is all a Windows()
// hook can see -- yields nonsense: `ea = r11 + r3` becomes 0x49600060 + 0x4B399B80 = 0x94999BE0,
// outside guest range. That is the measurement in probe/harness/out/p17_windows.log, and it is
// the answer; an earlier pass judged this function portable by reading the FORM of those
// expressions instead of their operands, one of the three inference mistakes docs/PLAN.md
// records. No prediction is attempted here to rescue it.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B50380
SKATE3_PORT(82B50380, skate3::audio::kPortGate2, skate3::audio::kReturnR3)
