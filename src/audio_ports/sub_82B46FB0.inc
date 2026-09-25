// sub_82B46FB0  role: decode one signed code into the accumulator and advance the stream cursor   lifted: skate3_recomp.68.cpp:33450 (53 lines)
// STATUS: verified
// calls/session: boot 15048  play 12360  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B46FB0.md
namespace port_82B46FB0 {

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)
constexpr uint32_t kScratch = 80;      // addi r4,r1,80 -- the decoded word lands here

// The sub-object in r3, as port_82B47658 already describes it: {cursor* +0, accumulated +4,
// remaining +8, flag +12}. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kCursor = 0;
constexpr uint32_t kAccumulated = 4;

// The cursor object the first field points at: its +0 is the live byte pointer into the stream.
constexpr uint32_t kStreamPointer = 0;

REX_FUNC(Native) {
  const uint64_t object = ctx.r3.u64;  // mr r31,r3
  const uint32_t entry_sp = ctx.r1.u32;

  // stwu r1,-112(r1). REPRODUCED: the decoded word goes to sp+80 and that address is handed to
  // sub_82B46EA0, so without the move it would land 112 bytes higher, in the caller's frame, and
  // the callee's own scratch below r1 could land on top of it.
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  const uint32_t cursor = REX_LOAD_U32(uint32_t(object) + kCursor);       // lwz r30,0(r3)
  const uint64_t scratch = static_cast<uint64_t>(ctx.r1.s64 + kScratch);  // addi r4,r1,80

  // bl 0x82b46ea0 -- decode one signed zigzag variable-length integer; returns its byte length in
  // r3 and stores the value at r4. The original does not spill anything around this call that it
  // needs afterwards except r30/r31, which this body keeps in C++ locals instead, so r13-r31 are
  // left at their entry values rather than saved and restored.
  skate3::audio::GuestCall(ctx, base, sub_82B46EA0,
                           REX_LOAD_U32(cursor + kStreamPointer),  // lwz r3,0(r30)
                           scratch);
  const uint64_t length = ctx.r3.u64;

  // lwz r10,0(r30) -- RELOADED after the call even though the callee never writes it. Kept.
  const uint64_t stream = REX_LOAD_U32(cursor + kStreamPointer);
  const uint64_t decoded = REX_LOAD_U32(uint32_t(scratch));  // lwz r11,80(r1)
  // add r9,r3,r10 -- 64-bit on two zero-extended values; the carry out of bit 31 lives only in
  // r9, which is volatile, and the stw discards it. Hence the 64-bit intermediate, cast here.
  REX_STORE_U32(cursor + kStreamPointer, uint32_t(length + stream));  // stw r9,0(r30)

  const uint64_t accumulated = REX_LOAD_U32(uint32_t(object) + kAccumulated);  // lwz r10,4(r31)
  // add r8,r10,r11 -- same again; the decoded word may be a negative value as 0xFFFF....
  REX_STORE_U32(uint32_t(object) + kAccumulated,
                uint32_t(accumulated + decoded));  // stw r8,4(r31)

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112, on the full 64-bit register
}

// Two words: the cursor's byte pointer and the sub-object's accumulator. sub_82B46EA0's only
// store is the 4 bytes at its r4, which is this function's own frame scratch, so it needs no
// window. Both addresses are read from entry state before either store, which is what makes this
// enumerable even though the census flags the cursor as a derived base.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;
  const uint32_t cursor = REX_LOAD_U32(object + kCursor);
  if (cursor == 0) return false;  // stw r9,0(r30) would store through null; not comparable
  spec.write(cursor + kStreamPointer, 4);
  spec.write(object + kAccumulated, 4);
  spec.read(object, 8);
  spec.read(REX_LOAD_U32(cursor + kStreamPointer), 5);  // at most a five-byte code
  return true;
}

}  // namespace port_82B46FB0
SKATE3_PORT(82B46FB0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
