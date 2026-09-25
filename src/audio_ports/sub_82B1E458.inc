// sub_82B1E458  role: destroy an object -- queue an 8-byte {handler 0x82B49238, sub-object} record into the System command queue for the sub-object at +4, then run the object's own vtable slot +28 with r4 = 0 and free it through the global System's allocator (vtable slot +12)   lifted: skate3_recomp.67.cpp:1227 (89 lines)
// STATUS: gate-1 (two of its own bctrls, both unconditional: vtable slot +28 of the object at 0x82B1E4C0, then the free entry of u32[u32[0x8307762C]+36]'s vtable at 0x82B1E4DC)
// calls/session: boot 514  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1E458.md
namespace port_82B1E458 {

// Both computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye.
// lis r7,-32075 ; addi r6,r7,-28104 -> 0x82B50000 - 0x6DC8. A code address: sub_82B49238 is a
// real function in the corpus (docs/ports-static.md), so this is the record's handler word.
constexpr uint32_t kQueueHandler = 0x82B50000u - 28104u;  // 0x82B49238
// lis r10,-31993 ; lwz r30,30252(r10) -> 0x83070000 + 0x762C, the global System pointer
// sub_82B219E8 reads the same cell.
constexpr uint32_t kSystemPtr = 0x83070000u + 30252u;     // 0x8307762C

constexpr uint32_t kPending = 4;       // lwz r10,4(r3) -- the sub-object whose teardown defers
constexpr uint32_t kPendingSystem = 16;  // lwz r8,16(r10) -- the System* it hangs off
constexpr uint32_t kQueueBuffer = 48;    // System+0x30, docs/command-queue.md
constexpr uint32_t kQueueOffset = 204;   // System+0xCC, the write offset
constexpr uint32_t kRecordBytes = 8;     // addi r5,r11,8 -- this record is handler + object
constexpr uint32_t kDestroySlot = 28;    // lwz r9,28(r11) -- the object's own destroy entry
constexpr uint32_t kAllocator = 36;      // lwz r3,36(r30) -- the System's allocator
constexpr uint32_t kFreeSlot = 12;       // lwz r7,12(r8) -- the allocator vtable's free entry

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;

  // lwz r10,4(r3) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1e4a4
  const uint32_t pending = REX_LOAD_U32(object + kPending);
  if (pending != 0) {
    const uint32_t system = REX_LOAD_U32(pending + kPendingSystem);   // lwz r8,16(r10)
    const uint32_t offset = REX_LOAD_U32(system + kQueueOffset);      // lwz r11,204(r8)
    const uint32_t buffer = REX_LOAD_U32(system + kQueueBuffer);      // lwz r9,48(r8)
    // add r11,r9,r11 -- both operands zero-extended, so the sum is 64-bit; the stores truncate.
    const uint32_t record = static_cast<uint32_t>(static_cast<uint64_t>(buffer) +
                                                  static_cast<uint64_t>(offset));
    // BUG 1, docs/command-queue.md: the write offset is published BEFORE the record is stored,
    // so a consumer can see the advanced offset over a record that is not there yet. Reproduced
    // exactly -- the fix is not this port's business.
    REX_STORE_U32(system + kQueueOffset, offset + kRecordBytes);  // addi r5,r11,8 ; stw r5,204(r8)
    REX_STORE_U32(record + 0, kQueueHandler);                     // stw r6,0(r11)
    REX_STORE_U32(record + 4, pending);                           // stw r10,4(r11)
  }

  // loc_82B1E4A4
  const uint32_t vtable = REX_LOAD_U32(object + 0);         // lwz r11,0(r31)
  ctx.r4.s64 = 0;                                           // li r4,0
  ctx.r3.u64 = object;                                      // mr r3,r31
  const uint32_t destroy = REX_LOAD_U32(vtable + kDestroySlot);  // lwz r9,28(r11)
  // lwz r30,30252(r10) -- read BEFORE the call, into a callee-saved register, so the callee
  // cannot change which System this frees through. Kept as a local: the native body must not
  // clobber r30.
  const uint32_t system = REX_LOAD_U32(kSystemPtr);
  ctx.ctr.u64 = destroy;                                    // mtctr r9
  ctx.lr = 0x82B1E4C4;                                      // bctrl
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  const uint32_t allocator = REX_LOAD_U32(system + kAllocator);  // lwz r3,36(r30)
  ctx.r3.u64 = allocator;
  ctx.r5.s64 = 0;                                                // li r5,0
  ctx.r4.u64 = object;                                           // mr r4,r31
  const uint32_t avtable = REX_LOAD_U32(allocator + 0);          // lwz r8,0(r3)
  ctx.ctr.u64 = REX_LOAD_U32(avtable + kFreeSlot);               // lwz r7,12(r8) ; mtctr r7
  ctx.lr = 0x82B1E4E0;                                           // bctrl
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  // r3 is whatever the free returned: the function ends here, nothing overrides it.
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1E458
SKATE3_PORT(82B1E458, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
