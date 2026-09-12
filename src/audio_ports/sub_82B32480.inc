// sub_82B32480  role: retire the finished entries of the 20-slot ring at +100 -- for each entry whose state byte reads exactly 2, clear the state, give the buffer's byte count back to the budget slot at u32[self+96] + class*80 + 24, hand the buffer to sub_82EC1378 unless the record is mid-flush, null the entry and advance the read index at +475 with a wrap at 20   lifted: skate3_recomp.67.cpp:49955 (119 lines)
// STATUS: gate-1 (sub_82EC1378 takes the heap critical section one level down: RtlEnterCriticalSection / RtlLeaveCriticalSection, reached from `bl 0x82ec1378` at 0x82B32504)
// calls/session: boot 205361  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B32480.md
namespace port_82B32480 {

// The same ~478-byte record port_82B32870 and sub_82B32328 work on; nothing in
// docs/rw_audio_structs.h names it, so plain offsets are honest. sub_82B32328 owns +432..+477,
// which is how the size is known and where the two index bytes below come from.
constexpr uint32_t kBudgetArray = 96;  // u32 -> an array of 80-byte records, indexed by entry +14
constexpr uint32_t kEntries = 100;     // the ring base, embedded in the record
constexpr uint32_t kReadIndex = 475;   // u8, this ring's read cursor
constexpr uint32_t kFlushing = 477;    // u8, set by sub_82B32328 while it flushes: suppresses the
                                       // release call but not the accounting

constexpr uint32_t kRingSlots = 20;    // addi r10,r11,-20 -- the wrap point
constexpr uint32_t kEntryStride = 16;  // rotlwi r11,r11,4

constexpr uint32_t kEntryBuffer = 0;  // u32, the buffer; u32[buffer+4] is its byte count
constexpr uint32_t kEntryOwner = 4;   // u32, sub_82EC1378's r3; zero skips the release
constexpr uint32_t kEntryState = 13;  // u8, exactly 2 selects this entry; cleared to 0
constexpr uint32_t kEntryClass = 14;  // u8, the budget-array index

constexpr uint32_t kBufferBytes = 4;        // u32[buffer+4]
constexpr uint32_t kBudgetStride = 80;      // rotlwi 2 ; add ; rlwinm 4,0,27 -- class * 80
constexpr uint32_t kBudgetOutstanding = 24; // u32, decremented by the buffer's byte count

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)

REX_FUNC(Native) {
  // The frame is reproduced because sub_82EC1378 runs on it. __savegprlr_29's spills of r29..r31
  // land inside these 112 bytes, so they are this call's own frame; they are not reproduced
  // because this body keeps those values in locals and never writes the guest registers.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f5211c
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-112(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;                            // mr r30,r3
  uint32_t index = REX_LOAD_U8(self + kReadIndex);             // lbz r11,475(r3)
  // rotlwi r11,r11,4 ; add r11,r11,r3 ; addi r31,r11,100 -- index came from a u8 load, so the
  // shift cannot carry out of 32 bits.
  uint32_t entry = self + (index * kEntryStride) + kEntries;

  // lbz r10,113(r11) ; cmplwi cr6,r10,2 ; bne cr6,0x82b32544 -- exactly 2, not merely nonzero.
  // The lifted form is a do-while with the same test at the bottom, against the NEW entry.
  while (REX_LOAD_U8(entry + kEntryState) == 2) {
    // loc_82B324B0 -- the buffer is read BEFORE the state is cleared; keep that order.
    const uint32_t buffer = REX_LOAD_U32(entry + kEntryBuffer);  // lwz r8,0(r31)
    REX_STORE_U8(entry + kEntryState, 0);                        // stb r29,13(r31), r29 = 0

    // cmplwi cr6,r8,0 ; beq cr6,0x82b3250c -- an empty entry is only advanced past.
    if (buffer != 0) {
      const uint32_t klass = REX_LOAD_U8(entry + kEntryClass);      // lbz r11,14(r31)
      const uint32_t array = REX_LOAD_U32(self + kBudgetArray);     // lwz r9,96(r30)
      const uint32_t bytes = REX_LOAD_U32(buffer + kBufferBytes);   // lwz r8,4(r8)
      // rotlwi r10,r11,2 ; add r7,r11,r10 ; rlwinm r11,r7,4,0,27 -- class*5 then *16. The adds
      // are 64-bit on zero-extended operands; klass is a u8 load, so the product is at most
      // 20400 and the 0xFFFFFFF0 mask drops nothing (its low four bits are already zero).
      const uint64_t quintuple = uint64_t(klass) + uint64_t(__builtin_rotateleft32(klass, 2));
      const uint64_t slot_off = (uint64_t(uint32_t(quintuple) << 4)) & 0xFFFFFFF0u;
      static_assert(kBudgetStride == 80, "class*5*16");
      const uint32_t slot = uint32_t(slot_off + uint64_t(array));  // add r11,r11,r9
      // lwz r6,24(r11) ; subf r5,r8,r6 ; stw r5,24(r11) -- a 64-bit subtract on two
      // zero-extended words, truncated at the store. It is allowed to go negative.
      const uint64_t remaining =
          uint64_t(REX_LOAD_U32(slot + kBudgetOutstanding)) - uint64_t(bytes);
      REX_STORE_U32(slot + kBudgetOutstanding, uint32_t(remaining));

      const uint32_t owner = REX_LOAD_U32(entry + kEntryOwner);  // lwz r3,4(r31)
      // cmplwi cr6,r3,0 ; beq cr6,0x82b32508 -- then lbz r11,477(r30) ; bne cr6,0x82b32508:
      // both tests must pass, and the +477 byte is read only when the owner is nonzero.
      if (owner != 0 && REX_LOAD_U8(self + kFlushing) == 0) {
        // lwz r4,0(r31) -- the buffer pointer is RELOADED here rather than reused from r8.
        // bl 0x82ec1378 -- the gate-1 call: it takes a critical section.
        skate3::audio::GuestCall(ctx, base, sub_82EC1378, owner,
                                 REX_LOAD_U32(entry + kEntryBuffer));
      }

      // loc_82B32508
      REX_STORE_U32(entry + kEntryBuffer, 0);  // stw r29,0(r31)
    }

    // loc_82B3250C -- advance the read index. +475 is RELOADED: the release call above can have
    // touched it.
    const uint32_t next = (REX_LOAD_U8(self + kReadIndex) + 1u) & 0xFFu;  // lbz ; addi ; clrlwi
    // addi r10,r11,-20 ; subfic r10,r10,0 ; subfe r8,r9,r9 ; and r7,r8,r11 -- subfic sets CA
    // only when next-20 is exactly zero, and `~r9 + r9` is all ones whatever r9 holds, so
    // subfe leaves 0 on the wrap and all ones otherwise. (r9 is not written on every path that
    // reaches this line; the idiom's result does not depend on it, which is why the compiler
    // could emit it.)
    index = (next == kRingSlots) ? 0u : next;
    REX_STORE_U8(self + kReadIndex, uint8_t(index));  // stb r7,475(r30)
    // rlwinm r11,r7,4,20,27 ; add r11,r11,r30 ; addi r31,r11,100 -- the 0xFF0 mask is exactly
    // index*16 for any u8 index.
    entry = self + (((index * kEntryStride)) & 0xFF0u) + kEntries;
  }

  // loc_82B32544 -- no `li r3,...` on any path: r3 is `self` until a release runs, and that
  // callee's result afterwards.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112
  ctx.lr = entry_lr;                      // b 0x82f5216c
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B32480
SKATE3_PORT(82B32480, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
