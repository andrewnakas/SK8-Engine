// sub_82B33780  role: decode one packet header for stream `index` through sub_82B470D0 into frame scratch and scatter the seven fields into the object's 48-byte slot and the array's 80-byte entry; on a non-positive count or a null pointer both records are zeroed and the entry is marked 1 instead   lifted: skate3_recomp.67.cpp:52720 (127 lines)
// STATUS: verified
// calls/session: boot 1943  play 2175  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33780.md
namespace port_82B33780 {

// r3 = the object, r4 = the stream index, r5 = the encoded bytes, r6 = their length. Nothing in
// docs/rw_audio_structs.h names either record, so the offsets stay numbers.
constexpr uint32_t kSlotBase = 464;  // u16  where the 48-byte slots start inside the object
constexpr uint32_t kArray = 96;      // rw_ptr  the base of the 80-byte entries

// The 48-byte slot (r30 = object + u16[+464] + 48*index).
constexpr uint32_t kSlotFlag = 28;  // u32  always zeroed
constexpr uint32_t kSlotA = 32;     // u32
constexpr uint32_t kSlotB = 36;     // u32  written, then RELOADED and copied to the entry

// The 80-byte entry (r31 = u32[object+96] + 80*index).
constexpr uint32_t kEntryCopy = 20;       // u32  the reloaded kSlotB, decode arm only
constexpr uint32_t kEntryC = 56;          // u32
constexpr uint32_t kEntryD = 60;          // u32
constexpr uint32_t kEntryE = 64;          // u32
constexpr uint32_t kEntryF = 68;          // u32
constexpr uint32_t kEntryState = 77;      // u8   0 from the decode, 1 on the empty arm

// The 144-byte frame, and sub_82B470D0's output buffer inside it. The callee and its subtree write
// [out+0, out+32]; this function reads out+4, +8, +12, +16, +20, +24 and the byte at out+32.
constexpr uint32_t kFrameBytes = 144;
constexpr uint32_t kOutBuffer = 80;

REX_FUNC(Native) {
  const uint64_t object64 = ctx.r3.u64;
  const uint32_t object = ctx.r3.u32;
  const uint64_t index = ctx.r4.u64;    // mr r11,r4 -- a 64-bit copy
  const uint64_t bytes = ctx.r5.u64;    // mr r4,r5
  const uint64_t length = ctx.r6.u64;   // mr r5,r6, later

  const uint64_t slot_base = REX_LOAD_U16(object + kSlotBase);  // lhz r9,464(r3)
  const uint64_t array = REX_LOAD_U32(object + kArray);         // lwz r10,96(r3)

  // rlwinm r7,r11,1,0,30 / rlwinm r8,r11,2,0,29 -- a 32-bit rotate whose mask clears exactly the
  // bits rotated in from the top, so each is the low word doubled / quadrupled, zero-extended. The
  // two `add`s that follow are 64-BIT on the full index, and only the next rlwinm truncates.
  const uint64_t triple = index + uint64_t(uint32_t(index) * 2u);  // add r7,r11,r7
  const uint64_t quint = index + uint64_t(uint32_t(index) * 4u);   // add r5,r11,r8
  const uint64_t off48 = uint64_t(uint32_t(triple) * 16u);         // rlwinm r8,r7,4,0,27
  const uint64_t off80 = uint64_t(uint32_t(quint) * 16u);          // rlwinm r11,r5,4,0,27
  // add r9,r9,r8 ; add r30,r9,r3 and add r31,r11,r10 -- 64-bit, truncated only by the stores
  const uint32_t slot = uint32_t(slot_base + off48 + object64);
  const uint32_t entry = uint32_t(off80 + array);

  // stwu r1,-144(r1) -- reproduced: r1+80 is the buffer handed to sub_82B470D0, which allocates 128
  // bytes of its own below it and whose subtree spills further still.
  const uint32_t sp = ctx.r1.u32 - kFrameBytes;
  REX_STORE_U32(sp, ctx.r1.u32);  // the back chain; inside this function's own frame
  ctx.r1.u32 = sp;
  const uint32_t out = sp + kOutBuffer;

  // cmpwi cr6,r6,0 ; ble -- SIGNED; then cmplwi cr6,r4,0 ; beq on the byte pointer
  if (ctx.r6.s32 > 0 && ctx.r5.u32 != 0) {
    // mr r5,r6 ; addi r3,r1,80 ; bl 0x82b470d0 -- the whole subtree (82B471D8, 82B474B8, 82B472C0,
    // 82B47658/82B47010) writes only [out+0, out+32] and its own frames, so it needs no windows.
    skate3::audio::GuestCall(ctx, base, sub_82B470D0, ctx.r1.u64 + kOutBuffer, bytes, length);

    const uint32_t at88 = REX_LOAD_U32(out + 8);    // lwz r10,88(r1)
    const uint32_t at92 = REX_LOAD_U32(out + 12);   // lwz r9,92(r1)
    const uint32_t at96 = REX_LOAD_U32(out + 16);   // lwz r8,96(r1)
    const uint32_t at100 = REX_LOAD_U32(out + 20);  // lwz r7,100(r1)
    const uint32_t at84 = REX_LOAD_U32(out + 4);    // lwz r6,84(r1)
    const uint32_t at104 = REX_LOAD_U32(out + 24);  // lwz r5,104(r1)
    const uint32_t at112 = REX_LOAD_U8(out + 32);   // lbz r4,112(r1)

    REX_STORE_U32(slot + kSlotB, at88);              // stw r10,36(r30)
    REX_STORE_U32(slot + kSlotA, at92);              // stw r9,32(r30)
    REX_STORE_U32(entry + kEntryD, at96);            // stw r8,60(r31)
    REX_STORE_U32(entry + kEntryE, at100);           // stw r7,64(r31)
    REX_STORE_U32(entry + kEntryC, at84);            // stw r6,56(r31)
    REX_STORE_U32(entry + kEntryF, at104);           // stw r5,68(r31)
    REX_STORE_U8(entry + kEntryState, uint8_t(at112));  // stb r4,77(r31)
    // lwz r3,36(r30) -- RELOADED, after the five entry stores that could alias it
    const uint32_t published = REX_LOAD_U32(slot + kSlotB);
    REX_STORE_U32(slot + kSlotFlag, 0);              // stw r11,28(r30)
    REX_STORE_U32(entry + kEntryCopy, published);    // stw r3,20(r31)
  } else {
    // loc_82B33830 -- nothing decoded: clear both records and mark the entry 1.
    REX_STORE_U32(slot + kSlotA, 0);        // stw r11,32(r30)
    REX_STORE_U32(entry + kEntryD, 0);      // stw r11,60(r31)
    REX_STORE_U32(entry + kEntryE, 0);      // stw r11,64(r31)
    REX_STORE_U32(entry + kEntryC, 0);      // stw r11,56(r31)
    REX_STORE_U8(entry + kEntryState, 1);   // stb r10,77(r31)
    REX_STORE_U32(slot + kSlotFlag, 0);     // stw r11,28(r30)
    REX_STORE_U32(slot + kSlotB, 0);        // stw r11,36(r30)
  }

  // loc_82B33854
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144 -- a 64-bit add, as in the lifted form
}

// Nine words, on records whose addresses come entirely from the entry registers and two loads taken
// before any store. Both arms are declared as a union: the decode arm writes +20 where the empty arm
// writes +36, and a word a call leaves alone compares equal anyway.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;  // the original would load u16[464] out of the image
  const uint64_t index = ctx.r4.u64;
  const uint64_t slot_base = REX_LOAD_U16(object + kSlotBase);
  const uint64_t array = REX_LOAD_U32(object + kArray);
  const uint64_t triple = index + uint64_t(uint32_t(index) * 2u);
  const uint64_t quint = index + uint64_t(uint32_t(index) * 4u);
  const uint32_t slot = uint32_t(slot_base + uint64_t(uint32_t(triple) * 16u) + ctx.r3.u64);
  const uint32_t entry = uint32_t(uint64_t(uint32_t(quint) * 16u) + array);

  spec.read(object + kSlotBase, 2);
  spec.read(object + kArray, 4);
  spec.read(slot + kSlotFlag, 12);  // +28, +32, +36 -- +36 is re-read after being written
  if (ctx.r6.s32 > 0 && ctx.r5.u32 != 0) {
    // For replay only. The length in r6 reaches the walkers as a TARGET, not a byte count, so the
    // parser reads past it: 12 of 300 recorded calls ran off a length-sized read. A fixed 1 KB summary
    // matches sub_82B470D0's own.
    const uint64_t summary_end = uint64_t(ctx.r5.u32) + 1024;
    spec.read(ctx.r5.u32,
              uint32_t((summary_end > 0x100000000ull ? 0x100000000ull : summary_end) - ctx.r5.u32));
  }

  spec.write(slot + kSlotFlag, 12);      // +28, +32, +36
  spec.write(entry + kEntryCopy, 4);     // +20
  spec.write(entry + kEntryC, 16);       // +56, +60, +64, +68
  spec.write(entry + kEntryState, 1);    // +77
  return true;
}

}  // namespace port_82B33780
SKATE3_PORT(82B33780, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
