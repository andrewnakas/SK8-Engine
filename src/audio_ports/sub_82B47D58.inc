// sub_82B47D58  role: drop one reference -- decrement the u16 count at +24 and, on the last one, release the handle at +20 and clear the in-use byte at +26   lifted: skate3_recomp.68.cpp:35424 (48 lines)
// STATUS: gate-1 (RtlEnterCriticalSection: sub_82EC1180 takes a lock, and it is the release itself)
// calls/session: boot 19  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B47D58.md
namespace port_82B47D58 {

constexpr uint32_t kRefCount = 24;  // u16; nothing in docs/rw_audio_structs.h names it
constexpr uint32_t kHandle = 20;    // the pointer handed to sub_82EC1180
constexpr uint32_t kInUse = 26;     // u8, cleared once the count reaches zero

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r31,r3 -- kept across the call
  // lhz r11,24(r3) ; addi r10,r11,-1 ; extsh r9,r10
  // The count is loaded zero-extended, decremented in 64 bits, then the LOW HALFWORD is
  // sign-extended: that is what both the store and the compare below see, so a count of 0
  // wraps to -1 and takes the early exit rather than releasing.
  const int32_t remaining =
      static_cast<int16_t>(static_cast<uint16_t>(REX_LOAD_U16(object + kRefCount) - 1));
  REX_STORE_U16(object + kRefCount, static_cast<uint16_t>(remaining));  // sth r9,24(r3)
  // cmpwi cr6,r9,0 ; bne cr6,0x82b47d94
  if (remaining != 0) return;
  // lwz r3,20(r3) ; bl 0x82ec1180
  skate3::audio::GuestCall(ctx, base, sub_82EC1180, REX_LOAD_U32(object + kHandle));
  // li r11,0 ; stb r11,26(r31) -- r31, not r3: the callee clobbered r3.
  REX_STORE_U8(object + kInUse, 0);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B47D58
SKATE3_PORT(82B47D58, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
