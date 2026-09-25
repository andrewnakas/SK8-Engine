// sub_82B3DF90  role: fill the tail of one or two float output buffers with a constant   lifted: skate3_recomp.68.cpp:11292 (242 lines)
// STATUS: verified
// calls/session: boot 533208  play 759184  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3DF90.md
namespace port_82B3DF90 {

// lis r6,-32234 ; lfs f0,23056(r6): a single-precision constant in the image's read-only data.
// Nothing documents it; the body loads it rather than assuming its value.
constexpr uint32_t kFillConstant = 0x82165A10u;

// Object fields read (r3). Nothing in docs/rw_audio_structs.h names this structure.
constexpr uint32_t kBlock = 16;    // added to 127 / 255 to cap the fill length
constexpr uint32_t kEnd = 20;      // compared against the cursor; nothing to do when cursor >= end
constexpr uint32_t kCursor = 36;
constexpr uint32_t kLimit0 = 40;   // fill length of buffer 0 counts up to here
constexpr uint32_t kLimit1 = 44;   // fill length of buffer 1 counts up to here

// The buffer record (r5): two float pointers at +4 and +8. The second may be null.
constexpr uint32_t kBuffer0 = 4;
constexpr uint32_t kBuffer1 = 8;

// subfic r10,rN,0 ; rlwinm r9,rN,1,31,31 ; addme r10,r9 ; and r8,r10,rN -- the branch-free
// max(n, 0): r10 is all-ones only when n > 0 (no borrow and sign bit clear), so the AND keeps
// n exactly then and yields 0 otherwise. Only the low 32 bits are consumed afterwards.
inline uint32_t ClampNonNegative(int32_t n) { return n > 0 ? static_cast<uint32_t>(n) : 0u; }

// stfs/stfsx f0: narrow the loaded double back to single, as the lifted form does per store.
inline void StoreF0(PPCContext& ctx, uint8_t* base, uint32_t ea) {
  PPCRegister temp{};
  temp.f32 = float(ctx.f0.f64);
  REX_STORE_U32(ea, temp.u32);
}

// The fill: `count` floats of f0 through the pointer at `slot`, four per iteration then a tail.
// The pointer is reloaded before every store, as the original does (lwz r9,4(r5) / lwz r9,8(r5)
// between every stfs), so a store that aliases the slot behaves identically.
inline void Fill(PPCContext& ctx, uint8_t* base, uint32_t slot, uint32_t count) {
  uint32_t done = 0;  // li r7,0
  if (static_cast<int32_t>(count) >= 4) {  // cmpwi cr6,r8,4 ; blt cr6
    // addi r10,r8,-4 ; rlwinm r10,r10,30,2,31 ; addi r10,r10,1 -> quads; rlwinm r7,r10,2,0,29
    uint32_t quads = ((count - 4u) >> 2) + 1u;
    done = (quads << 2) & 0xFFFFFFFCu;
    uint32_t off = 0;  // li r11,0
    do {  // mtctr r10 ... bdnz
      ctx.fpscr.disableFlushModeUnconditional();
      StoreF0(ctx, base, REX_LOAD_U32(slot) + off);                // lwz r9,N(r5) ; stfsx f0,r11,r9
      StoreF0(ctx, base, (REX_LOAD_U32(slot) + off) + 4u);         // lwz r9,N(r5) ; add r6 ; stfs f0,4(r6)
      StoreF0(ctx, base, (REX_LOAD_U32(slot) + off + 12u) - 4u);   // lwz r9,N(r5) ; add ; stfs f0,-4(rX)
      StoreF0(ctx, base, REX_LOAD_U32(slot) + off + 12u);          // lwz rY,N(r5) ; stfsx f0,r10,rY
      off += 16u;  // addi r11,r11,16
    } while (--quads != 0);  // bdnz
  }
  if (static_cast<int32_t>(done) < static_cast<int32_t>(count)) {  // cmpw cr6,r7,r8 ; bge cr6
    uint32_t remaining = count - done;            // subf r10,r7,r8 ; mtctr r10
    uint32_t off = (done << 2) & 0xFFFFFFFCu;     // rlwinm r11,r7,2,0,29
    do {
      ctx.fpscr.disableFlushModeUnconditional();
      StoreF0(ctx, base, REX_LOAD_U32(slot) + off);  // lwz r10,N(r5) ; stfsx f0,r11,r10
      off += 4u;  // addi r11,r11,4
    } while (--remaining != 0);  // bdnz
  }
}

// How many floats buffer 0 gets, from the object's fields and r4 (32-bit wrap arithmetic).
inline uint32_t Count0(uint32_t limit0, uint32_t cursor, uint32_t block, uint32_t consumed) {
  int32_t n = static_cast<int32_t>((limit0 - cursor) - consumed);  // subf r8,r11,r9 ; subf r11,r4,r8
  const int32_t cap = static_cast<int32_t>(block + 255u);          // addi r10,r10,255
  if (!(n < cap)) n = cap;  // cmpw cr6,r11,r10 ; blt cr6 ; mr r11,r10
  return ClampNonNegative(n);
}

// How many floats buffer 1 gets: capped at block+127, then (redundantly, as the original does)
// at block+255.
inline uint32_t Count1(uint32_t limit1, uint32_t cursor, uint32_t block, uint32_t consumed) {
  int32_t n = static_cast<int32_t>((limit1 - consumed) - cursor);  // subf r8,r4,r11 ; subf r11,r10,r8
  int32_t cap = static_cast<int32_t>(block + 127u);                // addi r10,r9,127
  if (!(n < cap)) n = cap;  // cmpw cr6,r11,r10 ; blt cr6 ; mr r11,r10
  cap = static_cast<int32_t>(block + 255u);                        // addi r10,r9,255
  if (!(n < cap)) n = cap;  // cmpw cr6,r11,r10 ; blt cr6 ; mr r11,r10
  return ClampNonNegative(n);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t consumed = ctx.r4.u32;
  const uint32_t buffers = ctx.r5.u32;

  const uint32_t end = REX_LOAD_U32(object + kEnd);        // lwz r10,20(r3)
  const uint32_t cursor = REX_LOAD_U32(object + kCursor);  // lwz r11,36(r3)
  if (static_cast<int32_t>(cursor) >= static_cast<int32_t>(end)) return;  // cmpw cr6,r11,r10 ; bgelr cr6

  const uint32_t limit0 = REX_LOAD_U32(object + kLimit0);  // lwz r9,40(r3)
  const uint32_t block = REX_LOAD_U32(object + kBlock);    // lwz r10,16(r3)
  const uint32_t count0 = Count0(limit0, cursor, block, consumed);

  // lfs f0,23056(r6): loaded once, before either loop, whatever count0 is.
  ctx.fpscr.disableFlushModeUnconditional();
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(kFillConstant);
  ctx.f0.f64 = double(temp.f32);

  Fill(ctx, base, buffers + kBuffer0, count0);

  if (REX_LOAD_U32(buffers + kBuffer1) == 0) return;  // lwz r11,8(r5) ; cmplwi cr6,r11,0 ; beqlr cr6

  // Reloaded after the first fill, as the original does (lwz r11,44 ; lwz r10,36 ; lwz r9,16).
  const uint32_t limit1 = REX_LOAD_U32(object + kLimit1);
  const uint32_t cursor1 = REX_LOAD_U32(object + kCursor);
  const uint32_t block1 = REX_LOAD_U32(object + kBlock);
  const uint32_t count1 = Count1(limit1, cursor1, block1, consumed);

  Fill(ctx, base, buffers + kBuffer1, count1);
}

// Writes: {buffer0, 4*count0} and, when the second pointer is non-null, {buffer1, 4*count1}.
// Both counts derive from entry state: object +16/+20/+36/+40/+44, r4 and the two pointers.
// The original reloads the pointer slot before every store and the object fields between the
// two fills, so a fill that overlaps the record or the object would change the write set
// mid-call; those cases are not enumerable and return false.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  const uint32_t consumed = ctx.r4.u32;
  const uint32_t buffers = ctx.r5.u32;

  spec.read(object + kBlock, 32);  // +16 .. +47: block, end, (gap), cursor, limit0, limit1
  spec.read(buffers + kBuffer0, 8);
  spec.read(kFillConstant, 4);

  const uint32_t end = REX_LOAD_U32(object + kEnd);
  const uint32_t cursor = REX_LOAD_U32(object + kCursor);
  if (static_cast<int32_t>(cursor) >= static_cast<int32_t>(end)) return true;  // bgelr: no stores

  const uint32_t limit0 = REX_LOAD_U32(object + kLimit0);
  const uint32_t limit1 = REX_LOAD_U32(object + kLimit1);
  const uint32_t block = REX_LOAD_U32(object + kBlock);
  const uint32_t buffer0 = REX_LOAD_U32(buffers + kBuffer0);
  const uint32_t buffer1 = REX_LOAD_U32(buffers + kBuffer1);

  const uint64_t len0 = 4ull * Count0(limit0, cursor, block, consumed);
  const uint64_t len1 = buffer1 != 0 ? 4ull * Count1(limit1, cursor, block, consumed) : 0ull;
  if (len0 + len1 > skate3::audio::kPortWatchCap) return false;  // over budget (and u32 length)

  // A fill through a null pointer is a store at guest address 0..; not comparable.
  if (len0 != 0 && buffer0 == 0) return false;

  // The reloaded slots must not be inside a window, or the pointer changes under the loop.
  auto overlaps = [](uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
    return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
  };
  const uint64_t record = static_cast<uint64_t>(buffers) + kBuffer0;  // +4 .. +11
  const uint64_t fields = static_cast<uint64_t>(object) + kBlock;     // +16 .. +47
  if (overlaps(buffer0, len0, record, 8) || overlaps(buffer0, len0, fields, 32)) return false;
  if (overlaps(buffer1, len1, record, 8)) return false;

  spec.write(buffer0, static_cast<uint32_t>(len0));
  spec.write(buffer1, static_cast<uint32_t>(len1));
  return true;
}

}  // namespace port_82B3DF90
SKATE3_PORT(82B3DF90, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
