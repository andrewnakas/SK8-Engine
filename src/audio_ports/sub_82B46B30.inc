// sub_82B46B30  role: interleave six planar float channels into 24-byte frames, with a channel remap   lifted: skate3_recomp.68.cpp:32717 (324 lines)
// STATUS: verified
// calls/session: boot 47859  play 54900  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B46B30.md
namespace port_82B46B30 {

// The descriptor in r4. Nothing in docs/rw_audio_structs.h names these two fields.
constexpr uint32_t kPlaneBase = 4;     // lwz r10,4(r4)   the planar float buffer
constexpr uint32_t kPlaneFrames = 14;  // lhz r11,14(r4)  floats per plane: the plane stride / 4

// addi r4,r5,1024 -- the copy length is a literal added to the cursor's own register, NOT
// derived from the descriptor: one plane's worth of bytes. 1024 / 4 = 256 frames, so every
// call that gets past the first compare copies exactly 256 frames. That constant is what
// makes the write set enumerable; see the note.
constexpr uint32_t kSpanBytes = 1024;
constexpr uint32_t kFrameBytes = 24;  // six floats out per input frame
constexpr uint32_t kPlanes = 6;

// The six stores of one frame, in the original's order: the source plane, then the byte
// offset of its slot in the output frame. Read off the store offsets, which agree between
// the unrolled loop and the tail: plane 0 -> +0, 1 -> +8, 2 -> +4, 3 -> +16, 4 -> +20,
// 5 -> +12. A channel remap, not a straight interleave.
struct Lane {
  uint32_t plane;
  uint32_t slot;
};
constexpr Lane kLanes[kPlanes] = {{1, 8}, {2, 4}, {4, 20}, {3, 16}, {0, 0}, {5, 12}};

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

// One output frame: six lfs/stfs pairs emitted in the original's order, so a source buffer
// that aliases the destination sees the same interleaving of loads and stores. The
// single->double->single round trip is kept rather than copied as a word, so a signalling
// NaN quietens exactly the way the lifted form's does.
void Frame(uint8_t* base, const uint64_t* plane, uint32_t element, uint32_t out) {
  for (const Lane lane : kLanes) {
    // lfs f,4k(rp)
    const double value =
        LoadSingle(base, static_cast<uint32_t>(plane[lane.plane]) + 4 * element);
    // stfs f,slot(r3)  (in the tail loop: stfs/stfsu f,slot+12(r11), r11 = r3-12)
    StoreSingle(base, out + lane.slot, value);
  }
}

REX_FUNC(Native) {
  const uint32_t desc = ctx.r4.u32;
  const uint64_t frames_per_plane = REX_LOAD_U16(desc + kPlaneFrames);  // lhz r11,14(r4)
  const uint64_t buffer = REX_LOAD_U32(desc + kPlaneBase);              // lwz r10,4(r4)
  // rotlwi r,1/2/3/4 of a zero-extended halfword (<= 0xFFFF, so n<<4 never reaches bit 31 and
  // the rotates are plain shifts), plus two `rlwinm r,2,0,29` of 3n and 5n (both < 2^30, so
  // the 0xFFFFFFFC mask is a no-op), give the six plane pointers as buffer + 4*n*p. Each is a
  // 64-BIT add of two zero-extended words: keep it 64-bit and cast only at the access.
  uint64_t plane[kPlanes];
  for (uint32_t p = 0; p < kPlanes; p++) {
    plane[p] = buffer + 4 * frames_per_plane * p;
  }

  // add r5,r9,r10 -- the loop cursor starts at plane 2's pointer, which is also the register
  // the limit is measured from. addi r4,r5,1024 is that limit.
  const uint64_t start = plane[2];
  const int64_t limit = static_cast<int64_t>(start) + kSpanBytes;
  // cmplw cr6,r5,r4 ; bge cr6,0x82b46cfc -- the compare is on the LOW words, so this fires
  // only when start + 1024 carries out of 32 bits. Nothing at all is written on that path.
  if (static_cast<uint32_t>(start) >= static_cast<uint32_t>(limit)) {
    return;
  }

  // subf r5,r5,r4 ; addi r5,r5,3 ; srawi r5,r5,2 ; addze r5,r5 -- the floats in the span,
  // rounded up. The span is (start+1024)-start in 64 bits, so this is 256 on every call.
  const uint64_t span = static_cast<uint64_t>(limit) - start;
  const int32_t rounded = static_cast<int32_t>(static_cast<uint32_t>(span + 3));
  const int64_t carry = (rounded < 0 && (static_cast<uint32_t>(rounded) & 3) != 0) ? 1 : 0;
  const int64_t elements = static_cast<int64_t>(rounded >> 2) + carry;

  int64_t out = ctx.r3.s64;  // the output cursor; addi r3,r3,96 per unrolled iteration
  uint32_t element = 0;
  // cmpwi cr6,r5,4 ; blt cr6,0x82b46c90 -- fewer than four frames skips the unrolled loop.
  if (static_cast<int32_t>(static_cast<uint32_t>(elements)) >= 4) {
    // subf r5,r11,r4 ; addi r5,r5,-13 ; rlwinm r5,r5,28,4,31 ; addi r5,r5,1 -- (span-13)/16 + 1
    // iterations of four frames each; 64 for a 1024-byte span, which consumes it exactly.
    const int64_t adjusted = static_cast<int64_t>(span) - 13;
    const uint64_t unrolled_trips = (static_cast<uint32_t>(adjusted) >> 4) + 1;
    for (uint64_t t = 0; t < unrolled_trips; t++) {
      ctx.fpscr.disableFlushModeUnconditional();  // at the top of loc_82B46BB0, once per trip
      for (uint32_t k = 0; k < 4; k++) {
        Frame(base, plane, element + k, static_cast<uint32_t>(out) + kFrameBytes * k);
      }
      element += 4;               // addi r9/r11/r8/r7/r10/r6,+16 on all six plane pointers
      out += 4 * kFrameBytes;     // addi r3,r3,96
    }
  }
  // r3 keeps the cursor the unrolled loop left; the tail loop reads it (addi r11,r3,-12) and
  // never advances it, so this is the value the original returns in r3 on every path.
  ctx.r3.s64 = out;

  // cmplw cr6,r11,r4 ; bge cr6,0x82b46cfc -- r11 is plane 2's cursor, start + 4*element.
  const uint64_t cursor = start + 4 * element;
  if (static_cast<uint32_t>(cursor) >= static_cast<uint32_t>(limit)) {
    return;
  }

  // subf r5,r11,r4 ; addi r6,r5,-1 ; rlwinm r10,r6,30,2,31 ; addi r31,r10,1 -- one frame per
  // trip over the remainder. Unreachable for a 1024-byte span (the unrolled loop consumes it
  // exactly); written so the body stays faithful if that ever stops being true. The original
  // holds this trip count in r31, which is why it spills and reloads it at -8(r1); this body
  // never touches r31.
  const int64_t left = static_cast<int64_t>(static_cast<uint64_t>(limit) - cursor) - 1;
  const uint64_t tail_trips = (static_cast<uint32_t>(left) >> 2) + 1;
  uint32_t tail = static_cast<uint32_t>(out);  // addi r11,r3,-12, then +24 per stfsu
  for (uint64_t t = 0; t < tail_trips; t++) {
    ctx.fpscr.disableFlushModeUnconditional();  // at the top of loc_82B46CC8, once per trip
    Frame(base, plane, element, tail);
    element++;
    tail += kFrameBytes;
  }
}

// One contiguous block at r3. Both loops write whole 24-byte frames with no gaps: the
// unrolled one writes r3+0..95 then advances 96, the tail writes 24 and advances 24. The
// frame count is fixed by the literal 1024, so it does not depend on anything the call
// itself writes -- that is the whole gate-2 argument, and it is why 30 flagged stores
// collapse to a single span.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t desc = ctx.r4.u32;
  const uint64_t frames_per_plane = REX_LOAD_U16(desc + kPlaneFrames);
  const uint64_t buffer = REX_LOAD_U32(desc + kPlaneBase);
  spec.read(desc + kPlaneBase, 12);  // +4 the buffer, +14 the plane stride

  const uint64_t start = buffer + 8 * frames_per_plane;  // plane 2 == the loop cursor
  const int64_t limit = static_cast<int64_t>(start) + kSpanBytes;
  if (static_cast<uint32_t>(start) >= static_cast<uint32_t>(limit)) {
    return true;  // the low-word compare early-outs: no writes on that path
  }
  const uint32_t elements = kSpanBytes / 4;  // 256, always
  spec.write(ctx.r3.u32, kFrameBytes * elements);
  for (uint32_t p = 0; p < kPlanes; p++) {
    spec.read(static_cast<uint32_t>(buffer + 4 * frames_per_plane * p), 4 * elements);
  }
  return true;
}

}  // namespace port_82B46B30
SKATE3_PORT(82B46B30, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
