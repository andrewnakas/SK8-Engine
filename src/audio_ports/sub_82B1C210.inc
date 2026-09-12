// sub_82B1C210  role: clamp a block of N values into their per-value [min,max] ranges, then broadcast the block to the record's listener list   lifted: skate3_recomp.66.cpp:63915 (99 lines)
// STATUS: gate-1 (indirect call: sub_828E29C0 at depth 1 -- its bctrl calls every registered listener)
// calls/session: boot 25894  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C210.md
namespace port_82B1C210 {

// The first two words are sub_828E29C0's handle pair (holder, token); this function does not
// touch them, it only hands the record over.
constexpr uint32_t kClampEnabled = 8;  // u8: zero means the range table is absent
constexpr uint32_t kCount = 9;         // u8: N, RELOADED on every iteration
constexpr uint32_t kTableBase = 12;    // N pairs of (min,max) int32 when enabled, else the
                                       // value block starts here directly
constexpr uint32_t kRangeStride = 8;   // one (min,max) pair
constexpr uint32_t kBlockHeader = 0;   // on the value block: must be non-zero to broadcast
constexpr uint32_t kBlockValues = 4;   // the N clamped words, and the payload pointer passed on

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  const uint64_t entry_lr = ctx.lr;  // mflr r12 / stw r12,-8(r1)

  // lbz r11,8(r3) / cmplwi cr6,r11,0 / beq cr6,0x82b1c28c
  uint64_t block;  // r5
  if (REX_LOAD_U8(self + kClampEnabled) == 0) {
    block = static_cast<uint64_t>(self) + kTableBase;  // addi r5,r3,12
  } else {
    const uint32_t count = REX_LOAD_U8(self + kCount);   // lbz r9,9(r3)
    uint64_t range = static_cast<uint64_t>(self) + kTableBase;  // addi r11,r3,12
    // rotlwi r10,r9,3 then add r5,r10,r11 -- a rotate, not a shift, but r9 came from an lbz
    // so it is 0..255 and no bit can wrap out of bit 31. The add is 64-bit on zero-extended
    // operands, as RexGlue lifts it.
    block = static_cast<uint64_t>(__builtin_rotateleft32(count, 3)) + range;
    // cmpwi cr6,r9,0 / ble cr6,0x82b1c290 -- signed, though a byte can never be negative
    if (static_cast<int32_t>(count) > 0) {
      uint64_t slot = block;  // mr r8,r5
      for (int32_t done = 0;;) {
        // lwz r9,0(r11) / lwz r10,4(r8) / lwz r7,4(r11) -- three loads in this order, and the
        // value is read from the very address the store below writes.
        const int32_t lo = static_cast<int32_t>(REX_LOAD_U32(static_cast<uint32_t>(range)));
        int32_t value = static_cast<int32_t>(
            REX_LOAD_U32(static_cast<uint32_t>(slot) + kBlockValues));
        const int32_t hi = static_cast<int32_t>(
            REX_LOAD_U32(static_cast<uint32_t>(range) + 4));
        if (value < lo) {
          value = lo;  // cmpw cr6,r10,r9 / bge / mr r10,r9 / b 0x82b1c270
        } else if (value > hi) {
          value = hi;  // cmpw cr6,r10,r7 / ble / mr r10,r7
        }
        done++;  // addi r6,r6,1
        // stwu r10,4(r8) -- store WITH update: the pointer advances to the address written.
        slot += kBlockValues;
        REX_STORE_U32(static_cast<uint32_t>(slot), static_cast<uint32_t>(value));
        range += kRangeStride;  // addi r11,r11,8
        // lbz r10,9(r3) / cmpw cr6,r6,r10 / blt -- the count is re-read every iteration, so a
        // listener or another thread shortening it mid-loop shortens the loop.
        if (done >= static_cast<int32_t>(REX_LOAD_U8(self + kCount))) break;
      }
    }
  }

  // lwz r11,0(r5) / cmpwi cr6,r11,0 / beq cr6,0x82b1c2a4
  if (static_cast<int32_t>(REX_LOAD_U32(static_cast<uint32_t>(block) + kBlockHeader)) != 0) {
    // addi r4,r5,4 / bl 0x828e29c0 -- r3 passes through untouched, r4 is the value array.
    ctx.lr = 0x82B1C2A4;
    skate3::audio::GuestCall(ctx, base, sub_828E29C0, ctx.r3.u64, block + kBlockValues);
  }
  ctx.r3.s64 = 0;    // li r3,0 -- the callee's result is discarded
  ctx.lr = entry_lr;  // lwz r12,-8(r1) / mtlr r12
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1C210
SKATE3_PORT(82B1C210, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
