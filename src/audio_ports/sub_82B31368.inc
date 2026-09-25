// sub_82B31368  role: alias for the voice-release routine at 0x82B31480   lifted: skate3_recomp.67.cpp:47422 (6 lines)
// STATUS: verified
// calls/session: boot 3206  play 3688  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B31368.md
namespace port_82B31368 {

// The voice object sub_82B31480 takes in r3. Nothing in docs/rw_audio_structs.h names this
// layout, so the offsets stay numbers.
constexpr uint32_t kChannelCount = 41;  // lbz r6,41(r3) -- passed on as a source-channel count
constexpr uint32_t kNode = 56;          // the intrusive list node sub_82B34BD0 unlinks
constexpr uint32_t kGainCount = 78;     // lbz r5,78(r3) -- also that node's own +22
constexpr uint32_t kGains = 80;         // eight gain words, cleared on the folding path
constexpr uint32_t kGainWords = 8;

// Node layout, identical to port_82B34BD0's: 82B31480 calls that function on one path and
// inlines it verbatim on the other.
constexpr uint32_t kPrev = 0;
constexpr uint32_t kNext = 4;
constexpr uint32_t kLiveWord = 8;
constexpr uint32_t kListToken = 12;
constexpr uint32_t kOwner = 16;
constexpr uint32_t kShortField = 20;
constexpr uint32_t kNodeGainCount = 22;
constexpr uint32_t kOwnerHead = 4;
constexpr uint32_t kOwnerGains = 12;
constexpr uint32_t kOwnerDirty = 46;

REX_FUNC(Native) {
  // b 0x82b31480 -- an unconditional tail branch, not a call: r3..r10 pass through untouched and
  // LR still holds this function's own return address, so the callee returns straight to our
  // caller. GuestCall with no arguments is exactly that machine state (it writes no argument
  // register and does not touch LR), and it keeps the callee behind its own hook.
  skate3::audio::GuestCall(ctx, base, sub_82B31480);
}

// sub_82B34BD0's write set, restated here because 82B31480 reaches that function on one path and
// inlines it on the other. `fold` is its r4 (the gain vector) being non-null.
bool UnlinkWindows(uint8_t* base, skate3::audio::PortSpec& spec, uint32_t node, bool fold,
                   uint32_t count) {
  spec.read(node, 24);
  if (REX_LOAD_U32(node + kListToken) == 0) {
    return true;  // beqlr / beq cr6 -- writes nothing at all
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
  if (node == head) spec.write(owner + kOwnerHead, 4);            // stw r10,4(r9)
  const uint32_t next = REX_LOAD_U32(node + kNext);
  const uint32_t prev = REX_LOAD_U32(node + kPrev);
  if (static_cast<int32_t>(next) != 0) spec.write(next + kPrev, 4);  // stw r9,0(r10)
  if (static_cast<int32_t>(prev) != 0) spec.write(prev + kNext, 4);  // stw r9,4(r10)
  spec.write(node + kLiveWord, 12);   // +8, +12, +16
  spec.write(node + kShortField, 3);  // sth +20, stb +22
  return true;
}

// Windows for the callee, since this function has no body of its own.
//
// sub_82B31480(voice):
//   if voice[+78] != 0:
//       builds two eight-entry pointer arrays inside its own 208-byte frame -- [sp+112..140]
//       pointing at frame scratch sp+144..172, [sp+80..108] pointing at &voice[+80..+108] --
//       then sub_82B468C0(sp+112, sp+80, voice[+78], voice[+41], 1) to gather the gains into that
//       scratch, sub_82B34BD0(voice+56, sp+144) to unlink and fold them into the owner, then
//       eight `stwu` zeroes over voice[+80..+108].
//   else:
//       sub_82B34BD0(voice+56, 0) inlined: the same unlink with no gain folding.
//
// sub_82B468C0's whole subtree writes ONLY into that frame. Its three callees are bounded by
// their own verified ports: sub_82B3BED8 and sub_82B44B20 write [r3, r3 + 4*r6) and the count
// here is the literal 1 that 82B31480 passes in r7, sub_82EE5E80 is memset and is called with
// the same 4; every destination pointer they get is an entry of the sp+112 array, i.e. a word of
// frame scratch. The voice's gains are the SOURCE side throughout. So the only writes above the
// stack pointer are sub_82B34BD0's plus the eight-word clear.
//
// The one thing that has to be checked from entry state is that the array indices stay inside the
// eight entries: both loop counts and the sub_82B426D0 table's destination index are bounded by
// voice[+78] and voice[+41], and past eight the original would read pointers out of the scratch
// area itself and store through them.
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
  // The gather's rodata, declared for replay only (reads cost nothing against the budget): the
  // scatter-mixer's gain table and its range and route tables, and the copy path's unity gain.
  spec.read(0x820ED6C0u, 16);
  spec.read(0x820ED700u, 128);
  spec.read(0x820ED780u, 256);
  spec.read(0x8231A844u, 4);
  if (!UnlinkWindows(base, spec, node, true, gain_count)) return false;
  spec.write(voice + kGains, 4 * kGainWords);  // stwu r9,4(r11) x8
  return true;
}

}  // namespace port_82B31368
SKATE3_PORT(82B31368, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
