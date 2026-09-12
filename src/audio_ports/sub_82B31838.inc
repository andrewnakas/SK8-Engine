// sub_82B31838  role: mix one source's channels into this object's bus blocks for the block -- a gain ramp through sub_82B46810 when the gain has moved, flat through sub_82B44B20 per routing-table entry when it has not -- then bump the bus counter and publish the per-channel gains   lifted: skate3_recomp.67.cpp:48102 (421 lines)
// STATUS: verified
// calls/session: boot 626736  play 933816  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B31838.md
namespace port_82B31838 {

// Offsets on the object in r3. docs/rw_audio_structs.h names none of them, so each one is the
// raw offset plus the use the lifted body makes of it -- no invented field names beyond that.
constexpr uint32_t kModeOwner = 12;      // lwz r11,12(r31)  the object whose +72 selects the path
constexpr uint32_t kModeByte = 72;       // lbz r11,72(r11)  1, 3, or anything else
constexpr uint32_t kChannelCount = 41;   // lbz r27,41(r31)  channels to fan out; the table row
constexpr uint32_t kTargetGain = 52;     // lfs f0,52(r31)   the gain this block should end at
constexpr uint32_t kBusArray = 64;       // lwz r9,64(r31)   first of `buses` 1024-byte blocks
constexpr uint32_t kCounterOffset = 76;  // lhz r11,76(r31)  byte offset, inside that array, of
                                         //                  the word this call increments
constexpr uint32_t kBusCount = 78;       // lbz r7,78(r31)   bus blocks; the table column
constexpr uint32_t kChannelGains = 80;   // stfs 4(r10), r10 = r31+76: one single per channel
constexpr uint32_t kCurrentGain = 112;   // stfs f0,112(r31) the gain the buses were mixed at
constexpr uint32_t kGainDirty = 116;     // stb ...,116(r31) nonzero => snap instead of ramp

// On the command record in r4.
constexpr uint32_t kSourceList = 28;    // lwz r11,28(r4)
constexpr uint32_t kSourceFirst = 4;    // lwz r11,4(r11)   first channel buffer
constexpr uint32_t kSourceStride = 14;  // lhz r8,14(r11)   in words: rotlwi r8,r8,2 scales it
constexpr uint32_t kSourceGain = 1020;  // lfs f13,1020(r7) the per-channel gain cell

constexpr uint32_t kBusBytes = 1024;    // the block stride (addi r11,r11,1024) and the mix length
constexpr int64_t kMixSingles = 256;    // li r6,256 -- what sub_82B44B20 is told to mix
constexpr uint32_t kFrameBytes = 208;   // stwu r1,-208(r1)
constexpr uint32_t kChannelArray = 80;  // addi r4,r1,80  -- the channel pointers, in the frame
constexpr uint32_t kBusPtrArray = 112;  // addi r3,r1,112 -- the bus pointers, in the frame
// What the frame actually holds: (112-80)/4 channel pointers before the bus array starts, and
// (208-112)/4 bus pointers before the frame ends.
constexpr uint32_t kMaxFrameChannels = 8;
constexpr uint32_t kMaxFrameBuses = 24;

// Rodata, every address computed as ((lis_imm & 0xFFFF) << 16) + offset rather than read off by
// eye: one misread digit in sub_82B2FE00 cost this project's first shadow divergence.
constexpr uint32_t kZeroSingle = 0x82165A10;   // lis -32234 ; lfs 23056 -- the 0.0f cell
constexpr uint32_t kRouteGains = 0x820ED6C0;   // lis -32241 ; addi -10560 -- four gain constants
constexpr uint32_t kRoutePairs = 0x820ED700;   // lis -32241 ; addi -10496 -- {first,last} per row
constexpr uint32_t kRouteEntries = 0x820ED780; // lis -32241 ; addi -10368 -- the entry bytes
constexpr uint32_t kMaxRouteRun = 64;          // eight buses x eight channels: any longer run is
                                               // not a routing row, and is not enumerated

// lfs: the word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: the double rounded back to single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fmuls. Operand order is kept as written: float multiply is not NaN-commutative, and which
// operand's payload survives cannot be derived from source (docs/vmx128-exactness.md rule 4).
inline double MulSingle(double a, double b) {
  return static_cast<double>(static_cast<float>(a * b));
}

// rlwinm r11,r27,3,0,28 ; add r11,r11,r7 ; addi r11,r11,-9 ; rlwinm r10,r11,1,0,30 -- the row is
// picked by (channels, buses), the first row being one channel into one bus. sub_82B46810
// computes the same index from the same two registers.
inline uint32_t RoutePair(uint32_t channels, uint32_t buses) {
  return kRoutePairs + 2 * (8 * channels + buses - 9);
}

// Each entry byte packs three pre-scaled four-byte indices:
//   rlwinm r5,r11,2,27,29  -> (entry & 7) * 4        the destination bus
//   rlwinm r7,r11,31,27,29 -> ((entry >> 3) & 7) * 4 the source channel
//   rlwinm r8,r11,28,4,29  -> ((entry >> 6) & 3) * 4 the gain constant
inline uint32_t EntryBus(uint32_t entry) { return entry & 7u; }
inline uint32_t EntryChannel(uint32_t entry) { return (entry >> 3) & 7u; }
inline uint32_t EntryGain(uint32_t entry) { return (entry >> 6) & 3u; }

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                 // mr r31,r3
  const uint32_t command = ctx.r4.u32;
  const bool forced = (ctx.r5.u32 & 0xFFu) != 0;      // clrlwi r11,r5,24 ; cmplwi ; bne

  // stwu r1,-208(r1): the frame that holds the two pointer arrays the mixers are handed. Native
  // spills neither f31 nor r26-r31 (both stay in C++ locals), so the back chain and the arrays
  // are all it writes there -- all of it this call's own frame, and so never windowed. r1 has to
  // move all the same: every callee allocates below r1, and with r1 left at entry sub_82B46810's
  // own frame would sit on top of the arrays it is being passed.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);  // stwu r1,-208(r1)
  ctx.r1.u32 = frame;
  const uint32_t channel_ptrs = frame + kChannelArray;  // addi r4,r1,80
  const uint32_t bus_ptrs = frame + kBusPtrArray;       // addi r3,r1,112

  // loc_82B31864, entered when the flag argument is set or the gain is already marked dirty:
  // the buses are declared to be at the target gain, so the mix below will be flat.
  if (forced || REX_LOAD_U8(object + kGainDirty) != 0) {  // bne ; lbz r11,116(r3) ; beq
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, object + kCurrentGain,
                LoadSingle(base, object + kTargetGain));  // lfs f0,52(r31) ; stfs f0,112(r31)
    REX_STORE_U8(object + kGainDirty, 0);                 // stb r11,116(r31)
  }

  // loc_82B31874: no buses, nothing to mix. The gain is left marked dirty for the next call.
  const uint32_t buses = REX_LOAD_U8(object + kBusCount);  // lbz r7,78(r31)
  if (buses == 0) {                                        // cmplwi cr6,r7,0 ; bne
    REX_STORE_U8(object + kGainDirty, 1);                  // stb r11,116(r31)
    ctx.r3.s64 = 1;                                        // li r3,1
    ctx.r1.u32 = entry_sp;                                 // addi r1,r1,208
    return;
  }

  // loc_82B31898: one word inside the bus array counts the blocks mixed into it.
  const uint32_t bus_array = REX_LOAD_U32(object + kBusArray);        // lwz r9,64(r31)
  const uint32_t counter = bus_array + REX_LOAD_U16(object + kCounterOffset);  // lhz ; lwzx
  REX_STORE_U32(counter, REX_LOAD_U32(counter) + 1);                  // addi r10,r10,1 ; stwx

  const uint32_t channels = REX_LOAD_U8(object + kChannelCount);  // lbz r27,41(r31)
  const uint32_t sources = REX_LOAD_U32(command + kSourceList);   // lwz r11,28(r4), before the beq
  if (channels != 0) {
    // loc_82B318D0: the channel pointer array -- the first source buffer, then a fixed stride.
    // Both the base and the stride are zero-extended, and `add` is 64-bit, so the accumulator is
    // 64 bits wide and only the store truncates it.
    const uint64_t stride = static_cast<uint64_t>(REX_LOAD_U16(sources + kSourceStride)) << 2;
    uint64_t pointer = REX_LOAD_U32(sources + kSourceFirst);  // lwz r11,4(r11)
    for (uint32_t i = 0; i < channels; i++) {
      REX_STORE_U32(channel_ptrs + 4 * i, static_cast<uint32_t>(pointer));  // stwu r11,4(r10)
      pointer += stride;                                                    // add r11,r8,r11
    }
  }
  // loc_82B318F0: the bus pointer array. Same 64-bit accumulation, same truncating store. The
  // guard on r7 here cannot fail -- the path that reaches it already tested r7 nonzero.
  {
    uint64_t pointer = bus_array;  // mr r11,r9
    for (uint32_t i = 0; i < buses; i++) {
      REX_STORE_U32(bus_ptrs + 4 * i, static_cast<uint32_t>(pointer));  // stwu r11,4(r10)
      pointer += kBusBytes;                                             // addi r11,r11,1024
    }
  }

  // loc_82B318FC: three ways to mix. Modes 1 and 3 ramp between a pool zero and the gain in
  // question; anything else ramps from where the buses stand to the target, or -- if those are
  // already equal, which the snap above arranges -- mixes flat.
  const uint32_t owner = REX_LOAD_U32(object + kModeOwner);   // lwz r11,12(r31)
  const uint32_t mode = REX_LOAD_U8(owner + kModeByte);       // lbz r11,72(r11)
  if (mode == 1) {                                  // cmplwi cr6,r11,1 ; bne
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f1.f64 = LoadSingle(base, kZeroSingle);                    // lfs f1,23056(r11)
    ctx.f2.f64 = LoadSingle(base, object + kCurrentGain);          // lfs f2,112(r31)
    ctx.r7.u64 = buses;                                            // r7 still holds the count
    ctx.r8.u64 = channels;                                         // mr r8,r27
    skate3::audio::GuestCall(ctx, base, sub_82B46810, bus_ptrs, channel_ptrs);  // bl 0x82b46810
  } else if (mode == 3) {                           // cmplwi cr6,r11,3 ; bne
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f1.f64 = LoadSingle(base, object + kTargetGain);           // lfs f1,52(r31)
    ctx.f2.f64 = LoadSingle(base, kZeroSingle);                    // lfs f2,23056(r11)
    ctx.r7.u64 = buses;
    ctx.r8.u64 = channels;                                         // mr r8,r27
    skate3::audio::GuestCall(ctx, base, sub_82B46810, bus_ptrs, channel_ptrs);
  } else {
    // loc_82B31954.
    ctx.fpscr.disableFlushModeUnconditional();
    const double target = LoadSingle(base, object + kTargetGain);    // lfs f31,52(r31)
    const double current = LoadSingle(base, object + kCurrentGain);  // lfs f2,112(r31)
    if (target != current) {                                         // fcmpu cr6,f31,f2 ; beq
      ctx.f1.f64 = target;                                           // fmr f1,f31
      ctx.f2.f64 = current;
      ctx.r7.u64 = buses;
      ctx.r8.u64 = channels;                                         // mr r8,r27
      skate3::audio::GuestCall(ctx, base, sub_82B46810, bus_ptrs, channel_ptrs);
    } else {
      // loc_82B3197C: the gain has not moved, so walk the routing row and mix each entry flat.
      // This is sub_82B46810's loop with the ramp collapsed to a single gain.
      const uint32_t pair = RoutePair(channels, buses);
      const uint32_t first = REX_LOAD_U8(pair);       // lbzx r30,r11,r10
      if (first <= REX_LOAD_U8(pair + 1)) {           // lbz r8,1(r29) ; cmplw ; bgt
        uint32_t reached = first;                     // r30
        uint32_t cursor = kRouteEntries + first - 1;  // addi r28,r11,-1
        do {
          cursor += 1;                                 // lbzu r11,1(r28): the pre-increment, kept
          const uint32_t entry = REX_LOAD_U8(cursor);  // out of the macro argument on purpose --
                                                       // REX_LOAD_* names its argument twice
          ctx.fpscr.disableFlushModeUnconditional();
          const double gain =
              LoadSingle(base, kRouteGains + 4 * EntryGain(entry));  // lfsx f0,r8,r26
          const uint32_t source =
              REX_LOAD_U32(channel_ptrs + 4 * EntryChannel(entry));  // lwzx r4,r7,r10
          const uint32_t bus = REX_LOAD_U32(bus_ptrs + 4 * EntryBus(entry));  // lwzx r3,r5,r9
          ctx.f1.f64 = MulSingle(gain, target);                      // fmuls f1,f0,f31
          ctx.r6.s64 = kMixSingles;                                  // li r6,256
          skate3::audio::GuestCall(ctx, base, sub_82B44B20, bus, source);  // bl 0x82b44b20
          reached++;                                                 // addi r30,r30,1
        } while (reached <= REX_LOAD_U8(pair + 1));  // lbz r4,1(r29) ; cmplw ; ble
      }
    }
  }

  // loc_82B31A00, reached from all four paths above: the buses now hold the target gain.
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, object + kCurrentGain,
              LoadSingle(base, object + kTargetGain));  // lfs f0,52(r31) ; stfs f0,112(r31)

  // loc_82B31A30 and loc_82B31AB0: the published per-channel gains. The original runs this four
  // at a time and then a remainder loop; both walk i ascending, both reload obj+52 for every
  // multiply, and between them they cover exactly i in [0, channels) -- the unrolled trip count
  // is floor(channels/4) and the remainder is channels mod 4. The channel pointers come off this
  // frame and the stores land on the object, so nothing in the loop can alias a later load, which
  // is why the unrolled form's four-pointers-up-front ordering does not have to be reproduced.
  for (uint32_t i = 0; i < channels; i++) {
    const uint32_t channel = REX_LOAD_U32(channel_ptrs + 4 * i);  // lwz r7,4(r11) / lwzu
    ctx.fpscr.disableFlushModeUnconditional();
    const double scale = LoadSingle(base, object + kTargetGain);  // lfs f0,52(r31)
    StoreSingle(base, object + kChannelGains + 4 * i,
                MulSingle(LoadSingle(base, channel + kSourceGain),
                          scale));  // lfs f13,1020(r7) ; fmuls f12,f13,f0 ; stfs f12,4(r10)
  }

  ctx.r3.s64 = 1;         // li r3,1
  ctx.r1.u32 = entry_sp;  // addi r1,r1,208
}

// Everything this call writes, from entry state. The two pointer arrays and the back chain are
// in this function's own frame and are excluded; so are the callees' frames.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t command = ctx.r4.u32;

  // The union of both branches of the snap: a byte a given call leaves alone compares equal.
  spec.write(object + kCurrentGain, 5);  // stfs 112(r31) ; stb 116(r31)
  spec.read(object + kChannelCount, 1);
  spec.read(object + kTargetGain, 4);
  spec.read(object + kBusArray, 4);
  spec.read(object + kCounterOffset, 4);  // 76..77 the offset, 78 the bus count

  const uint32_t buses = REX_LOAD_U8(object + kBusCount);
  if (buses == 0) return true;  // the early return writes nothing but those five bytes

  const uint32_t bus_array = REX_LOAD_U32(object + kBusArray);
  if (bus_array == 0) return false;  // the counter store would go through null
  const uint32_t counter = bus_array + REX_LOAD_U16(object + kCounterOffset);
  // A block array or a counter that runs off the top of the address space: no describable window.
  if (static_cast<uint64_t>(counter) + 4 > 0xFFFFFFFFull) return false;
  if (static_cast<uint64_t>(bus_array) + static_cast<uint64_t>(kBusBytes) * buses >
      0xFFFFFFFFull) {
    return false;
  }
  spec.write(counter, 4);  // stwx r10,r11,r9

  const uint32_t channels = REX_LOAD_U8(object + kChannelCount);
  // The two arrays live at frame+80 and frame+112 inside a 208-byte frame, so the frame holds
  // eight channel pointers and twenty-four bus pointers. Past either count the original overruns
  // -- the channel array into the bus array, the bus array into the caller's frame -- and a body
  // that reproduced that would be writing outside any window this can declare. Not comparable.
  if (channels > kMaxFrameChannels || buses > kMaxFrameBuses) return false;
  const uint32_t sources = REX_LOAD_U32(command + kSourceList);
  spec.read(command + kSourceList, 4);
  if (channels != 0) {
    if (sources == 0) return false;  // lhz 14(r11) through null
    spec.read(sources + kSourceFirst, 12);              // +4 the base, +14 the stride
    spec.write(object + kChannelGains, 4 * channels);   // one single per channel
  }

  const uint32_t owner = REX_LOAD_U32(object + kModeOwner);
  if (owner == 0) return false;  // lbz 72(r11) through null
  spec.read(owner + kModeByte, 1);

  // Which bus blocks the mixers touch is a routing-table walk, and the table is rodata: readable
  // before the call, and the same walk on every mixing path (sub_82B46810 computes the row from
  // the same two counts, and the flat path walks it inline). The destination is always one of
  // eight bus pointers this call itself builds, so the write set is eight 1024-byte blocks at
  // most, whatever the bus count is. Both mixers are ported and verified: sub_82B44D18 writes
  // [(dst & ~0xF), +1024) because every stvx128 masks the low four address bits off, and
  // sub_82B44B20 writes [dst, +1024) on both of its paths. One span per block covers both.
  if (8 * channels + buses < 9) return false;  // the row index goes negative: not a table row
  const uint32_t pair = RoutePair(channels, buses);
  const uint32_t first = REX_LOAD_U8(pair);
  const uint32_t last = REX_LOAD_U8(pair + 1);
  spec.read(pair, 2);
  if (first > last) return true;                  // bgt: this row mixes nothing
  if (last - first >= kMaxRouteRun) return false;  // longer than any real row: not enumerated

  uint32_t touched_buses = 0;
  uint32_t touched_channels = 0;
  for (uint32_t i = first; i <= last && i - first < kMaxRouteRun; i++) {
    const uint32_t entry = REX_LOAD_U8(kRouteEntries + i);
    // An index past the array this call built would send a mixer at an uninitialised frame word.
    if (EntryBus(entry) >= buses) return false;
    if (EntryChannel(entry) >= channels) return false;
    touched_buses |= 1u << EntryBus(entry);
    touched_channels |= 1u << EntryChannel(entry);
  }
  spec.read(kRouteEntries + first, last - first + 1);
  spec.read(kRouteGains, 16);
  spec.read(kZeroSingle, 4);

  for (uint32_t bus = 0; bus < 8; bus++) {
    if ((touched_buses & (1u << bus)) == 0) continue;
    const uint32_t dst = bus_array + kBusBytes * bus;
    spec.write(dst & ~0xFu, kBusBytes + (dst & 0xFu));
    // The block is accumulated into, so it is read as well -- already covered by the window.
  }
  const uint64_t stride = static_cast<uint64_t>(REX_LOAD_U16(sources + kSourceStride)) << 2;
  const uint32_t base_source = REX_LOAD_U32(sources + kSourceFirst);
  for (uint32_t channel = 0; channel < 8; channel++) {
    if ((touched_channels & (1u << channel)) == 0) continue;
    spec.read(static_cast<uint32_t>(base_source + stride * channel), kBusBytes);
  }
  return true;
}

}  // namespace port_82B31838
SKATE3_PORT(82B31838, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
