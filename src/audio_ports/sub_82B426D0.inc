// sub_82B426D0  role: scatter-mix a routing table -- for each route byte scale one source buffer into one destination buffer, accumulating into destinations already written, then zero every destination no route touched   lifted: skate3_recomp.68.cpp:22179 (155 lines)
// STATUS: verified
// calls/session: boot 62521  play 83885  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B426D0.md
namespace port_82B426D0 {

constexpr uint32_t kFrameBytes = 176;  // stwu r1,-176(r1)
constexpr uint32_t kFlagBytes = 8;     // li r11,8 ; mtctr ; stbu r9,1(r10) from r1+79

// lis r11,-32241 ; addi r24,r11,-10560 -- ((-32241 & 0xFFFF) << 16) + (-10560).
constexpr uint32_t kGainTable = 0x820ED6C0;  // four floats, selected by route bits 6-7
constexpr uint32_t kGainTableBytes = 16;

// The route byte, as the three rlwinm fields decode it. All three identities were checked over
// every byte value rather than read off the rotate-and-mask by eye.
constexpr uint32_t kRouteDestMask = 7;  // clrlwi r9,r11,29 -- destination slot, 0..7

// The route range in r7: an inclusive [first, last] pair of byte indices into the route table.
constexpr uint32_t kRangeFirst = 0;  // lbz r31,0(r25)
constexpr uint32_t kRangeLast = 1;   // lbz r11,1(r25), RELOADED every iteration

// rlwinm r8,r11,28,4,29 -- ((route >> 4) & 0xC): a word index 0..3 into kGainTable, times four.
inline uint32_t GainSlot(uint32_t route) { return (route >> 4) & 0xC; }
// rlwinm r10,r11,31,27,29 -- ((route >> 1) & 0x1C): source slot 0..7, times four.
inline uint32_t SourceSlot(uint32_t route) { return (route >> 1) & 0x1C; }
// rlwinm r9,r11,2,27,29 -- ((route & 7) << 2): destination slot 0..7, times four.
inline uint32_t DestSlot(uint32_t route) { return (route & kRouteDestMask) << 2; }

// lfsx: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// The byte span either scale routine writes at [dst, dst + len). This MIRRORS
// recomp/src/audio_ports/sub_82B3BED8.inc and recomp/src/audio_ports/sub_82B44B20.inc: a change to
// either port's Windows() requires a change here. The two differ only in how the vector path
// derives the span (srawi-then-shift there, one rlwinm here), which matters solely for a negative
// count, so both are computed and the longer declared -- this function cannot tell from entry
// state which of the two a given route will call without also trusting the flag simulation below.
inline bool ScaleSpan(uint32_t dst, uint32_t src, uint32_t count, uint32_t& len) {
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    const uint32_t by_shift = (count << 2) & 0xFFFFFFF0u;  // sub_82B44B20
    const uint32_t by_srawi =
        (static_cast<uint32_t>(static_cast<int32_t>(count) >> 2) << 4) & 0xFFFFFFF0u;  // sub_82B3BED8
    // Either vector loop exits only after whole 128-byte steps land exactly on the end pointer; a
    // zero or non-multiple span never terminates and the original hangs there.
    if (by_shift == 0 || (by_shift & 127u) != 0) return false;
    if (by_srawi == 0 || (by_srawi & 127u) != 0) return false;
    len = by_shift > by_srawi ? by_shift : by_srawi;
  } else {
    len = (count << 2) & 0xFFFFFFFCu;
    if (dst + len <= dst) len = 0;  // dst >= dst_end: the call writes nothing at all
  }
  return static_cast<uint64_t>(dst) + len <= 0xFFFFFFFFull;
}

REX_FUNC(Native) {
  const uint32_t dest_array = ctx.r3.u32;   // mr r29,r3 -- eight destination float* slots
  const uint32_t source_array = ctx.r4.u32; // mr r28,r4 -- eight source float* slots
  const uint32_t slot_count = ctx.r5.u32;   // mr r23,r5 -- destination slots the caller owns
  const uint32_t count = ctx.r6.u32;        // mr r27,r6 -- floats per buffer
  const uint32_t range = ctx.r7.u32;        // mr r25,r7
  const uint32_t table = ctx.r8.u32;        // mr r30,r8 -- the route bytes

  // li r11,8 ; addi r10,r1,79 ; stbu r9,1(r10) x8 -- eight "this destination has been written"
  // flags at sp+80..87. They are this function's own frame: no callee is given their address, so a
  // local array is the same machine. Windows() refuses slot_count > 8, which is the only way the
  // original could read a flag byte this array does not have.
  uint8_t written[kFlagBytes] = {0, 0, 0, 0, 0, 0, 0, 0};

  // lbz r31,0(r25) ; lbz r11,1(r25) ; cmplw ; bgt cr6,0x82b42788
  uint32_t index = REX_LOAD_U8(range + kRangeFirst);
  if (index <= REX_LOAD_U8(range + kRangeLast)) {
    do {  // loc_82B42728
      const uint32_t route = REX_LOAD_U8(table + index);     // lbzx r11,r31,r30
      const uint32_t dest_slot = route & kRouteDestMask;     // clrlwi r9,r11,29
      const uint32_t gain_slot = GainSlot(route);            // rlwinm r8,r11,28,4,29
      const uint32_t flag = written[dest_slot];              // lbzx r7,r9,r10 (r10 = sp+80)
      const uint32_t source_slot = SourceSlot(route);        // rlwinm r10,r11,31,27,29
      const uint32_t dest_index = DestSlot(route);           // rlwinm r9,r11,2,27,29

      ctx.fpscr.disableFlushModeUnconditional();             // emitted by the lfsx
      const double gain = LoadSingle(base, kGainTable + gain_slot);  // lfsx f1,r8,r24
      const uint32_t src = REX_LOAD_U32(source_array + source_slot); // lwzx r4,r10,r28
      const uint32_t dst = REX_LOAD_U32(dest_array + dest_index);    // lwzx r3,r9,r29

      // The original sets r3, r4, r6 and f1 and leaves r5 alone, so the registers are written
      // by hand rather than through GuestCall's argument list -- neither callee reads r5, and
      // both see exactly the entry state the original hands them.
      ctx.r3.u64 = dst;
      ctx.r4.u64 = src;
      ctx.r6.u64 = count;   // mr r6,r27
      ctx.f1.f64 = gain;
      if (flag != 0) {
        // bl 0x82b44b20 -- dst[i] += src[i] * gain, a verified port, so GuestCall on the live
        // context keeps it behind its own hook.
        skate3::audio::GuestCall(ctx, base, sub_82B44B20);
      } else {
        // loc_82B42764: bl 0x82b3bed8 -- dst[i] = src[i] * gain, likewise a verified port.
        skate3::audio::GuestCall(ctx, base, sub_82B3BED8);
        // lbzx r7,r31,r30 -- the route byte is RELOADED after the call rather than kept in a
        // register, so a table that aliased a destination buffer would see the new value. Windows()
        // refuses that case, which is what makes the reload equal to `route` here.
        const uint32_t reloaded = REX_LOAD_U8(table + index);
        written[reloaded & kRouteDestMask] = 1;  // li r26,1 ; stbx r26,r6,r8
      }

      index++;  // addi r31,r31,1
      // loc_82B42778: lbz r11,1(r25) ; cmplw cr6,r31,r11 ; ble -- the last index is reloaded from
      // memory every iteration.
    } while (index <= REX_LOAD_U8(range + kRangeLast));
  }

  // loc_82B42788: li r31,0 ; cmplwi cr6,r23,0 ; beq cr6,0x82b427cc
  if (slot_count != 0) {
    uint32_t slot = dest_array;  // mr r30,r29
    uint32_t i = 0;
    do {  // loc_82B4279C
      // lbzx r11,r31,r11 with r11 = sp+80, recomputed at the bottom of every iteration.
      if (written[i] == 0) {  // cmplwi cr6,r11,0 ; bne cr6,0x82b427b8
        // rlwinm r5,r27,2,0,29 ; lwz r3,0(r30) ; li r4,0 ; bl 0x82ee5e80 -- memset.
        skate3::audio::GuestCall(ctx, base, sub_82EE5E80, REX_LOAD_U32(slot), uint64_t(0),
                                 uint64_t((count << 2) & 0xFFFFFFFCu));
      }
      i++;                  // addi r31,r31,1
      slot += 4;            // addi r30,r30,4
      // cmplw cr6,r31,r23 ; blt
    } while (i < slot_count);
  }
  // The frame move is NOT reproduced: sp+80 never leaves this function, so nothing needs the
  // guest stack to have moved, and r23-r31 are never written here, so __savegprlr_23's spills and
  // __restgprlr_23's reloads have nothing to restore. Both callee groups preserve r13-r31.
}

// Every byte the call writes, from entry state. The two scale routines write [r3, r3 + count*4);
// sub_82EE5E80 (memset) writes exactly [dst, dst + n). The eight flag bytes are frame scratch.
//
// The write set is the union over every route of its destination buffer, plus every destination
// slot the closing memset loop can reach -- declared for ALL slot_count slots rather than only the
// flag-zero ones, so a mis-simulated flag cannot put a store outside a window.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t dest_array = ctx.r3.u32;
  const uint32_t source_array = ctx.r4.u32;
  const uint32_t slot_count = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;
  const uint32_t range = ctx.r7.u32;
  const uint32_t table = ctx.r8.u32;

  if (range == 0 || dest_array == 0) return false;  // the original would address through null
  // Past eight the closing loop reads flag bytes the eight-store clear never wrote, i.e. this
  // function's own uninitialised frame: the original's result would depend on stack residue that
  // a native body has no way to reproduce.
  if (slot_count > kFlagBytes) return false;

  struct Span {
    uint32_t addr;
    uint32_t len;
  };
  Span spans[skate3::audio::kPortMaxSpans];
  uint32_t span_count = 0;
  auto add = [&](uint32_t addr, uint32_t len) {
    if (len == 0) return true;
    for (uint32_t i = 0; i < span_count; i++) {
      if (spans[i].addr == addr && spans[i].len == len) return true;  // the same buffer again
    }
    if (span_count >= skate3::audio::kPortMaxSpans) return false;
    spans[span_count].addr = addr;
    spans[span_count].len = len;
    span_count++;
    return true;
  };

  spec.read(range, 2);
  spec.read(kGainTable, kGainTableBytes);

  // The flags are deliberately NOT simulated here. They decide only which of the two scale
  // routines runs -- both write the same span -- and which destinations the closing loop zeroes,
  // and that loop is declared for every slot instead. So no store can escape a window even if the
  // simulation in Native were wrong.
  const uint32_t first = REX_LOAD_U8(range + kRangeFirst);
  const uint32_t last = REX_LOAD_U8(range + kRangeLast);
  uint32_t table_lo = 0;
  uint32_t table_hi = 0;  // half-open; empty when the loop does not run
  if (first <= last) {
    if (table == 0 || source_array == 0) return false;
    table_lo = table + first;
    table_hi = table + last + 1;  // both are bytes, so at most 256 iterations
    spec.read(table_lo, (last - first) + 1);
    for (uint32_t index = first; index <= last; index++) {
      const uint32_t route = REX_LOAD_U8(table + index);
      const uint32_t dst = REX_LOAD_U32(dest_array + DestSlot(route));
      const uint32_t src = REX_LOAD_U32(source_array + SourceSlot(route));
      spec.read(dest_array + DestSlot(route), 4);
      spec.read(source_array + SourceSlot(route), 4);
      if (dst == 0) return false;  // the scale routine would store through null
      uint32_t len = 0;
      if (!ScaleSpan(dst, src, count, len)) return false;
      if (!add(dst, len)) return false;
      spec.read(src, len);
      spec.read(dst, len);  // sub_82B44B20 accumulates, so it reads the destination too
    }
  }

  if (slot_count != 0) {
    const uint32_t bytes = (count << 2) & 0xFFFFFFFCu;
    for (uint32_t i = 0; i < slot_count && i < kFlagBytes; i++) {
      const uint32_t slot = REX_LOAD_U32(dest_array + 4 * i);
      spec.read(dest_array + 4 * i, 4);
      if (slot == 0) return false;  // memset through null
      if (static_cast<uint64_t>(slot) + bytes > 0xFFFFFFFFull) return false;
      if (!add(slot, bytes)) return false;
    }
  }

  // Everything re-read inside the loops has to stay outside every write span, or neither the trip
  // count nor the destinations derive from entry state: the route range (reloaded each iteration),
  // the route bytes (reloaded after each sub_82B3BED8 call), and both pointer arrays (indexed
  // afresh every iteration and again by the closing loop).
  const Span reread[4] = {{range + kRangeFirst, 2},
                          {table_lo, table_hi - table_lo},
                          {dest_array, 4 * kFlagBytes},
                          {source_array, first <= last ? 4 * kFlagBytes : 0}};
  for (uint32_t i = 0; i < span_count; i++) {
    for (uint32_t k = 0; k < 4; k++) {
      const Span& f = reread[k];
      if (f.len == 0) continue;
      if (static_cast<uint64_t>(f.addr) < static_cast<uint64_t>(spans[i].addr) + spans[i].len &&
          static_cast<uint64_t>(spans[i].addr) < static_cast<uint64_t>(f.addr) + f.len) {
        return false;
      }
    }
  }

  for (uint32_t i = 0; i < span_count; i++) spec.write(spans[i].addr, spans[i].len);
  return true;
}

}  // namespace port_82B426D0
SKATE3_PORT(82B426D0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
