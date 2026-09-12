// sub_82B43FB8  role: linearly interpolating resampler -- count floats out of a table walked by a 16.16 phase   lifted: skate3_recomp.68.cpp:26015 (610 lines)
// STATUS: pending
// calls/session: boot 233413  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B43FB8.md
namespace port_82B43FB8 {

// lis r31,-32208 ; addi r31,r31,-31232 ; lfs f0,428(r31) -- 0x82300000 - 0x7A00 + 428. The cell
// holds 0x377FFC9C, which is the float literal 1.5258e-5: 1/65536 to six digits only (1/65536 is
// 0x37800000). The body loads it, as the original does; nothing here assumes a value.
constexpr uint32_t kFractionScaleCell = 0x822F87ACu;

// Nothing in docs/rw_audio_structs.h names anything this function touches: it takes five raw
// pointers and a step, with no structure offsets at all.

inline double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

inline double SubSingle(double a, double b) { return double(float(a - b)); }  // fsubs
inline double MulSingle(double a, double b) { return double(float(a * b)); }  // fmuls
inline double FmaSingle(double a, double b, double c) {                       // fmadds
  return double(float(std::fma(a, b, c)));
}

// std rN,-k(r1) ; lfd fM,-k(r1) ; fcfid ; frsp -- the zero-extended fraction as a single, held in
// a double. The spill slots are the function's own frame (-184 .. -240 of r1, no stwu) and are
// neither reproduced nor windowed.
inline double FractionToSingle(uint64_t fraction) {
  return double(float(double(int64_t(fraction))));
}

// One output sample: the pair difference, the fraction scaled into a weight, one fmadds. Operand
// order follows the lifted lines exactly (fmuls fN,fraction,f0 and fmadds fN,difference,weight,a),
// because neither multiply is NaN-commutative.
inline double Interpolate(double a, double b, uint64_t fraction, double scale) {
  const double weight = MulSingle(FractionToSingle(fraction), scale);
  return FmaSingle(SubSingle(b, a), weight, a);
}

// The running phase is 16.16: the low 16 bits of the 32-bit value are the fraction that weights
// the pair, the next 16 are whole input samples to advance by.
struct Phase {
  uint64_t fraction;  // clrlwi rN,rX,16
  uint64_t advance;   // rlwinm rN,rX,16,16,31
};
inline Phase Split(uint64_t value) {
  return {uint64_t(uint32_t(value) & 0xFFFFu), uint64_t((uint32_t(value) >> 16) & 0xFFFFu)};
}

// rlwinm rN,rIndex,2,0,29 ; add rN,rN,r4 -- the sample index scaled to a float offset and added to
// the table, all in 32 bits (the guest address is the low word of a 64-bit add).
inline uint32_t SampleAddress(uint64_t index, uint32_t table) {
  return uint32_t(((uint32_t(index) << 2) & 0xFFFFFFFCu) + table);
}

REX_FUNC(Native) {
  const uint32_t count = ctx.r3.u32;       // r3: output samples
  const uint32_t table = ctx.r4.u32;       // r4: float table, already biased by the caller
  const uint32_t index_slot = ctx.r6.u32;  // r6: u32, the whole-sample cursor, in and out
  const uint32_t phase_slot = ctx.r7.u32;  // r7: the 16-bit fraction, in the word's HIGH half
  const uint64_t step = ctx.r8.u64;        // r8: the 16.16 phase increment

  // rlwinm r21,r8,1,0,30 -- twice the step, truncated to 32 bits; r20..r15 then add the step in
  // 64 bits, so the group offsets are (2*step mod 2^32) + k*step, not (k+2)*step. Kept literal.
  const uint64_t twice = uint64_t((ctx.r8.u32 << 1) & 0xFFFFFFFEu);
  const uint64_t step_to[8] = {0,                // sample 0 uses the running phase unchanged
                               step,             // r8
                               twice,            // r21
                               twice + step,     // r20
                               twice + 2 * step, // r19
                               twice + 3 * step, // r18
                               twice + 4 * step, // r17
                               twice + 5 * step};// r16
  const uint64_t step_group = twice + 6 * step;  // r15: the whole group of eight

  uint64_t fraction = REX_LOAD_U16(phase_slot);  // lhz r11,0(r7)
  uint64_t index = REX_LOAD_U32(index_slot);     // lwz r10,0(r6)
  uint32_t out = ctx.r5.u32;                     // r5: the output cursor

  // rlwinm r9,r3,2,14,26 ; add r9,r9,r5 -- where the eight-at-a-time loop stops: only bits 3..15
  // of the count survive that mask, so it is the count rounded down to a multiple of eight.
  const uint32_t group_end = out + ((count << 2) & 0x3FFE0u);
  // rlwinm r3,r3,2,0,29 ; add r14,r3,r5 -- the end of the whole buffer. Only the low 32 bits of
  // r5/r14 are ever consumed (cmplw and store addresses), so both cursors are tracked as u32.
  const uint32_t end = out + ((count << 2) & 0xFFFFFFFCu);

  ctx.fpscr.disableFlushModeUnconditional();                  // stfd f29,-176(r1)
  const double scale = LoadSingle(base, kFractionScaleCell);  // lfs f0,428(r31)

  // Eight samples per trip, 32 bytes of output.
  if (out < group_end) {  // cmplw cr6,r5,r9 ; bge cr6,0x82b44234
    // subf r9,r5,r9 ; addi r9,r9,-1 ; rlwinm r9,r9,27,5,31 (>>5) ; addi r9,r9,1 ; mtctr r9
    uint32_t trips = (((group_end - out) - 1u) >> 5) + 1u;
    do {  // loc_82B44028
      uint64_t fraction_of[8];
      uint32_t address[8];
      // Sample 0 takes the running fraction whole (clrldi r29,r11,32, not the 16-bit mask) and
      // the running index; samples 1..7 add their group offset. All eight are formed from the
      // index as it stands at the top of the trip, before the group advance below.
      fraction_of[0] = fraction & 0xFFFFFFFFu;
      address[0] = SampleAddress(index, table);
      for (uint32_t k = 1; k < 8; k++) {
        const Phase phase = Split(fraction + step_to[k]);  // add ; clrlwi ; rlwinm 16,16,31
        fraction_of[k] = phase.fraction;
        address[k] = SampleAddress(phase.advance + index, table);  // add rN,rN,r10 ; rlwinm ; add r4
      }

      ctx.fpscr.disableFlushModeUnconditional();  // emitted before the first lfs of the trip
      // Twelve loads, three stores, four loads, five stores: the original's interleaving, kept
      // because the output buffer may alias the table.
      const double s0a = LoadSingle(base, address[0]);      // lfs f13,0(r9)
      const double s0b = LoadSingle(base, address[0] + 4);  // lfs f12,4(r9)
      const double s1a = LoadSingle(base, address[1]);      // lfs f11,0(r3)
      const double s1b = LoadSingle(base, address[1] + 4);  // lfs f9,4(r3)
      const double s2a = LoadSingle(base, address[2]);      // lfs f10,0(r30)
      const double s2b = LoadSingle(base, address[2] + 4);  // lfs f2,4(r30)
      const double s3a = LoadSingle(base, address[3]);      // lfs f3,0(r9)
      const double s3b = LoadSingle(base, address[3] + 4);  // lfs f2,4(r9)
      const double s4a = LoadSingle(base, address[4]);      // lfs f6,0(r3)
      const double s5a = LoadSingle(base, address[5]);      // lfs f31,0(r31)
      const double s6a = LoadSingle(base, address[6]);      // lfs f30,0(r30)
      const double s7a = LoadSingle(base, address[7]);      // lfs f29,0(r25)

      StoreSingle(base, out + 4, Interpolate(s1a, s1b, fraction_of[1], scale));  // stfs f12,4(r5)
      StoreSingle(base, out + 8, Interpolate(s2a, s2b, fraction_of[2], scale));  // stfs f11,8(r5)
      StoreSingle(base, out + 0, Interpolate(s0a, s0b, fraction_of[0], scale));  // stfs f10,0(r5)

      const double s4b = LoadSingle(base, address[4] + 4);  // lfs f8,4(r3)
      const double s5b = LoadSingle(base, address[5] + 4);  // lfs f1,4(r31)
      const double s6b = LoadSingle(base, address[6] + 4);  // lfs f10,4(r30)
      const double s7b = LoadSingle(base, address[7] + 4);  // lfs f4,4(r25)

      StoreSingle(base, out + 12, Interpolate(s3a, s3b, fraction_of[3], scale));  // stfs f5,12(r5)
      StoreSingle(base, out + 24, Interpolate(s6a, s6b, fraction_of[6], scale));  // stfs f11,24(r5)
      StoreSingle(base, out + 16, Interpolate(s4a, s4b, fraction_of[4], scale));  // stfs f8,16(r5)
      StoreSingle(base, out + 20, Interpolate(s5a, s5b, fraction_of[5], scale));  // stfs f9,20(r5)
      StoreSingle(base, out + 28, Interpolate(s7a, s7b, fraction_of[7], scale));  // stfs f7,28(r5)

      // add r11,r15,r11 ; rlwinm r9,r11,16,16,31 ; clrlwi r11,r11,16 ; add r10,r9,r10
      const Phase group = Split(fraction + step_group);
      index += group.advance;
      fraction = group.fraction;
      out += 32;              // addi r5,r5,32
    } while (--trips != 0);   // bdnz 0x82b44028
  }

  // loc_82B44234
  if (out < end) {  // cmplw cr6,r5,r14 ; bge cr6,0x82b443dc
    // subf r9,r5,r14 ; addi r9,r9,3 ; srawi r3,r9,2 ; addze r9,r3 -- ceil((end-out)/4) samples
    // left, computed on the low word with the carry srawi sets.
    const uint32_t biased = (end - out) + 3u;
    const bool carry = int32_t(biased) < 0 && (biased & 3u) != 0;
    const int32_t remaining = (int32_t(biased) >> 2) + (carry ? 1 : 0);

    // Four samples per trip, 16 bytes of output.
    if (remaining >= 4) {  // cmpwi cr6,r9,4 ; blt cr6,0x82b44374
      // subf ; addi r9,r9,-13 ; rlwinm r9,r9,28,4,31 (>>4) ; addi r9,r9,1 ; mtctr r9
      uint32_t trips = (((end - out) - 13u) >> 4) + 1u;
      do {  // loc_82B44268
        // Chained here rather than precomputed: each phase is the previous 16-bit fraction plus
        // the step, and each whole part lands on the index as it is produced.
        const uint64_t fraction0 = fraction & 0xFFFFFFFFu;  // clrldi r9,r11,32
        const uint64_t index0 = index;
        const Phase p1 = Split(fraction + step);       // add r11,r11,r8
        const uint64_t index1 = index0 + p1.advance;   // add r10,r30,r10
        const Phase p2 = Split(p1.fraction + step);    // add r9,r3,r8
        const uint64_t index2 = index1 + p2.advance;   // add r10,r31,r10
        const Phase p3 = Split(p2.fraction + step);    // add r9,r3,r8
        const uint64_t index3 = index2 + p3.advance;   // add r10,r30,r10
        const Phase p4 = Split(p3.fraction + step);    // add r11,r3,r8
        index = index3 + p4.advance;                   // add r10,r31,r10 -- the next trip's index
        fraction = p4.fraction;                        // clrlwi r11,r11,16

        const uint32_t address0 = SampleAddress(index0, table);
        const uint32_t address1 = SampleAddress(index1, table);
        const uint32_t address2 = SampleAddress(index2, table);
        const uint32_t address3 = SampleAddress(index3, table);

        ctx.fpscr.disableFlushModeUnconditional();  // emitted before the first lfd of the trip
        const double s0a = LoadSingle(base, address0);      // lfs f13,0(r11)
        const double s0b = LoadSingle(base, address0 + 4);  // lfs f12,4(r11)
        const double s1a = LoadSingle(base, address1);      // lfs f2,0(r11)
        const double s1b = LoadSingle(base, address1 + 4);  // lfs f1,4(r11)
        const double s2b = LoadSingle(base, address2 + 4);  // lfs f8,4(r9) -- +4 before +0 here
        const double s2a = LoadSingle(base, address2);      // lfs f10,0(r9)
        const double s3a = LoadSingle(base, address3);      // lfsx f5,r3,r4
        const double s3b = LoadSingle(base, address3 + 4);  // lfs f1,4(r9)

        StoreSingle(base, out + 12, Interpolate(s3a, s3b, p3.fraction, scale));  // stfs f6,12(r5)
        StoreSingle(base, out + 4, Interpolate(s1a, s1b, p1.fraction, scale));   // stfs f5,4(r5)
        StoreSingle(base, out + 0, Interpolate(s0a, s0b, fraction0, scale));     // stfs f3,0(r5)
        StoreSingle(base, out + 8, Interpolate(s2a, s2b, p2.fraction, scale));   // stfs f2,8(r5)
        out += 16;              // addi r5,r5,16
      } while (--trips != 0);   // bdnz 0x82b44268
    }

    // loc_82B44374 -- one sample per trip, for the tail.
    if (out < end) {  // cmplw cr6,r5,r14 ; bge cr6,0x82b443dc
      // subf ; addi r9,r9,-1 ; rlwinm r9,r9,30,2,31 (>>2) ; addi r9,r9,1 ; mtctr r9
      uint32_t trips = (((end - out) - 1u) >> 2) + 1u;
      uint32_t cursor = out - 4u;  // addi r5,r5,-4, because stfsu adds 4 before storing
      do {                         // loc_82B44394
        ctx.fpscr.disableFlushModeUnconditional();             // emitted before the lfd
        const uint32_t address = SampleAddress(index, table);  // rlwinm r9,r10,2,0,29 ; add r9,r9,r4
        const double a = LoadSingle(base, address);            // lfs f10,0(r9)
        const double b = LoadSingle(base, address + 4);        // lfs f9,4(r9)
        const double sample = Interpolate(a, b, fraction & 0xFFFFFFFFu, scale);  // clrldi r3,r11,32
        const Phase advanced = Split(fraction + step);          // add r11,r11,r8
        index += advanced.advance;                              // add r10,r9,r10
        fraction = advanced.fraction;                           // clrlwi r11,r11,16
        cursor += 4u;                                           // stfsu f6,4(r5): ea = 4 + r5
        StoreSingle(base, cursor, sample);
      } while (--trips != 0);  // bdnz 0x82b44394
      // r5 is left holding the last store address and is not read again (volatile, not compared).
    }
  }

  // loc_82B443DC -- publish the cursor and the fraction.
  // rlwinm r11,r11,16,0,15: the fraction back into the high half of the word. The low half of the
  // rotate lands outside the mask, so this is exact for any value, not just a 16-bit one.
  const uint32_t published_fraction = (uint32_t(fraction) << 16) & 0xFFFF0000u;
  REX_STORE_U32(index_slot, uint32_t(index));    // stw r10,0(r6)
  REX_STORE_U32(phase_slot, published_fraction);  // stw r11,0(r7) -- 4 bytes over a 16-bit field
  ctx.fpscr.disableFlushModeUnconditional();      // lfd f29,-176(r1), the frame restore
}

// Writes: the output buffer, [r5, r5 + ((r3 << 2) & 0xFFFFFFFC)) -- the three loops tile it
// exactly, each one's trip count being the remaining span divided by its stride -- plus the
// 4-byte cursor at r6 and the 4-byte phase word at r7, both written on every path, count 0
// included. Nothing else: the fraction spills live in the function's own frame, below r1.
//
// The read set is the table span; r6/r7 are not recorded as reads because the vector dumps input
// spans after the lifted body has run, which for a span the function also writes would record the
// result, not the input. Their entry bytes are in the write windows already.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t out = ctx.r5.u32;
  const uint32_t table = ctx.r4.u32;
  const uint64_t length = uint64_t((ctx.r3.u32 << 2) & 0xFFFFFFFCu);  // rlwinm r3,r3,2,0,29

  if (length + 8u > skate3::audio::kPortWatchCap) return false;  // over the window budget
  if (uint64_t(out) + length > 0x100000000ull) return false;     // the buffer span would wrap
  spec.write(out, uint32_t(length));
  spec.write(ctx.r6.u32, 4);
  spec.write(ctx.r7.u32, 4);
  spec.read(kFractionScaleCell, 4);

  // The table span. The index advances by the step's whole part per output sample plus the carries
  // out of the 16-bit fraction, so after N = length/4 samples it has reached
  //     index + N*whole + ((fraction + N*step_fraction) >> 16)
  // and the +4 of the last pair reads one sample past that. Every loop masks the whole part to 16
  // bits (per group of eight in the first loop), so that closed form is the true bound only while
  // eight steps cannot overflow the field; when they can, or when the span is wider than the
  // window budget, the table read is left out rather than guessed at. Reads are metadata for
  // replay and are never compared, so an absent span is honest and an over-wide one is not.
  const uint64_t whole = (ctx.r8.u32 >> 16) & 0xFFFFu;
  const uint64_t step_fraction = ctx.r8.u32 & 0xFFFFu;
  const uint64_t samples = length / 4;
  const uint64_t fraction = REX_LOAD_U16(ctx.r7.u32);
  if (8 * whole + 8 <= 0xFFFF) {
    const uint64_t index = REX_LOAD_U32(ctx.r6.u32);
    const uint64_t last = index + samples * whole + ((fraction + samples * step_fraction) >> 16);
    const uint64_t span = 4 * (last - index + 2);
    if (4 * (last + 2) <= 0xFFFFFFFFull && span <= skate3::audio::kPortWatchCap &&
        uint64_t(SampleAddress(index, table)) + span <= 0x100000000ull) {
      spec.read(SampleAddress(index, table), uint32_t(span));
    }
  }
  return true;
}

}  // namespace port_82B43FB8
SKATE3_PORT(82B43FB8, skate3::audio::kPortPending, skate3::audio::kReturnNone)
