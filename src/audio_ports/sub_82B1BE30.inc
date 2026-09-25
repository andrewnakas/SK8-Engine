// sub_82B1BE30  role: clamp-and-dispatch property setter -- takes an object, a property id in r4 and a value in r5, clamps the value to the range that property uses (raw, [0,32767] or [0,65535]) and TAIL-branches to the object's own setter through vtable+12, or for id 3 through vtable+16 with the value moved into r4   lifted: skate3_recomp.66.cpp:63344 (179 lines)
// STATUS: gate-1 (every path that does anything ends in an indirect tail branch through the object's vtable: sub_82B1BE30 indirect call, six `bctr` sites, vtable+12 and vtable+16)
// calls/session: boot 12461  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1BE30.md
namespace port_82B1BE30 {

// Nothing in docs/rw_audio_structs.h names this object; only its vtable is touched here, so the
// two slot numbers are all the layout this function needs.
constexpr uint32_t kVptr = 0;        // object+0 -> vtable
constexpr uint32_t kSetSlot = 12;    // vtable+12: the setter taking (id, value)
constexpr uint32_t kSetAltSlot = 16; // vtable+16: the setter id 3 uses, taking (value, 0)

constexpr int32_t kClampU16 = 65535;  // lis r10,0 ; ori r10,r10,65535
constexpr int32_t kClampS16 = 32767;  // cmpwi cr6,r11,32767

// lwz r10,0(r3) ; mr r5,r11 ; lwz r9,<slot>(r10) ; mtctr r9 ; bctr
// A TAIL branch, not a call: `bctr` leaves lr alone, so the setter returns straight to OUR
// caller and the r3 it leaves is this function's result. r3 is never rewritten here -- the
// object pointer is already in it -- so the callee sees the entry register bit for bit.
inline void TailDispatch(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot,
                         uint64_t value_in_r5) {
  const uint32_t vtable = REX_LOAD_U32(object + kVptr);  // lwz r10,0(r3)
  ctx.r5.u64 = value_in_r5;                              // mr r5,r11 (or li r5,0)
  ctx.ctr.u64 = REX_LOAD_U32(vtable + slot);             // lwz r9,slot(r10) ; mtctr r9
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);                   // bctr
}

// cmpwi cr6,r11,0 ; bge ; li r11,0 / cmpw(i) against the limit ; ble ; mr r11,r10 -- both clamps
// compare SIGNED, so a negative value becomes 0 rather than a huge unsigned one, and the result
// is a small literal in all 64 bits of r11.
inline uint64_t ClampSigned(uint64_t value, int32_t limit) {
  if (static_cast<int32_t>(value) < 0) return 0;
  if (static_cast<int32_t>(value) > limit) return static_cast<uint64_t>(limit);
  return value;
}

REX_FUNC(Native) {
  uint64_t value = ctx.r5.u64;  // mr r11,r5 -- all 64 bits; the clamps overwrite with literals

  // cmplwi cr6,r3,0 ; beqlr cr6 -- a null object writes nothing and leaves r3 == 0.
  if (ctx.r3.u32 == 0) return;
  const uint32_t object = ctx.r3.u32;

  // cmplwi cr6,r4,8 ; bgt cr6,0x82b1bf3c -- UNSIGNED, so a negative id also lands at BF3C.
  if (ctx.r4.u32 > 8) {
    // loc_82B1BF3C: cmpwi cr6,r4,9 ; bltlr cr6 ; cmpwi cr6,r4,136 ; bgtlr cr6 -- both SIGNED, so
    // this is the window [9,136]; a negative id returned by the first test.
    if (ctx.r4.s32 < 9) return;
    if (ctx.r4.s32 > 136) return;
    TailDispatch(ctx, base, object, kSetSlot, value);  // no clamp on this range
    return;
  }

  // mtctr r4 ; cmpwi cr6,r4,0 ; seven `bdzf 4*cr6+eq` ; bne cr6 -- the compiler's jump table for
  // id 0..8. cr6+eq is "r4 == 0" and never changes, so for a nonzero id the n-th bdzf is the one
  // whose decrement lands CTR on zero, i.e. it fires exactly when id == n; id 8 exhausts all
  // seven and is caught by the trailing `bne cr6`; id 0 keeps eq set, so nothing branches and
  // control falls through the whole chain. The lifted form tests ctx.ctr.u32 after each
  // decrement, so it is the LOW 32 BITS of r4 that select (see the note).
  uint64_t ctr = ctx.r4.u64;                // mtctr r4
  const bool id_is_zero = ctx.r4.s32 == 0;  // cmpwi cr6,r4,0
  int id = 0;                               // 0 = fell through the chain
  for (int n = 1; n <= 7; n++) {
    --ctr;
    if (static_cast<uint32_t>(ctr) == 0 && !id_is_zero) {
      id = n;
      break;
    }
  }
  if (id == 0 && !id_is_zero) id = 8;  // bne cr6,0x82b1bea8
  // CTR is dead here: every path below re-loads it with `mtctr r9` before branching.

  switch (id) {
    case 1:  // loc_82B1BF28
    case 4:
      // No clamp at all: r5 is handed on exactly as it arrived.
      TailDispatch(ctx, base, object, kSetSlot, value);
      return;

    case 2:  // loc_82B1BEA8
    case 5:
    case 8:
      TailDispatch(ctx, base, object, kSetSlot, ClampSigned(value, kClampS16));
      return;

    case 3:
      // loc_82B1BED8: lwz r10,0(r3) ; li r5,0 ; mr r4,r11 ; lwz r9,16(r10) -- the only site that
      // uses vtable+16, and the only one that moves the value into r4 and zeroes r5. No clamp.
      ctx.r4.u64 = value;  // mr r4,r11
      TailDispatch(ctx, base, object, kSetAltSlot, 0);  // li r5,0
      return;

    case 6:  // loc_82B1BEF0
    case 7:
      TailDispatch(ctx, base, object, kSetSlot, ClampSigned(value, kClampU16));
      return;

    default:
      // id == 0, the chain's fall-through. Identical to loc_82B1BEF0 except for the redundant
      // `li r4,0` -- r4.u32 is already 0 here, but the literal clears all 64 bits, so it is
      // reproduced rather than dropped.
      value = ClampSigned(value, kClampU16);
      {
        const uint32_t vtable = REX_LOAD_U32(object + kVptr);  // lwz r10,0(r3)
        ctx.r5.u64 = value;                                    // mr r5,r11
        ctx.r4.s64 = 0;                                        // li r4,0
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kSetSlot);         // lwz r9,12(r10) ; mtctr r9
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);                   // bctr
      }
      return;
  }
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. This function stores nothing itself; the whole write set
// belongs to the vtable setter it tail-branches to, which is exactly why it is gate 1.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1BE30
SKATE3_PORT(82B1BE30, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
