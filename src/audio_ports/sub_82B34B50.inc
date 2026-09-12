// sub_82B34B50  role: retire a record -- unlink its intrusive node at +44 from the global registry list when the flag at +164 says it is linked, then tail-call sub_82B34D60 on the held object at +52 with the owner from +8   lifted: skate3_recomp.67.cpp:55593 (55 lines)
// STATUS: gate-1 (the tail-callee frees through the owner's allocator vtable slot +12: sub_82B34D60 indirect call, `bctr` at 0x82B34E00)
// calls/session: boot 244  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34B50.md
namespace port_82B34B50 {

// ((-31988 & 0xFFFF) << 16) + (-8520) = 0x830C0000 - 0x2148. Computed, never read off by eye.
constexpr uint32_t kRegistryHead = 0x830C0000u - 8520u;  // lis r9,-31988 ; lwz r10,-8520(r9)

// Nothing in docs/rw_audio_structs.h names this record; plain offsets are honest. The owner at
// +8 and the allocator at owner+36 are the same pair sub_82B32870 and sub_82B34D60 use.
constexpr uint32_t kLinked = 164;  // u8   nonzero: the node at +44 is on the registry list
constexpr uint32_t kOwner = 8;     // ->owner, handed to sub_82B34D60 as its r4
constexpr uint32_t kNode = 44;     // intrusive registry node {next at +0, prev at +4}
constexpr uint32_t kHeld = 52;     // the sub-object sub_82B34D60 tears down
constexpr uint32_t kNext = 0;
constexpr uint32_t kPrev = 4;

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r8,r3

  // lbz r11,164(r3) ; cmplwi cr6,r11,0 ; beq cr6,0x82b34ba4
  if (REX_LOAD_U8(object + kLinked) != 0) {
    const uint32_t node = object + kNode;               // addi r11,r3,44
    const uint32_t head = REX_LOAD_U32(kRegistryHead);  // lwz r10,-8520(r9)
    // cmplw cr6,r11,r10 ; bne cr6,0x82b34b7c -- unsigned; the node is the list head
    if (node == head) {
      // lwz r10,0(r10) ; stw r10,-8520(r9). head == node here, so this is node->next; written
      // through r10 in the original, which is why it is not read from `node`.
      REX_STORE_U32(kRegistryHead, REX_LOAD_U32(head + kNext));
    }
    // loc_82B34B7C: lwz r10,4(r11) ; cmpwi cr6,r10,0 ; beq cr6,0x82b34b90 -- signed test
    const uint32_t prev = REX_LOAD_U32(node + kPrev);
    if (static_cast<int32_t>(prev) != 0) {
      // lwz r9,0(r11) ; stw r9,0(r10) -- prev->next = node->next
      REX_STORE_U32(prev + kNext, REX_LOAD_U32(node + kNext));
    }
    // loc_82B34B90. Both node words are re-read here: the store above aliases node->next when
    // the list is one or two elements long, and the original reloads, so the port reloads.
    const uint32_t next = REX_LOAD_U32(node + kNext);  // lwz r10,0(r11)
    // cmpwi cr6,r10,0 ; beq cr6,0x82b34ba4
    if (static_cast<int32_t>(next) != 0) {
      // lwz r11,4(r11) ; stw r11,4(r10) -- next->prev = node->prev
      REX_STORE_U32(next + kPrev, REX_LOAD_U32(node + kPrev));
    }
  }

  // loc_82B34BA4: lwz r4,8(r8) ; addi r3,r8,52 ; b 0x82b34d60. A tail call: lr is untouched and
  // r3 is whatever sub_82B34D60 leaves. Through GuestCall on the live context so the callee's
  // own hook decides how it runs.
  const uint32_t owner = REX_LOAD_U32(object + kOwner);
  skate3::audio::GuestCall(ctx, base, sub_82B34D60, object + kHeld, owner);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B34B50
SKATE3_PORT(82B34B50, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
