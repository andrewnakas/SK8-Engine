// sub_82B31480  role: release a voice -- when it still holds per-channel gains, gather them through sub_82B468C0 into frame scratch, unlink and fold them into the owner with sub_82B34BD0, then zero the eight gain words; otherwise the same unlink, inlined, with no fold   lifted: skate3_recomp.67.cpp:47572 (193 lines)
// STATUS: verified
// calls/session: boot 12908  play 14922  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B31480.md
namespace port_82B31480 {

// r3 = the voice. Nothing in docs/rw_audio_structs.h names this layout, so the offsets stay
// numbers; the names match port_82B31368, which windows this same function from its tail branch.
constexpr uint32_t kChannelCount = 41;  // u8  lbz r6,41(r3) -- passed on as a source-channel count
constexpr uint32_t kNode = 56;          // the intrusive list node sub_82B34BD0 unlinks
constexpr uint32_t kGainCount = 78;     // u8  lbz r5,78(r3) -- also the node's own +22
constexpr uint32_t kGains = 80;         // eight gain words, the source side then cleared
constexpr uint32_t kGainWords = 8;

// Node layout, identical to port_82B34BD0's: this function calls that function on the fold arm
// and inlines it verbatim on the other.
constexpr uint32_t kPrev = 0;
constexpr uint32_t kNext = 4;
constexpr uint32_t kLiveWord = 8;
constexpr uint32_t kListToken = 12;  // zero means already detached: the unlink does nothing
constexpr uint32_t kOwner = 16;
constexpr uint32_t kShortField = 20;
constexpr uint32_t kNodeGainCount = 22;
constexpr uint32_t kOwnerHead = 4;
constexpr uint32_t kOwnerGains = 12;
constexpr uint32_t kOwnerDirty = 46;

// The 208-byte frame, and the three areas inside it the callees address.
constexpr uint32_t kFrameBytes = 208;
constexpr uint32_t kSourceArray = 80;   // eight pointers, &voice[+80 + 4i]
constexpr uint32_t kDestArray = 112;    // eight pointers, &frame[kScratch + 4i]
constexpr uint32_t kScratch = 144;      // eight gathered gain words, sub_82B34BD0's r4

REX_FUNC(Native) {
  const uint64_t voice64 = ctx.r3.u64;       // mr r31,r3 -- a 64-bit copy; the addi below is too
  const uint32_t voice = ctx.r3.u32;
  const uint32_t gain_count = REX_LOAD_U8(voice + kGainCount);  // lbz r5,78(r3)

  // stwu r1,-208(r1). The frame move IS reproduced, unlike most ports: sub_82B468C0 allocates its
  // own 144 bytes and spills r25-r31 and lr below r1 through __savegprlr_25, which would land on
  // the two pointer arrays if they stayed in this function's red zone. The low word is assigned on
  // its own, exactly as the lifted form does, so the closing 64-bit addi undoes it bit for bit.
  const uint32_t sp = ctx.r1.u32 - kFrameBytes;
  REX_STORE_U32(sp, ctx.r1.u32);  // the back chain; inside this function's own frame
  ctx.r1.u32 = sp;
  // The r12/r30/r31 spills at the caller's -8/-16/-24 are NOT reproduced: nothing reads them, and
  // r30/r31 are left untouched here, so their exit values are their entry values either way.

  if (gain_count != 0) {  // cmplwi cr6,r5,0 ; beq cr6,0x82b31560
    const uint32_t channels = REX_LOAD_U8(voice + kChannelCount);  // lbz r6,41(r3)
    // Sixteen stores, interleaved in the original (112, 84, 88, 92, 80, 116, 120, 124, 96, 128,
    // 100, 132, 104, 136, 108, 140) purely for scheduling: sixteen distinct frame words, each
    // written once, none of them read before the call.
    for (uint32_t i = 0; i < kGainWords; i++) {
      // addi r10,r3,84 / addi r9,r31,100 ... -- 64-bit adds the stw truncates
      REX_STORE_U32(sp + kSourceArray + 4 * i, uint32_t(voice64 + kGains + 4 * i));
      // addi r11,r1,144 ... -- likewise off the moved r1
      REX_STORE_U32(sp + kDestArray + 4 * i, uint32_t(ctx.r1.u64 + kScratch + 4 * i));
    }
    // bl 0x82b468c0 -- gather: for each of the first `gain_count` entries, one gain word is copied
    // or accumulated from *source[i] into *dest[i], the literal 1 in r7 being the element count.
    skate3::audio::GuestCall(ctx, base, sub_82B468C0, ctx.r1.u64 + kDestArray,
                             ctx.r1.u64 + kSourceArray, uint64_t(gain_count), uint64_t(channels),
                             uint64_t(1));
    // bl 0x82b34bd0 -- unlink the node and fold the gathered gains into the owner.
    skate3::audio::GuestCall(ctx, base, sub_82B34BD0, voice64 + kNode, ctx.r1.u64 + kScratch);
    // loc_82B31554: stwu r9,4(r11) x8 off r11 = voice+76 -- voice[+80 .. +111] cleared.
    for (uint32_t i = 0; i < kGainWords; i++) {
      REX_STORE_U32(voice + kGains + 4 * i, 0);
    }
  } else {
    // loc_82B31560 -- sub_82B34BD0 with a null gain vector, inlined: the same three list fixups
    // and the same five clears, in the same order, with the same reloads.
    const uint32_t node = voice + kNode;                   // addi r11,r31,56
    if (REX_LOAD_U32(node + kListToken) != 0) {            // lwz r10,68(r31) ; cmplwi ; beq
      const uint32_t owner = REX_LOAD_U32(node + kOwner);  // lwz r9,16(r11)
      const uint32_t head = REX_LOAD_U32(owner + kOwnerHead);  // lwz r10,4(r9)
      if (node == head) {                                      // cmplw cr6,r11,r10 ; bne
        // lwz r10,0(r10) ; stw r10,4(r9)
        REX_STORE_U32(owner + kOwnerHead, REX_LOAD_U32(head + kPrev));
      }
      const uint32_t next = REX_LOAD_U32(node + kNext);  // lwz r10,4(r11)
      if (int32_t(next) != 0) {                          // cmpwi cr6,r10,0 ; beq -- SIGNED
        // lwz r9,0(r11) ; stw r9,0(r10)
        REX_STORE_U32(next + kPrev, REX_LOAD_U32(node + kPrev));
      }
      const uint32_t prev = REX_LOAD_U32(node + kPrev);  // lwz r10,0(r11) -- reloaded
      if (int32_t(prev) != 0) {                          // cmpwi cr6,r10,0 ; beq
        // lwz r9,4(r11) ; stw r9,4(r10)
        REX_STORE_U32(prev + kNext, REX_LOAD_U32(node + kNext));
      }
      REX_STORE_U32(node + kListToken, 0);     // stw r10,12(r11)
      REX_STORE_U32(node + kOwner, 0);         // stw r10,16(r11)
      REX_STORE_U32(node + kLiveWord, 0);      // stw r10,8(r11)
      REX_STORE_U8(node + kNodeGainCount, 0);  // stb r10,22(r11)
      REX_STORE_U16(node + kShortField, 0);    // sth r10,20(r11)
    }
  }

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,208 -- a 64-bit add, as in the lifted form
}

// sub_82B34BD0's write set, restated because this function reaches it on one arm and inlines it on
// the other. `fold` is its r4 (the gathered gain vector) being non-null. Identical to
// port_82B31368::UnlinkWindows, which windows this same function through its tail branch.
bool UnlinkWindows(uint8_t* base, skate3::audio::PortSpec& spec, uint32_t node, bool fold,
                   uint32_t count) {
  spec.read(node, 24);
  if (REX_LOAD_U32(node + kListToken) == 0) {
    return true;  // beq cr6 / beqlr -- writes nothing at all
  }
  const uint32_t owner = REX_LOAD_U32(node + kOwner);
  if (fold) {
    spec.write(owner + kOwnerDirty, 1);  // stb r9,46(r11)
    if (count != 0) {
      // The owner pointer and the count are both re-read inside the fold loop. If either field
      // lives inside the accumulated range its value changes mid-call, and then neither the
      // addresses nor the trip count derive from entry state -- gate 2.
      const uint32_t lo = owner + kOwnerGains;
      const uint32_t hi = lo + 4 * count;
      if (node + kOwner + 3 >= lo && node + kOwner < hi) return false;
      if (node + kNodeGainCount >= lo && node + kNodeGainCount < hi) return false;
      spec.write(lo, 4 * count);  // stfs f12,0(r9) per gain
    }
  }
  const uint32_t head = REX_LOAD_U32(owner + kOwnerHead);
  spec.read(owner + kOwnerHead, 4);
  if (node == head) spec.write(owner + kOwnerHead, 4);  // stw r10,4(r9)
  const uint32_t next = REX_LOAD_U32(node + kNext);
  const uint32_t prev = REX_LOAD_U32(node + kPrev);
  if (int32_t(next) != 0) spec.write(next + kPrev, 4);  // stw r9,0(r10)
  if (int32_t(prev) != 0) spec.write(prev + kNext, 4);  // stw r9,4(r10)
  spec.write(node + kLiveWord, 12);   // +8, +12, +16
  spec.write(node + kShortField, 3);  // sth +20, stb +22
  return true;
}

// Every byte the call writes above the stack pointer, from entry state.
//
// The fold arm's whole callee subtree writes ONLY into this function's own 208-byte frame:
// sub_82B468C0 hands sub_82B3BED8 / sub_82B44B20 / sub_82EE5E80 a destination taken from the
// kDestArray entries, i.e. a word of kScratch, and each of those writes exactly [dst, dst + 4*n)
// with n the literal 1 passed in r7 (so never the dcbzl vector path, which needs n % 64 == 0).
// sub_82B426D0, reached when both counts are in {1,2,4,6,8}, indexes the same two arrays with
// (byte & 7) out of a read-only table pair at 0x820CD700 / 0x820CD780 and keeps its visited flags
// in its own frame. So the only writes this call makes above r1 are sub_82B34BD0's plus the
// eight-word gain clear -- which is why the fold arm is NOT gate 2 here even though it is from
// sub_82B31680, whose entry state cannot name a frame this function has not built yet.
//
// The one thing that must be checked from entry state is that the array indices stay inside the
// eight entries: past eight the original reads pointers out of the uninitialised part of the frame
// and stores through them, and those addresses are not entry state.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t voice = ctx.r3.u32;
  if (voice == 0) return false;  // the original would load through null; not comparable
  const uint32_t gain_count = REX_LOAD_U8(voice + kGainCount);
  spec.read(voice + kGainCount, 1);
  const uint32_t node = voice + kNode;

  if (gain_count == 0) {
    // lbz r5,78(r3) ; beq cr6,0x82b31560 -- the inlined unlink, nothing else runs.
    return UnlinkWindows(base, spec, node, false, 0);
  }

  const uint32_t channels = REX_LOAD_U8(voice + kChannelCount);
  spec.read(voice + kChannelCount, 1);
  if (gain_count > kGainWords || channels > kGainWords) {
    return false;  // would index past the eight-entry frame arrays: not enumerable
  }
  spec.read(voice + kGains, 4 * kGainWords);
  if (!UnlinkWindows(base, spec, node, true, gain_count)) return false;
  spec.write(voice + kGains, 4 * kGainWords);  // stwu r9,4(r11) x8
  return true;
}

}  // namespace port_82B31480
SKATE3_PORT(82B31480, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
