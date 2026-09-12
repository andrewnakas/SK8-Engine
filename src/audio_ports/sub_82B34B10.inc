// sub_82B34B10  role: push an object's node onto a global singly-linked list   lifted: skate3_recomp.67.cpp:55556 (36 lines)
// STATUS: verified
// calls/session: boot 215  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34B10.md
namespace port_82B34B10 {

// lis r10,-31988 ; lwz r9,-8520(r10) -- 0x830C0000 - 0x2148. The list head, a fixed global.
constexpr uint32_t kListHead = 0x830BDEB8;
constexpr uint32_t kRecordTarget = 4;  // the object this record acts on
constexpr uint32_t kNodeOffset = 44;   // {next at +44, prev-ish word at +48} on the object
constexpr uint32_t kLinkedByte = 164;
constexpr uint32_t kBackLink = 4;      // written on the previous head's node
constexpr uint32_t kRecordSize = 8;    // returned in r3

REX_FUNC(Native) {
  const uint32_t object = REX_LOAD_U32(ctx.r3.u32 + kRecordTarget);  // lwz r8,4(r3)
  const uint32_t node = object + kNodeOffset;                        // addi r11,r8,44
  const uint32_t head = REX_LOAD_U32(kListHead);                     // lwz r9,-8520(r10)
  REX_STORE_U32(object + 48, 0);                                     // stw r7,48(r8)
  REX_STORE_U32(object + kNodeOffset, head);                         // stw r9,44(r8)
  // lwz r9,-8520(r10) again: the compiler could not prove the two stores above miss the head
  // cell, so it reloads it. Reproduce the reload rather than reusing the value.
  const uint32_t head_again = REX_LOAD_U32(kListHead);
  if (head_again != 0) {
    REX_STORE_U32(head_again + kBackLink, node);  // stw r11,4(r9)
  }
  REX_STORE_U32(kListHead, node);                             // stw r11,-8520(r10)
  ctx.r3.s64 = kRecordSize;                                   // li r3,8
  REX_STORE_U8(object + kLinkedByte, 1);                      // stb r9,164(r8)
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = REX_LOAD_U32(ctx.r3.u32 + kRecordTarget);
  if (object == 0) return false;  // the original would store through null
  const uint32_t node = object + kNodeOffset;
  // If the head cell sits inside the two words this writes first, the reloaded head is a value
  // produced DURING the call, so the back-link address is not knowable from entry state. That
  // is gate 2, and it is predicted rather than assumed away.
  if (kListHead + 3 >= object + kNodeOffset && kListHead <= object + 51) return false;
  const uint32_t head = REX_LOAD_U32(kListHead);
  spec.write(object + kNodeOffset, 8);
  spec.write(kListHead, 4);
  spec.write(object + kLinkedByte, 1);
  if (head != 0) spec.write(head + kBackLink, 4);
  spec.read(ctx.r3.u32, kRecordSize);
  spec.read(kListHead, 4);
  return true;
}

}  // namespace port_82B34B10
SKATE3_PORT(82B34B10, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
