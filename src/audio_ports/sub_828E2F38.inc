// sub_828E2F38  role: validate a handle, and if the incoming word differs from the holder's stored one, write it and notify every listener with the cell's address   lifted: skate3_recomp.47.cpp:4747 (109 lines)
// STATUS: gate-1 (indirect call: sub_828E2F38's own bctrl, once per list node, through the function pointer at node+8)
// calls/session: boot 3442  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_828E2F38.md
namespace port_828E2F38 {

constexpr uint32_t kHolder = 0;  // handle +0: the holder, zeroed when the handle goes stale
constexpr uint32_t kToken = 4;   // handle +4: generation token; already negative means invalid
constexpr uint32_t kHolderHead = 0;         // holder +0: head of the listener list
constexpr uint32_t kHolderValue = 4;        // holder +4: the stored word, and what listeners
                                            //            are handed the ADDRESS of
constexpr uint32_t kHolderGeneration = 12;  // holder +12: the token must still match this
constexpr uint32_t kNodeNext = 0;      // node +0: next, RE-READ after each callback
constexpr uint32_t kNodeFunc = 8;      // node +8: the listener function pointer
constexpr uint32_t kNodeUserData = 12; // node +12: passed to the listener in r4

constexpr int32_t kStaleHandle = -3;  // written back into the handle before returning
constexpr int32_t kNoHolder = -6;

REX_FUNC(Native) {
  const uint32_t handle = ctx.r3.u32;  // mr r10,r3
  const uint32_t source = ctx.r4.u32;  // the incoming value's address
  const uint64_t entry_lr = ctx.lr;    // mflr r12 / stw r12,-8(r1)

  // lwz r9,4(r3) / cmpwi cr6,r9,0 / blt cr6,0x828e2f90
  const uint32_t token = REX_LOAD_U32(handle + kToken);
  if (static_cast<int32_t>(token) < 0) {
    // mr r3,r9 -- r9 holds the ZERO-EXTENDED loaded word, so this is 0x00000000FFFFFFxx and
    // not the sign-extended negative the `li` paths leave. Reproduced deliberately.
    ctx.r3.u64 = static_cast<uint64_t>(token);
    ctx.lr = entry_lr;
    return;
  }

  // lwz r11,0(r3) / cmplwi cr6,r11,0 / bne cr6,0x828e2f74
  const uint32_t holder = REX_LOAD_U32(handle + kHolder);
  if (holder == 0) {
    ctx.r3.s64 = kNoHolder;  // li r9,-6 / mr r3,r9 -- sign-extended, unlike the path above
    ctx.lr = entry_lr;
    return;
  }

  // lwz r8,12(r11) / cmpw cr6,r9,r8 / beq cr6,0x828e2f98
  if (static_cast<int32_t>(token) !=
      static_cast<int32_t>(REX_LOAD_U32(holder + kHolderGeneration))) {
    // Stale handle: mark it dead, +4 before +0, then report the code.
    REX_STORE_U32(handle + kToken, static_cast<uint32_t>(kStaleHandle));  // stw r9,4(r10)
    REX_STORE_U32(handle + kHolder, 0);                                   // stw r11,0(r10)
    ctx.r3.s64 = kStaleHandle;                                            // mr r3,r9
    ctx.lr = entry_lr;
    return;
  }

  // lwz r10,0(r4) / addi r30,r11,4 / lwz r9,4(r11) / cmplw cr6,r10,r9 / beq cr6,0x828e2fdc
  // The compare is UNSIGNED equality on the raw words: a bit compare, so it works for a
  // float payload too, with the usual -0.0 / NaN caveats. Nothing happens when they match.
  const uint32_t incoming = REX_LOAD_U32(source);
  const uint64_t cell = static_cast<uint64_t>(holder) + kHolderValue;  // addi is 64-bit
  if (incoming != REX_LOAD_U32(holder + kHolderValue)) {
    // lwz r31,0(r11) then stw r10,0(r30) -- the list head is loaded BEFORE the value is
    // published, and the walk uses that snapshot. Order kept.
    uint32_t node = REX_LOAD_U32(holder + kHolderHead);
    REX_STORE_U32(static_cast<uint32_t>(cell), incoming);
    while (node != 0) {
      // lwz r11,8(r31) / mr r3,r30 / lwz r4,12(r31) / mtctr r11 / bctrl
      // The listener is handed the ADDRESS of the stored cell in r3, not the value, plus its
      // own registration data in r4.
      ctx.ctr.u64 = REX_LOAD_U32(node + kNodeFunc);
      ctx.r3.u64 = cell;
      ctx.r4.u64 = REX_LOAD_U32(node + kNodeUserData);
      ctx.lr = 0x828E2FD0;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      // lwz r31,0(r31) -- `next` is read AFTER the callback ran, off the node it was called
      // for. A listener that frees its own node reads freed memory here. Reproduced.
      node = REX_LOAD_U32(node + kNodeNext);
    }
  }

  ctx.r3.s64 = 0;     // li r3,0
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

}  // namespace port_828E2F38
SKATE3_PORT(828E2F38, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
