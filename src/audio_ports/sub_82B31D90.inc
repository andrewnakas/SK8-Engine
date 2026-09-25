// sub_82B31D90  role: unpack a 64-bit packed stream header into a four-field description   lifted: skate3_recomp.67.cpp:48948 (93 lines)
// STATUS: verified
// calls/session: boot 745  play 981  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B31D90.md
namespace port_82B31D90 {

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)
constexpr uint32_t kReader = 80;       // addi r3,r1,80 -- the two-word bit reader lives here

// port_82B26B70's reader: {byte stream* +0, absolute bit cursor +4}.
constexpr uint32_t kReaderData = 0;
constexpr uint32_t kReaderCursor = 4;

// The byte the header may be prefixed with. cmplwi cr6,r11,72 -- 72 is 'H'; when it is there the
// first four bytes are skipped and the bit reader starts at +4.
constexpr uint32_t kSkippedTag = 72;
constexpr uint32_t kSkippedBytes = 4;

// The output record in r4. Nothing in docs/rw_audio_structs.h names this layout, so the offsets
// stay numbers; the comment is the field's bit width, which is all the code tells us.
constexpr uint32_t kCountPlusOne = 0;  // u8   6 bits + 1
constexpr uint32_t kField4 = 4;        // u32  18 bits
constexpr uint32_t kField8 = 8;        // u32  29 bits
constexpr uint32_t kField12 = 12;      // u32  4 bits, the SECOND nibble read

// The six fields in the order the original reads them; 4 + 4 + 6 + 18 + 3 + 29 = 64 bits.
constexpr uint32_t kHeaderBytes = 8;

REX_FUNC(Native) {
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t out = ctx.r4.u32;  // mr r31,r4
  // std r31,-16(r1) / ld r31,-16(r1) -- r31 is spilled because sub_82B26B70 clobbers it, and the
  // harness compares r13-r31. A local does the same job.
  const PPCRegister saved_r31 = ctx.r31;

  // lbz r11,0(r3) ; cmplwi cr6,r11,72 ; bne cr6,0x82b31db4 ; addi r3,r3,4
  uint32_t stream = ctx.r3.u32;
  if (REX_LOAD_U8(stream) == kSkippedTag) stream += kSkippedBytes;

  // stwu r1,-112(r1). REPRODUCED: sp+80 is handed to sub_82B26B70 as a guest pointer it both
  // reads and writes. Skip the move and the reader would land 112 bytes higher, in the caller's
  // frame, outside every declared window.
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // addi r3,r1,80, recomputed before each of the six calls in the original; r1 does not move
  // across them, so one local is the same address every time.
  const uint32_t reader = ctx.r1.u32 + kReader;
  REX_STORE_U32(reader + kReaderData, stream);  // stw r3,80(r1)
  REX_STORE_U32(reader + kReaderCursor, 0);     // li r11,0 ; stw r11,84(r1)

  // bl 0x82b26b70 -- the bit reader, itself a verified port, so it goes through GuestCall on the
  // live context and each call is compared by its own hook. Its only store is reader+4.
  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, 4);  // li r4,4 -- result discarded

  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, 4);  // li r4,4
  REX_STORE_U32(out + kField12, ctx.r3.u32);                     // stw r3,12(r31)

  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, 6);  // li r4,6
  // addi r10,r3,1 ; stb r10,0(r31) -- 64-bit add, low byte stored; the 6-bit field is 0..63.
  REX_STORE_U8(out + kCountPlusOne, static_cast<uint8_t>(ctx.r3.u64 + 1));

  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, 18);  // li r4,18
  REX_STORE_U32(out + kField4, ctx.r3.u32);                       // stw r3,4(r31)

  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, 3);   // li r4,3 -- result discarded

  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, 29);  // li r4,29
  REX_STORE_U32(out + kField8, ctx.r3.u32);                       // stw r3,8(r31)

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112, on the full 64-bit register
  ctx.r31 = saved_r31;
}

// Writes: the four output fields, and nothing else. sub_82B26B70's single store is the bit cursor
// at reader+4, which is this function's own frame scratch (sp+84) and so is neither windowed nor
// reproduced; the reader's byte stream is read-only throughout.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t stream = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  if (stream == 0) return false;  // the original would load the tag byte through null
  if (out == 0) return false;     // and store the four fields through null; not comparable
  spec.write(out + kCountPlusOne, 1);  // stb r10,0(r31)
  spec.write(out + kField4, 12);       // stw +4, stw +8, stw +12
  // The tag test and therefore the reader's start are both entry state.
  const uint32_t body =
      (REX_LOAD_U8(stream) == kSkippedTag) ? stream + kSkippedBytes : stream;
  spec.read(stream, kSkippedBytes);
  spec.read(body, kHeaderBytes);
  return true;
}

}  // namespace port_82B31D90
SKATE3_PORT(82B31D90, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
