// sub_82B3D4F8  role: mix dispatcher -- unpack a six-pointer buffer descriptor and call the one-gain kernel sub_82B3CF58 when the descriptor's word at +8 is zero, otherwise the two-gain kernel sub_82B3D0A8 with the sixth pointer passed on the stack   lifted: skate3_recomp.68.cpp:9763 (76 lines)
// STATUS: verified
// calls/session: boot 136586  play 225914  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3D4F8.md
namespace port_82B3D4F8 {

// Arguments: r3 = the gain block, r4 = the sample count, r7 = the buffer descriptor.
// r5, r6 and r8-r10 are not read on entry.
//
// The gain block: +16 and +20 are singles. The same object is read at +16..+32 by
// sub_82B39FA0, so this is a gain vector and only its first two slots are used here.
constexpr uint32_t kGain0 = 16;  // lfs f1,16(r6) -- both paths
constexpr uint32_t kGain1 = 20;  // lfs f2,20(r6) -- the two-gain path only
//
// The descriptor: six pointer-sized words plus the selector at +8. Nothing in
// docs/rw_audio_structs.h names it, so the names describe how each word is used.
constexpr uint32_t kDescAddend = 0;    // -> sub_82B3CF58's y (read only)
constexpr uint32_t kDescSource = 4;    // -> sub_82B3CF58's x (read only; its alignment picks
                                       //    the kernel's vector or scalar path)
constexpr uint32_t kDescSelect = 8;    // lwz r8,8(r7) -- zero picks the one-gain kernel. On the
                                       //    other path it is a POINTER: sub_82B3D0A8 alignment-
                                       //    tests it and reads it as the second source array, so
                                       //    "no second source" is what zero here means.
constexpr uint32_t kDescExtra = 12;    // the two-gain path only: sub_82B3D0A8's per-sample
                                       //    crossfade coefficient array (read only)
constexpr uint32_t kDescMixed = 16;    // -> sub_82B3CF58's z (written: x*gain + y)
constexpr uint32_t kDescCopy = 20;     // -> sub_82B3CF58's w (written: x re-read after z)
constexpr uint32_t kDescBytes = 24;

constexpr uint32_t kFrameBytes = 96;       // stwu r1,-96(r1)
constexpr uint32_t kLinkSlot = 8;          // stw r12,-8(r1), i.e. frame + 88
constexpr uint32_t kStackArgSlot = 84;     // stw r5,84(r1) -- sub_82B3D0A8's lwz r30,84(r1)

// The same overflow guard sub_82B3CF58's port uses on its own window arithmetic. The harness's
// 32 KB budget rejects anything past about 4,000 samples long before this.
constexpr int32_t kMaxEnumerableSamples = 0x01000000;

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

REX_FUNC(Native) {
  const uint32_t gains = ctx.r3.u32;    // mr r6,r3
  // mr r3,r4 -- the kernels' sample count. r4 itself is never reassigned, so r3 and r4 both
  // hold it at either call; neither kernel reads r4 on entry, and it is passed below only so
  // the register state the callee sees matches the original exactly.
  const uint64_t count = ctx.r4.u64;
  const uint32_t desc = ctx.r7.u32;     // mr r11,r7

  // The frame is reproduced because it carries an outgoing argument: sub_82B3D0A8 reads
  // 84(r1) off it. The link-register slot is at frame + 88 and the back chain at frame + 0;
  // both are inside the frame this call allocates, so neither is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                                    // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);        // stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;           // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t select = REX_LOAD_U32(desc + kDescSelect);  // lwz r8,8(r7)
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.f1.f64 = LoadSingle(base, gains + kGain0);             // lfs f1,16(r6)

  // cmplwi cr6,r8,0 ; bne cr6,0x82b3d544
  if (select == 0) {
    const uint32_t copy = REX_LOAD_U32(desc + kDescCopy);      // lwz r8,20(r7)
    const uint32_t mixed = REX_LOAD_U32(desc + kDescMixed);    // lwz r7,16(r7)
    const uint32_t source = REX_LOAD_U32(desc + kDescSource);  // lwz r6,4(r11)
    const uint32_t addend = REX_LOAD_U32(desc + kDescAddend);  // lwz r5,0(r11)
    // bl 0x82b3cf58 -- z = x*f1 + y and w = x, n samples. Through GuestCall on the live
    // context so the callee's own hook decides how it runs.
    skate3::audio::GuestCall(ctx, base, sub_82B3CF58, count, count, addend, source, mixed, copy);
  } else {
    // loc_82B3D544
    const uint32_t copy = REX_LOAD_U32(desc + kDescCopy);      // lwz r5,20(r11)
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f2.f64 = LoadSingle(base, gains + kGain1);             // lfs f2,20(r6)
    const uint32_t mixed = REX_LOAD_U32(desc + kDescMixed);    // lwz r10,16(r11)
    const uint32_t extra = REX_LOAD_U32(desc + kDescExtra);    // lwz r9,12(r11)
    const uint32_t source = REX_LOAD_U32(desc + kDescSource);  // lwz r7,4(r11)
    const uint32_t addend = REX_LOAD_U32(desc + kDescAddend);  // lwz r6,0(r11)
    // stw r5,84(r1) -- the sixth pointer as an outgoing stack argument. Inside this call's own
    // frame, so it is not a window; the value is the same one the original stores.
    REX_STORE_U32(ctx.r1.u32 + kStackArgSlot, copy);
    // bl 0x82b3d0a8 -- eight register arguments: the selector word stays live in r8 and the
    // sixth pointer is passed in r5 as well as on the stack, exactly as lifted.
    skate3::audio::GuestCall(ctx, base, sub_82B3D0A8, count, count, copy, addend, source,
                             static_cast<uint64_t>(select), extra, mixed);
  }

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;                         // addi r1,r1,96
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);            // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                          // mtlr r12
}

// Every byte the call writes, from ENTRY state only -- on BOTH paths, because the two kernels
// write the same two buffers.
//
// The two-gain path was refused until 2026-09-12 on the grounds that sub_82B3D0A8's write set is
// not derivable from here. It is. Its six arrays arrive entirely in registers this function
// loads out of the descriptor before the call (plus the 84(r1) slot it stores itself), and every
// store address in its 662 lifted lines resolves, in 32-bit wrapping arithmetic, to one of just
// two of them: `desc[16]` and `desc[20]`. Checked twice over -- a flow-sensitive pass over every
// store in the lifted body, and the store table in probe/ports/notes/sub_82B3D0A8.md, which was
// derived independently and agrees. The "cursors and inter-buffer deltas built inside its loops"
// are real but they cancel: each `stvx128 vX,rA,rB` pairs a cursor into one array with the
// precomputed difference to another, e.g. `stvx128 v39,r27,r11` is (desc[20] - desc[4]) +
// (desc[4] + 32 + 64k) = desc[20] + 32 + 64k.
//
// Its geometry is also the one-gain kernel's: 64 bytes per 16-sample block into each output from
// the 16-byte floor of its base (the stvx128 masking), then four bytes per sample for the
// remainder at the exact base. Only the vector gate differs -- sub_82B3CF58 needs its source
// 16-byte aligned, sub_82B3D0A8 needs `desc[4]` AND `desc[8]` aligned -- and a failed gate only
// moves samples from the vector spans into the scalar ones. So one enumeration covers both, with
// the gate below the only branch.
//
// What this does NOT lean on is sub_82B3D0A8's own port being green: its note says it has never
// been compared. It leans on the lifted body.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t desc = ctx.r7.u32;
  spec.read(desc, kDescBytes);
  spec.read(ctx.r3.u32 + kGain0, 8);

  const int32_t count = ctx.r4.s32;  // the callee's r3
  if (count <= 0) return true;       // both kernels' `bge` leaves before any store outside a frame
  if (count > kMaxEnumerableSamples) return false;

  const uint32_t addend = REX_LOAD_U32(desc + kDescAddend);
  const uint32_t source = REX_LOAD_U32(desc + kDescSource);
  const uint32_t select = REX_LOAD_U32(desc + kDescSelect);
  const uint32_t extra = REX_LOAD_U32(desc + kDescExtra);
  const uint32_t mixed = REX_LOAD_U32(desc + kDescMixed);
  const uint32_t copy = REX_LOAD_U32(desc + kDescCopy);
  const bool two_gain = select != 0;

  // clrlwi r11,r6,28 in sub_82B3CF58; clrlwi r11,r7,28 then clrlwi r11,r8,28 in sub_82B3D0A8 --
  // and on that path `select` is itself the third input array, which is why it is the operand of
  // an alignment test rather than a mode word.
  const bool vector = ((source & 0xFu) == 0) && (!two_gain || (select & 0xFu) == 0);
  const uint32_t blocks = vector ? static_cast<uint32_t>(count / 16) : 0;
  if (blocks != 0) {
    const uint32_t bytes = blocks * 64;
    spec.write(mixed & ~0xFu, bytes);
    spec.write(copy & ~0xFu, bytes);
    spec.read(source & ~0xFu, bytes);
    spec.read(addend & ~0xFu, bytes);
    if (two_gain) {
      spec.read(select & ~0xFu, bytes);  // the second source
      spec.read(extra & ~0xFu, bytes);   // the per-sample crossfade coefficient
    }
  }
  const uint32_t done = blocks * 16;
  if (done < static_cast<uint32_t>(count)) {
    const uint32_t offset = done * 4;
    const uint32_t bytes = (static_cast<uint32_t>(count) - done) * 4;
    spec.write(mixed + offset, bytes);
    spec.write(copy + offset, bytes);
    spec.read(source + offset, bytes);
    spec.read(addend + offset, bytes);
    if (two_gain) {
      spec.read(select + offset, bytes);
      spec.read(extra + offset, bytes);
    }
  }
  return true;
}

}  // namespace port_82B3D4F8
SKATE3_PORT(82B3D4F8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
