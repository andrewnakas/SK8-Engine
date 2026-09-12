// sub_82B1BF68  role: detach word at +16: return its old value in r3 and clear it to 0   lifted: skate3_recomp.66.cpp:63530 (13 lines)
// STATUS: verified
// calls/session: boot 52325  play 69016  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1BF68.md
namespace port_82B1BF68 {

// Nothing in docs/rw_audio_structs.h has this shape, so the offset stays plain. Address sibling
// sub_82B1BF80 is the same function over +0, and sub_82B1BF60 is a bare `blr`: a family of small
// method slots over one object, reached by function pointer.
constexpr uint32_t kDetachedWord = 16;

REX_FUNC(Native) {
  // The load and the store touch the SAME word, so the order is load-then-clear, and `object`
  // has to be taken before r3 is overwritten by the loaded value.
  const uint32_t object = ctx.r3.u32;                                 // mr r11,r3
  ctx.r3.u64 = REX_LOAD_U32(object + kDetachedWord);                  // lwz r3,16(r3)  (zero-extended into all 64 bits)
                                                                      // li r10,0
  REX_STORE_U32(object + kDetachedWord, 0);                           // stw r10,16(r11)
}

// One store, four bytes, at an offset off the entry pointer: enumerable from entry state alone.
// The same word is also the only thing read, declared so a recorded vector can be replayed.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.write(object + kDetachedWord, 4);  // stw r10,16(r11)
  spec.read(object + kDetachedWord, 4);   // lwz r3,16(r3)
  return true;
}

}  // namespace port_82B1BF68
SKATE3_PORT(82B1BF68, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
