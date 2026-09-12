// sub_82B470D0  role: parse a stream header -- dispatch on the version byte, inline the version-0 header, then hand the record payload to one of two record parsers; zero the object if any of them fails   lifted: skate3_recomp.68.cpp:33612 (155 lines)
// STATUS: verified
// calls/session: boot 1254  play 1030  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B470D0.md
namespace port_82B470D0 {

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)

// The object in r3. Nothing in docs/rw_audio_structs.h names this layout, so the offsets stay
// numbers; the comments say only what the code shows. Everything from +0 to the byte at +32 is
// written by this function or by one of its three callees, so one 33-byte window covers the lot.
constexpr uint32_t kOffset0 = 0;    // zeroed here; sub_82B471D8 stores a stream pointer
constexpr uint32_t kOffset4 = 4;    // a stream pointer derived from the header, or zero
constexpr uint32_t kOffset8 = 8;    // written only by the two record parsers
constexpr uint32_t kOffset12 = 12;  // likewise
constexpr uint32_t kOffset20 = 20;  // likewise
constexpr uint32_t kOffset24 = 24;  // the LOW nibble of the second header byte
constexpr uint32_t kOffset28 = 28;  // the big-endian u16 at stream[2..3]
constexpr uint32_t kObjectBytes = 33;  // +0 .. +32 inclusive; +32 is a single byte (stb)

// The version-0 fixed header: version, nibbles, a u16, a u32. The record payload starts at +8.
constexpr uint32_t kHeaderBytes = 8;

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;    // mr r31,r3
  const uint32_t stream = ctx.r4.u32;  // mr r10,r4

  // stwu r1,-128(r1). REPRODUCED: all three callees build their own frames below this one
  // (sub_82B472C0 uses the red zone at r1-129..r1-144 without a frame of its own), so skipping
  // the move would put every one of their stack writes 128 bytes higher than the original's.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // lbz r11,0(r4) ; extsb r11,r11 ; cmplwi cr6,r11,1 -- the byte is SIGN-extended and then
  // compared UNSIGNED, so 0x80..0xFF land above 1 along with 0x02..0x7F: only 0 and 1 have
  // their own path.
  const uint32_t version =
      static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(REX_LOAD_U8(stream))));

  if (version > 1) {
    ctx.r3.s64 = 1;  // li r3,1 -- failure, which the tail below turns into a wipe
  } else if (version == 1) {
    // loc_82B47104: mr r4,r10 ; mr r3,r31 ; bl 0x82b471d8. r5 is NOT set here: this function's
    // third argument flows through untouched to whichever record parser 82B471D8 picks, so
    // GuestCall is given two arguments only.
    skate3::audio::GuestCall(ctx, base, sub_82B471D8, self, stream);
  } else {
    // loc_82B47114 -- the version-0 header, parsed in place. The load and store order below is
    // the lifted one: self+0 and self+24 are written BEFORE stream[2..7] are read, which matters
    // if the object overlaps the stream.
    const uint32_t nibbles = REX_LOAD_U8(stream + 1);  // lbz r9,1(r10)
    REX_STORE_U32(self + kOffset0, 0);                 // stw r30,0(r31), r30 = 0 throughout
    const uint32_t low = nibbles & 0xF;                // clrlwi r8,r9,28
    const uint32_t high = nibbles >> 4;                // rlwinm r9,r9,28,4,31, on a byte
    REX_STORE_U32(self + kOffset24, low);              // stw r8,24(r31)

    const uint32_t b2 = REX_LOAD_U8(stream + 2);  // lbzu r7,1(r11)
    const uint32_t b3 = REX_LOAD_U8(stream + 3);  // lbz r6,1(r11)
    const uint32_t b4 = REX_LOAD_U8(stream + 4);  // lbzu r3,2(r11)
    const uint32_t b5 = REX_LOAD_U8(stream + 5);  // lbz r8,1(r11)
    const uint32_t b6 = REX_LOAD_U8(stream + 6);  // lbz r7,2(r11)
    const uint32_t b7 = REX_LOAD_U8(stream + 7);  // lbz r6,3(r11)

    // The original assembles both multi-byte fields through four scratch bytes at sp+80: two
    // stb then lhz, then four more stb over the same bytes then lwz. That scratch is its own
    // frame and nothing else ever sees it, so the two big-endian reads are written directly.
    // The lhz happens BEFORE the second group of stb, so the u16 is stream[2..3].
    const uint32_t field28 = (b2 << 8) | b3;                        // lhz r29,80(r1)
    const uint32_t offset = (b4 << 24) | (b5 << 16) | (b6 << 8) | b7;  // lwz r11,80(r1)

    REX_STORE_U32(self + kOffset28, field28);  // stw r29,28(r31)
    if (offset != 0) {                         // cmplwi cr6,r11,0 ; beq cr6,0x82b47180
      // add r11,r11,r10 ; stw r11,4(r31) -- a 64-bit add, the low word stored.
      REX_STORE_U32(self + kOffset4,
                    static_cast<uint32_t>(static_cast<uint64_t>(offset) +
                                          static_cast<uint64_t>(stream)));
    } else {
      REX_STORE_U32(self + kOffset4, 0);  // loc_82B47180: stw r30,4(r31)
    }

    // loc_82B47184: cmplwi cr6,r9,1 on the HIGH nibble, which is 0..15, so this is a plain
    // three-way. r4 = stream + 8 (addi r4,r11,4 with r11 already at stream+4) is the record
    // payload both parsers take; r5 again flows through untouched.
    const uint32_t payload = stream + kHeaderBytes;
    if (high > 1) {
      ctx.r3.u64 = 0;  // mr r3,r30 -- not a failure: the tail leaves the object alone
    } else if (high == 1) {
      // loc_82B47198: mr r3,r31 ; bl 0x82b474b8
      skate3::audio::GuestCall(ctx, base, sub_82B474B8, self, payload);
    } else {
      // loc_82B471A4: mr r3,r31 ; bl 0x82b472c0
      skate3::audio::GuestCall(ctx, base, sub_82B472C0, self, payload);
    }
  }

  // loc_82B471AC: clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; beq -- only the LOW BYTE of the status
  // decides, so a status of 0x100 would read as success. ctx.r3 is left exactly as the branch or
  // the callee left it, because it is this function's result.
  if ((ctx.r3.u32 & 0xFF) != 0) {
    REX_STORE_U32(self + kOffset0, 0);   // stw r30,0(r31)
    REX_STORE_U32(self + kOffset4, 0);   // stw r30,4(r31)
    REX_STORE_U32(self + kOffset8, 0);   // stw r30,8(r31)
    REX_STORE_U32(self + kOffset12, 0);  // stw r30,12(r31)
    REX_STORE_U32(self + kOffset20, 0);  // stw r30,20(r31)
    REX_STORE_U32(self + kOffset24, 0);  // stw r30,24(r31)
    // +16, +28 and +32 are deliberately NOT cleared. Reproduced as found.
  }

  // loc_82B471D0: addi r1,r1,128, then __restgprlr_29 puts back r29-r31 and lr. Nothing here
  // writes r29-r31, so only the stack pointer has to be restored.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
}

// Every byte the call writes outside a stack frame, from entry state -- and it is one span, with no
// loop to bound, because every store in the whole subtree is a fixed offset off this function's r3:
//
//   here            +0, +4, +24, +28, and the failure wipe over +0, +4, +8, +12, +20, +24
//   sub_82B471D8    +0, +4, +24, +28, then one of the two parsers below
//   sub_82B472C0    +4, +8, +12, +16, +20, and the byte at +32   (leaf; the rest is its red zone)
//   sub_82B474B8    the same six, plus a 272-byte frame it hands to sub_82B47658 and four
//                   sub_82B47010 calls -- every destination those touch (the four sub-objects at
//                   sp+100/116/132/148 and the shared cursor word at sp+96) is inside that frame,
//                   as their own verified ports (sub_82B47658.inc, sub_82B47010.inc) enumerate.
//
// So the union is [self, self+33): +32 is a single stb and nothing above it is written.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t self = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  if (self == 0) return false;    // the original would store the header fields through null
  if (stream == 0) return false;  // and load the version byte through null; not comparable
  if (static_cast<uint64_t>(self) + kObjectBytes > 0xFFFFFFFFull) return false;
  spec.write(self, kObjectBytes);
  spec.read(self, kObjectBytes);  // +4 and +28 are read back by both record parsers
  spec.read(stream, 2 * kHeaderBytes);
  return true;
}

}  // namespace port_82B470D0
SKATE3_PORT(82B470D0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
