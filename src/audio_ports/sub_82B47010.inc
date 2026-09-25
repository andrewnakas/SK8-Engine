// sub_82B47010  role: one run-length step of the variable-length decoder, returning the current value   lifted: skate3_recomp.68.cpp:33504 (107 lines)
// STATUS: verified
// calls/session: boot 12540  play 10300  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B47010.md
namespace port_82B47010 {

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)
constexpr uint32_t kScratch = 80;      // addi r4,r1,80 -- the decoded word lands here

// The sub-object in r3, as port_82B47658 describes it: {cursor* +0, accumulated +4,
// remaining +8, flag +12}. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kCursor = 0;
constexpr uint32_t kAccumulated = 4;
constexpr uint32_t kRemaining = 8;
constexpr uint32_t kFlag = 12;

// The cursor object the first field points at: its +0 is the live byte pointer into the stream.
constexpr uint32_t kStreamPointer = 0;

REX_FUNC(Native) {
  const uint64_t object = ctx.r3.u64;  // mr r31,r3
  const uint32_t entry_sp = ctx.r1.u32;

  // stwu r1,-112(r1). REPRODUCED: sp+80 is handed to sub_82B46EA0 as its output address, and
  // sub_82B46FB0 builds its own frame below this one. Skip the move and both land 112 bytes
  // higher, in the caller's frame.
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // lwz r11,8(r3) ; cmpwi cr6,r11,0 ; bgt cr6,0x82b47090 -- a SIGNED compare: the run is
  // exhausted at zero or below, and only then is a new code read.
  if (static_cast<int32_t>(REX_LOAD_U32(uint32_t(object) + kRemaining)) <= 0) {
    const uint32_t cursor = REX_LOAD_U32(uint32_t(object) + kCursor);        // lwz r30,0(r3)
    const uint64_t scratch = static_cast<uint64_t>(ctx.r1.s64 + kScratch);   // addi r4,r1,80

    // bl 0x82b46ea0 -- decode one signed code; byte length in r3, value at r4.
    skate3::audio::GuestCall(ctx, base, sub_82B46EA0,
                             REX_LOAD_U32(cursor + kStreamPointer),  // lwz r3,0(r30)
                             scratch);
    const uint64_t length = ctx.r3.u64;
    // lwz r10,0(r30) -- RELOADED after the call, though the callee never writes it. Kept.
    const uint64_t stream = REX_LOAD_U32(cursor + kStreamPointer);
    const uint64_t count = REX_LOAD_U32(uint32_t(scratch));  // lwz r11,80(r1)

    // add r8,r3,r10 ; stw r8,0(r30) -- 64-bit add, low word stored.
    REX_STORE_U32(cursor + kStreamPointer, uint32_t(length + stream));
    // addi r7,r11,1 ; stw r7,8(r31) -- likewise; a count of 0xFFFFFFFF stores 0.
    REX_STORE_U32(uint32_t(object) + kRemaining, uint32_t(count + 1));
    REX_STORE_U8(uint32_t(object) + kFlag, 1);  // stb r9,12(r31)

    // cmpwi cr6,r11,0 ; bge cr6,0x82b4707c -- SIGNED, on the decoded word. A negative code
    // rewrites both fields the two stores above just set; the dead stores stay.
    // Non-negative: flag 1, run length count+1, and sub_82B46FB0 runs only on this call, so the
    // following count calls return the accumulator unchanged -- a repeat run.
    // Negative: flag 0, run length 1-count, and sub_82B46FB0 runs on every call of the run,
    // advancing the accumulator each time -- a literal run.
    if (static_cast<int32_t>(count) < 0) {
      REX_STORE_U8(uint32_t(object) + kFlag, 0);  // stb r10,12(r31)
      // subfic r9,r11,1 -- 64-bit, so the borrow leaves bit 32 set; stw keeps the low word.
      REX_STORE_U32(uint32_t(object) + kRemaining,
                    uint32_t(static_cast<uint64_t>(1) - count));  // stw r9,8(r31)
    }

    // loc_82B4707C: lbz r11,12(r31) ; cmplwi ; beq -- the flag is RELOADED from memory rather
    // than carried in a register, which matters if the cursor object aliases this one.
    if (REX_LOAD_U8(uint32_t(object) + kFlag) != 0) {
      skate3::audio::GuestCall(ctx, base, sub_82B46FB0, object);  // bl 0x82b46fb0
    }
  }

  // loc_82B47090: lbz r11,12(r31) -- reloaded again. Exactly one of these two call sites runs
  // after a decode, because the flag decided which.
  if (REX_LOAD_U8(uint32_t(object) + kFlag) == 0) {
    skate3::audio::GuestCall(ctx, base, sub_82B46FB0, object);  // bl 0x82b46fb0
  }

  // loc_82B470A4
  const uint64_t remaining = REX_LOAD_U32(uint32_t(object) + kRemaining);  // lwz r11,8(r31)
  ctx.r3.u64 = REX_LOAD_U32(uint32_t(object) + kAccumulated);              // lwz r3,4(r31)
  // addi r11,r11,-1 ; stw r11,8(r31) -- loaded before the return value, stored after it.
  REX_STORE_U32(uint32_t(object) + kRemaining, uint32_t(remaining - 1));

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112, on the full 64-bit register
}

// Writes: the sub-object's +4 (through sub_82B46FB0), +8 and +12, declared as one nine-byte span
// exactly as port_82B47658 does; plus the cursor's byte pointer, written here and again by
// sub_82B46FB0. sub_82B46EA0's only store is the four bytes at its r4, which is this function's
// own frame scratch. Over-declaring +4 on the path where sub_82B46FB0 never runs is harmless:
// a windowed byte neither run writes compares equal.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;
  const int32_t remaining = static_cast<int32_t>(REX_LOAD_U32(object + kRemaining));
  const uint32_t flag = REX_LOAD_U8(object + kFlag);
  // The cursor is dereferenced unless the run is still live AND the flag is set: then neither
  // the decode branch nor either sub_82B46FB0 call site runs.
  if (remaining <= 0 || flag == 0) {
    const uint32_t cursor = REX_LOAD_U32(object + kCursor);
    if (cursor == 0) return false;  // the original would store through null; not comparable
    spec.write(cursor + kStreamPointer, 4);
    // Up to two codes are consumed on the decode path: sub_82B46EA0's, then sub_82B46FB0's from
    // the advanced pointer. Five bytes each, and reads are recorded, not compared.
    spec.read(REX_LOAD_U32(cursor + kStreamPointer), 10);
  }
  spec.write(object + kAccumulated, 9);  // +4, +8, +12
  spec.read(object, 16);
  return true;
}

}  // namespace port_82B47010
SKATE3_PORT(82B47010, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
