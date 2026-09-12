// sub_828E29C0  role: validate a listener-list handle against its holder's generation, then call every listener in the list with the caller's payload   lifted: skate3_recomp.47.cpp:3931 (102 lines)
// STATUS: gate-1 (indirect call: sub_828E29C0's own bctrl, once per list node, through the function pointer at node+8)
// calls/session: boot 284  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_828E29C0.md
namespace port_828E29C0 {

constexpr uint32_t kHolder = 0;  // handle +0: the list holder, zeroed when the handle goes stale
constexpr uint32_t kToken = 4;   // handle +4: generation token; already negative means invalid
constexpr uint32_t kHolderHead = 0;        // holder +0: head of the listener list
constexpr uint32_t kHolderGeneration = 8;  // holder +8: the token the handle must still match
constexpr uint32_t kNodeNext = 0;      // node +0: next, RE-READ after each callback
constexpr uint32_t kNodeFunc = 8;      // node +8: the listener function pointer
constexpr uint32_t kNodeUserData = 12; // node +12: passed to the listener in r4

constexpr int32_t kStaleHandle = -3;  // written back into the handle before returning
constexpr int32_t kEmptyList = -4;
constexpr int32_t kNoHolder = -6;

REX_FUNC(Native) {
  const uint32_t handle = ctx.r3.u32;    // mr r11,r3
  const uint64_t payload = ctx.r4.u64;   // mr r30,r4
  const uint64_t entry_lr = ctx.lr;      // mflr r12 / stw r12,-8(r1)

  // lwz r9,4(r3) / cmpwi cr6,r9,0 / blt cr6,0x828e2a1c
  const uint32_t token = REX_LOAD_U32(handle + kToken);
  if (static_cast<int32_t>(token) < 0) {
    // mr r3,r9 -- r9 holds the ZERO-EXTENDED loaded word, so the result is 0x00000000FFFFFFxx,
    // not the sign-extended negative the `li` paths below leave. Reproduced deliberately.
    ctx.r3.u64 = static_cast<uint64_t>(token);
    ctx.lr = entry_lr;
    return;
  }

  // lwz r10,0(r3) / cmplwi cr6,r10,0 / bne cr6,0x828e2a00
  const uint32_t holder = REX_LOAD_U32(handle + kHolder);
  if (holder == 0) {
    ctx.r3.s64 = kNoHolder;  // li r9,-6 / mr r3,r9 -- sign-extended, unlike the path above
    ctx.lr = entry_lr;
    return;
  }

  // lwz r8,8(r10) / cmpw cr6,r9,r8 / beq cr6,0x828e2a24
  if (static_cast<int32_t>(token) !=
      static_cast<int32_t>(REX_LOAD_U32(holder + kHolderGeneration))) {
    // The generation moved on: mark the handle dead and report it. Store order as lifted,
    // +4 before +0.
    REX_STORE_U32(handle + kToken, static_cast<uint32_t>(kStaleHandle));  // stw r9,4(r11)
    REX_STORE_U32(handle + kHolder, 0);                                   // stw r10,0(r11)
    ctx.r3.s64 = kStaleHandle;                                            // mr r3,r9
    ctx.lr = entry_lr;
    return;
  }

  // lwz r31,0(r10) / cmplwi cr6,r31,0 / bne cr6,0x828e2a38
  uint32_t node = REX_LOAD_U32(holder + kHolderHead);
  if (node == 0) {
    ctx.r3.s64 = kEmptyList;  // li r3,-4
    ctx.lr = entry_lr;
    return;
  }

  do {
    // lwz r11,8(r31) / mr r3,r30 / lwz r4,12(r31) / mtctr r11 / bctrl
    // The listener takes the caller's payload in r3 and its own registration data in r4.
    ctx.ctr.u64 = REX_LOAD_U32(node + kNodeFunc);
    ctx.r3.u64 = payload;
    ctx.r4.u64 = REX_LOAD_U32(node + kNodeUserData);
    ctx.lr = 0x828E2A4C;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    // lwz r31,0(r31) -- `next` is read AFTER the callback ran, off the node it was called for.
    // A listener that frees its own node reads freed memory here. Reproduced as found.
    node = REX_LOAD_U32(node + kNodeNext);
  } while (node != 0);

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

}  // namespace port_828E29C0
SKATE3_PORT(828E29C0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
