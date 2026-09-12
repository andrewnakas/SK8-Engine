// sub_82B468C0  role: gather one bank of float buffers into another -- when both channel counts are a standard layout, look the pair up in a 64-entry route table and hand the whole scatter-mix to sub_82B426D0; otherwise copy-with-unity-gain buffer by buffer through sub_82B3BED8 and zero any destination the sources ran out for   lifted: skate3_recomp.68.cpp:32346 (216 lines)
// STATUS: verified
// calls/session: boot 36882  play 51510  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B468C0.md
namespace port_82B468C0 {

// Arguments: r3 = the destination `float*` array, r4 = the source `float*` array, r5 = destination
// channels, r6 = source channels, r7 = floats per buffer. Void. All three call sites build both
// arrays as eight words of their own frame.
constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)
constexpr uint32_t kMaxSlots = 8;      // both arrays are eight entries at every call site

// lis r11,-32206 ; lfs f1,-22460(r11) -- ((-32206 & 0xFFFF) << 16) + (-22460), computed, not
// eyeballed. The gain every non-table path scales by; the value is not read here.
constexpr uint32_t kUnityGain = 0x8231A844;
// lis r10,-32241 ; addi r10,r10,-10496 -- 64 two-byte inclusive [first, last] route ranges, one per
// (source layout, destination layout) pair.
constexpr uint32_t kRangeTable = 0x820ED700;
// lis r8,-32241 ; addi r8,r8,-10368 -- the route bytes those ranges index.
constexpr uint32_t kRouteTable = 0x820ED780;

// MIRRORED from recomp/src/audio_ports/sub_82B426D0.inc, which is in aggregator ports_68a while
// this file is in ports_68b -- a different translation unit, so the values are copied rather than
// referenced. A change to that port's route decode or gain-table address requires a change here.
constexpr uint32_t kGainTable = 0x820ED6C0;
constexpr uint32_t kGainTableBytes = 16;
constexpr uint32_t kRouteDestMask = 7;
inline uint32_t GainSlot(uint32_t route) { return (route >> 4) & 0xC; }
inline uint32_t SourceSlot(uint32_t route) { return (route >> 1) & 0x1C; }
inline uint32_t DestSlot(uint32_t route) { return (route & kRouteDestMask) << 2; }

// cmplwi cr6,r5,1 / 2 / 4 / 6 / 8 -- five channel layouts, the same ladder applied to r5 and r6.
inline bool IsStandardLayout(uint32_t count) {
  return count == 1 || count == 2 || count == 4 || count == 6 || count == 8;
}

// MIRRORS recomp/src/audio_ports/sub_82B3BED8.inc's Windows(): the call writes exactly
// [dst, dst + count*4), the vector path's span derived by srawi-then-shift. Copied, not referenced
// (ports_68a). A change to that port's write set requires a change here.
inline bool SpanBED8(uint32_t dst, uint32_t src, uint32_t count, uint32_t& len) {
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    len = (static_cast<uint32_t>(static_cast<int32_t>(count) >> 2) << 4) & 0xFFFFFFF0u;
    // The vector loop exits only when the cursor lands exactly on the end pointer after whole
    // 128-byte steps; a zero or non-multiple span never terminates and the original hangs there.
    if (len == 0 || (len & 127u) != 0) return false;
  } else {
    len = (count << 2) & 0xFFFFFFFCu;
    if (dst + len <= dst) len = 0;  // dst >= dst_end: the call writes nothing at all
  }
  return static_cast<uint64_t>(dst) + len <= 0xFFFFFFFFull;
}

// MIRRORS recomp/src/audio_ports/sub_82B426D0.inc's ScaleSpan(): on the route path that callee
// picks between sub_82B3BED8 and sub_82B44B20 on a per-destination flag this function cannot see,
// and the two vector formulas differ only for a negative count, so both are computed and the longer
// declared.
inline bool SpanEither(uint32_t dst, uint32_t src, uint32_t count, uint32_t& len) {
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    const uint32_t by_shift = (count << 2) & 0xFFFFFFF0u;  // sub_82B44B20
    const uint32_t by_srawi =
        (static_cast<uint32_t>(static_cast<int32_t>(count) >> 2) << 4) & 0xFFFFFFF0u;  // sub_82B3BED8
    if (by_shift == 0 || (by_shift & 127u) != 0) return false;
    if (by_srawi == 0 || (by_srawi & 127u) != 0) return false;
    len = by_shift > by_srawi ? by_shift : by_srawi;
  } else {
    len = (count << 2) & 0xFFFFFFFCu;
    if (dst + len <= dst) len = 0;
  }
  return static_cast<uint64_t>(dst) + len <= 0xFFFFFFFFull;
}

// The two scale loops (loc_82B46954 and loc_82B469C8) are one machine with different registers:
// f1 is loaded ONCE from the constant pool -- the original never reloads it, so the loop depends on
// sub_82B3BED8 leaving f1 alone, and that dependence is reproduced rather than papered over -- and
// each iteration reads the destination A[i] and the source B[i] out of the two pointer arrays.
// r5 is deliberately left alone, as the original does, so the registers are written by hand instead
// of through GuestCall's argument list.
void ScaleLoop(PPCContext& ctx, uint8_t* base, uint32_t dest_array, uint32_t source_array,
               uint64_t floats, uint32_t trips) {
  const uint32_t delta = source_array - dest_array;  // subf r26,r28,r4 / subf r28,r28,r4
  uint32_t cursor = dest_array;                      // mr r31,r28
  ctx.fpscr.disableFlushModeUnconditional();
  PPCRegister gain{};
  gain.u32 = REX_LOAD_U32(kUnityGain);  // lfs f1,-22460(r11)
  ctx.f1.f64 = static_cast<double>(gain.f32);
  do {  // loc_82B46954 / loc_82B469C8
    ctx.r6.u64 = floats;                                 // mr r6,r25
    ctx.r4.u64 = REX_LOAD_U32(delta + cursor);           // lwzx r4,r26,r31 -- B[i], the 32-bit sum
    ctx.r3.u64 = REX_LOAD_U32(cursor);                   // lwz r3,0(r31)   -- A[i]
    skate3::audio::GuestCall(ctx, base, sub_82B3BED8);   // bl 0x82b3bed8
    cursor += 4;                                         // addi r31,r31,4
    // addic. r30,r30,-1 ; bne -- the branch tests the low 32 bits, so the trip count is r6.u32
    // (or r29.u32), and both are non-zero by the guard above the loop.
  } while (--trips != 0);
}

// loc_82B4698C -- zero the destinations the sources ran out for, one memset each.
void ZeroLoop(PPCContext& ctx, uint8_t* base, uint32_t dest_array, uint32_t first, uint32_t trips,
              uint64_t floats) {
  const uint32_t bytes = (static_cast<uint32_t>(floats) << 2) & 0xFFFFFFFCu;  // rlwinm r26,r25,2
  // rlwinm r11,r27,2,0,29 ; add r11,r11,r28 ; addi r30,r11,-4 -- the lwzu pre-increments.
  uint32_t cursor = ((first << 2) & 0xFFFFFFFCu) + dest_array - 4;
  do {
    cursor += 4;                                     // lwzu r3,4(r30)
    skate3::audio::GuestCall(ctx, base, sub_82EE5E80, uint64_t(REX_LOAD_U32(cursor)), uint64_t(0),
                             uint64_t(bytes));       // li r4,0 ; mr r5,r26 ; bl 0x82ee5e80
  } while (--trips != 0);                            // addic. r31,r31,-1 ; bne
}

REX_FUNC(Native) {
  const uint32_t dest_array = ctx.r3.u32;    // mr r28,r3
  const uint32_t source_array = ctx.r4.u32;
  const uint32_t dest_count = ctx.r5.u32;    // mr r29,r5
  const uint32_t source_count = ctx.r6.u32;
  const uint64_t floats = ctx.r7.u64;        // mr r25,r7

  // stwu r1,-144(r1). Reproduced, unlike sub_82B426D0.inc's own frame, for one reason: every callee
  // here spills below whatever r1 it is entered with (sub_82B3BED8's `stfs f1,-32(r1)`,
  // sub_82B426D0's own stwu), and if r1 sat 144 bytes higher during the replay those spills would
  // land at different addresses than in the original run. Any of them landing inside a declared
  // window would then read as a divergence. r25-r31 are never written here, so __savegprlr_25 has
  // nothing to reproduce.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  if (IsStandardLayout(dest_count) && IsStandardLayout(source_count)) {
    // loc_82B469EC -- both counts are a layout the route tables describe.
    // rlwinm r11,r6,3,0,28 ; add r11,r11,r29 ; addi r9,r11,-9 ; rlwinm r11,r9,1,0,30. Both rotates
    // wrap exactly the bits their masks clear, so they are plain shifts; the index is 8*src + dst -
    // 9, which is 0..63 for the five layouts, i.e. always inside the 128-byte table.
    const uint32_t index = ((source_count << 3) + dest_count) - 9;
    const uint32_t doubled = static_cast<uint32_t>(index << 1);
    // `lis` leaves the SIGN-EXTENDED constant in r10/r8 and `add r7,r11,r10` is 64-bit, so both
    // pointers reach the callee with 0xFFFFFFFF in the high half. sub_82B426D0 reads only the low
    // 32 bits, but the 64-bit form is reproduced rather than assumed.
    ctx.r3.u64 = dest_array;                                            // mr r3,r28
    ctx.r4.u64 = source_array;                                          // untouched by the original
    ctx.r5.u64 = dest_count;                                            // mr r5,r29
    ctx.r6.u64 = floats;                                                // mr r6,r25
    ctx.r7.u64 = uint64_t(int64_t(int32_t(kRangeTable))) + doubled;     // add r7,r11,r10
    ctx.r8.u64 = uint64_t(int64_t(int32_t(kRouteTable)));               // addi r8,r8,-10368
    skate3::audio::GuestCall(ctx, base, sub_82B426D0);                  // bl 0x82b426d0
  } else if (source_count >= dest_count) {
    // loc_82B469AC -- cmplw cr6,r6,r29 ; bge. More sources than destinations: every destination is
    // fed, nothing is zeroed.
    if (dest_count != 0) {  // cmplwi cr6,r29,0 ; beq cr6,0x82b46a20
      ScaleLoop(ctx, base, dest_array, source_array, floats, dest_count);
    }
  } else {
    // loc_82B46928 falling through -- fewer sources than destinations.
    uint32_t fed = 0;  // li r27,0
    if (source_count != 0) {  // cmplwi cr6,r6,0 ; beq cr6,0x82b46970
      ScaleLoop(ctx, base, dest_array, source_array, floats, source_count);
      fed = source_count;  // mr r27,r6
    }
    // loc_82B46970 -- cmplw cr6,r27,r29 ; bge cr6,0x82b46a20
    if (fed < dest_count) {
      ZeroLoop(ctx, base, dest_array, fed, dest_count - fed, floats);  // subf r31,r27,r29
    }
  }

  ctx.r1.u32 = entry_sp;  // addi r1,r1,144
}

// This function contains NO store of its own beyond the frame back-chain -- the census's
// `stores_present: false` is literally true of its instruction stream. Every guest-visible byte it
// writes comes from a callee, and all three callees are ported and verified, which is what makes the
// set enumerable:
//
//   * sub_82B3BED8 (recomp/src/audio_ports/sub_82B3BED8.inc): [r3, r3 + 4*r6).
//   * sub_82EE5E80: memset, [r3, r3 + r5).
//   * sub_82B426D0 (recomp/src/audio_ports/sub_82B426D0.inc): the union of one destination buffer
//     per route byte plus every one of its r5 destination slots; its own byte stores are the eight
//     flag bytes of its frame, NOT guest state.
//
// So the write set is a subset of the buffers named by the destination pointer array, and the whole
// job here is to enumerate which of them, from entry state.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t dest_array = ctx.r3.u32;
  const uint32_t source_array = ctx.r4.u32;
  const uint32_t dest_count = ctx.r5.u32;
  const uint32_t source_count = ctx.r6.u32;
  const uint32_t floats = ctx.r7.u32;
  if (dest_array == 0) return false;  // every path addresses off it

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

  // The extent of each pointer array that is re-read while the call runs, and the route bytes
  // sub_82B426D0 reloads after every non-accumulating scale. A declared write landing on any of
  // them would stop the addresses deriving from entry state -- gate 2, so it is checked below.
  uint32_t dest_slots = 0;
  uint32_t source_slots = 0;
  uint32_t routes_lo = 0;
  uint32_t routes_hi = 0;
  uint32_t range_addr = 0;
  uint32_t range_len = 0;

  if (IsStandardLayout(dest_count) && IsStandardLayout(source_count)) {
    // loc_82B469EC. Mirror of sub_82B426D0's Windows() with the arguments this path builds; both
    // counts are 1/2/4/6/8 here, so its own `slot_count > 8` refusal cannot trigger.
    if (source_array == 0) return false;
    range_addr = kRangeTable + static_cast<uint32_t>((((source_count << 3) + dest_count) - 9) << 1);
    range_len = 2;
    spec.read(range_addr, range_len);
    spec.read(kGainTable, kGainTableBytes);
    const uint32_t first = REX_LOAD_U8(range_addr + 0);
    const uint32_t last = REX_LOAD_U8(range_addr + 1);
    if (first <= last) {
      routes_lo = kRouteTable + first;
      routes_hi = kRouteTable + last + 1;  // both are bytes, so at most 256 iterations
      spec.read(routes_lo, (last - first) + 1);
      for (uint32_t index = first; index <= last; index++) {
        const uint32_t route = REX_LOAD_U8(kRouteTable + index);
        const uint32_t dst = REX_LOAD_U32(dest_array + DestSlot(route));
        const uint32_t src = REX_LOAD_U32(source_array + SourceSlot(route));
        spec.read(dest_array + DestSlot(route), 4);
        spec.read(source_array + SourceSlot(route), 4);
        if (dst == 0) return false;  // the scale routine would store through null
        uint32_t len = 0;
        if (!SpanEither(dst, src, floats, len)) return false;
        if (!add(dst, len)) return false;
        spec.read(src, len);
        spec.read(dst, len);  // sub_82B44B20 accumulates, so it reads the destination too
      }
    }
    // Its closing memset loop, declared for EVERY slot rather than only the flag-zero ones, so a
    // mis-simulated flag cannot put a store outside a window.
    const uint32_t bytes = (floats << 2) & 0xFFFFFFFCu;
    for (uint32_t i = 0; i < dest_count && i < kMaxSlots; i++) {
      const uint32_t slot = REX_LOAD_U32(dest_array + 4 * i);
      spec.read(dest_array + 4 * i, 4);
      if (slot == 0) return false;  // memset through null
      if (static_cast<uint64_t>(slot) + bytes > 0xFFFFFFFFull) return false;
      if (!add(slot, bytes)) return false;
    }
    // A route byte indexes either array at slot 0..7 regardless of the counts.
    dest_slots = kMaxSlots;
    source_slots = kMaxSlots;
  } else if (source_count >= dest_count) {
    // loc_82B469AC -- dest_count scales, no zeroing.
    if (dest_count == 0) return true;  // the call writes nothing at all
    if (dest_count > kMaxSlots) return false;  // past eight both arrays are the caller's residue
    if (source_array == 0) return false;
    spec.read(kUnityGain, 4);
    for (uint32_t i = 0; i < dest_count && i < kMaxSlots; i++) {
      const uint32_t dst = REX_LOAD_U32(dest_array + 4 * i);
      const uint32_t src = REX_LOAD_U32(source_array + 4 * i);
      spec.read(dest_array + 4 * i, 4);
      spec.read(source_array + 4 * i, 4);
      if (dst == 0) return false;
      uint32_t len = 0;
      if (!SpanBED8(dst, src, floats, len)) return false;
      if (!add(dst, len)) return false;
      spec.read(src, len);
    }
    dest_slots = dest_count;
    source_slots = dest_count;
  } else {
    // loc_82B46928 -- source_count scales, then dest_count - source_count zero fills.
    if (dest_count > kMaxSlots) return false;
    if (source_count != 0) {
      if (source_array == 0) return false;
      spec.read(kUnityGain, 4);
      for (uint32_t i = 0; i < source_count && i < kMaxSlots; i++) {
        const uint32_t dst = REX_LOAD_U32(dest_array + 4 * i);
        const uint32_t src = REX_LOAD_U32(source_array + 4 * i);
        spec.read(dest_array + 4 * i, 4);
        spec.read(source_array + 4 * i, 4);
        if (dst == 0) return false;
        uint32_t len = 0;
        if (!SpanBED8(dst, src, floats, len)) return false;
        if (!add(dst, len)) return false;
        spec.read(src, len);
      }
    }
    const uint32_t bytes = (floats << 2) & 0xFFFFFFFCu;
    for (uint32_t i = source_count; i < dest_count && i < kMaxSlots; i++) {
      const uint32_t slot = REX_LOAD_U32(dest_array + 4 * i);
      spec.read(dest_array + 4 * i, 4);
      if (slot == 0) return false;  // memset through null
      if (static_cast<uint64_t>(slot) + bytes > 0xFFFFFFFFull) return false;
      if (!add(slot, bytes)) return false;
    }
    dest_slots = dest_count;
    source_slots = source_count;
  }

  const Span reread[4] = {{dest_array, 4 * dest_slots},
                          {source_array, 4 * source_slots},
                          {routes_lo, routes_hi - routes_lo},
                          {range_addr, range_len}};
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

}  // namespace port_82B468C0
SKATE3_PORT(82B468C0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
