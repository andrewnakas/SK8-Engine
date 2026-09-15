// sub_82B370E8  role: expand three parameter blocks into the per-channel slots for the layout at +42, square-rooting the first   lifted: skate3_recomp.67.cpp:61203 (477 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 23 comparable calls in the weaker profile, 10112 in the stronger, against 477 lifted lines -- 477 lines against 23 calls in the weaker profile. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 46  play 20294  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B370E8.md
namespace port_82B370E8 {

constexpr uint32_t kLayout = 42;  // lbz r10,42(r3) -- 1, 2, 4, 6 or above 6; anything else returns

// lis r11,32759 ; ori r11,r11,65521 -- computed as ((32759 & 0xFFFF) << 16) + 0xFFF1, not read
// by eye. Stored with stw before every value; it is a NaN bit pattern, but the function treats
// it as an opaque word and nothing here establishes what it means.
constexpr uint32_t kSentinel = 0x7FF7FFF1u;

// The destination is an array of 8-byte entries, {u32 sentinel, float value}. Three groups of
// six entries, then a two-entry tail per group that only the above-6 layout fills:
//   group 0  dest +48   tail +192   source +272   fsqrts on the way through
//   group 1  dest +96   tail +208   source +304   copied
//   group 2  dest +144  tail +224   source +336   copied
// Each source block is eight floats. Sources (272..367) and destinations (48..239) are
// disjoint, so no load in this function can read a byte it stores.
constexpr uint32_t kGroups = 3;
constexpr uint32_t kSlots = 6;
constexpr uint32_t GroupDest(uint32_t g) { return 48 + 48 * g; }
constexpr uint32_t GroupTail(uint32_t g) { return 192 + 16 * g; }
constexpr uint32_t GroupSource(uint32_t g) { return 272 + 32 * g; }

// Which slot each source index lands in, per layout. Slot 1 is the one a single-channel
// layout uses and the one stereo and quad skip; reading the slots as
// {L, C, R, Ls, Rs, LFE} fits all five cases, but that is a reading, not a finding.
struct Map {
  uint32_t slot;
  uint32_t source;
};
constexpr Map kLayout1[] = {{1, 0}};
constexpr Map kLayout2[] = {{0, 0}, {2, 1}};
constexpr Map kLayout4[] = {{0, 0}, {2, 1}, {3, 2}, {4, 3}};
constexpr Map kLayoutWide[] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}};  // 6 and above: slots 0-4

struct Plan {
  const Map* slots;
  uint32_t count;
};

// cmplwi/bne chain on the layout byte, then `cmplwi cr6,r10,6 ; bltlr cr6`.
Plan PlanFor(uint32_t layout) {
  switch (layout) {
    case 1:
      return {kLayout1, 1};
    case 2:
      return {kLayout2, 2};
    case 4:
      return {kLayout4, 4};
    default:
      return (layout < 6) ? Plan{nullptr, 0} : Plan{kLayoutWide, 5};
  }
}

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

// One entry: the sentinel word, then the source float, square-rooted for group 0.
void Entry(uint8_t* base, uint32_t object, uint32_t g, uint32_t dest, uint32_t source) {
  // lfs f,src(r3)
  double value = LoadSingle(base, object + GroupSource(g) + 4 * source);
  if (g == 0) {
    value = double(float(sqrt(value)));  // fsqrts f,f -- the double sqrt rounded to single,
  }                                      // exactly as the lifted line does it
  REX_STORE_U32(object + dest, kSentinel);    // stw r11,dest(r3)
  StoreSingle(base, object + dest + 4, value);  // stfs f,dest+4(r3)
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t layout = REX_LOAD_U8(object + kLayout);  // lbz r10,42(r3)
  const Plan plan = PlanFor(layout);
  if (plan.slots == nullptr) {
    return;  // bltlr cr6 -- layouts 0, 3 and 5 write nothing at all
  }

  // Once per path, after `lis r11` and before the first lfs.
  ctx.fpscr.disableFlushModeUnconditional();
  // Group 2 first, then 1, then 0; ascending slot within a group. That is the order every
  // one of the four blocks emits, and the intra-pair order (stw before stfs or after) varies
  // with register scheduling -- unobservable here, since the sources cannot alias the
  // destinations.
  for (uint32_t g = kGroups; g-- > 0;) {
    for (uint32_t k = 0; k < plan.count; k++) {
      Entry(base, object, g, GroupDest(g) + 8 * plan.slots[k].slot, plan.slots[k].source);
    }
  }
  if (layout < 6) {
    return;  // layouts 1, 2 and 4 are complete
  }

  // bne cr6,0x82b3734c -- exactly 6 puts source 5 in slot 5 and stops; above 6 puts source 7
  // there and then fills the two-entry tail from sources 5 and 6.
  if (layout != 6) {
    ctx.fpscr.disableFlushModeUnconditional();  // at the top of loc_82B3734C
  }
  const uint32_t fifth = (layout == 6) ? 5 : 7;
  for (uint32_t g = kGroups; g-- > 0;) {
    Entry(base, object, g, GroupDest(g) + 8 * (kSlots - 1), fifth);
  }
  if (layout == 6) {
    return;
  }
  for (uint32_t g = kGroups; g-- > 0;) {
    Entry(base, object, g, GroupTail(g) + 0, 5);
    Entry(base, object, g, GroupTail(g) + 8, 6);
  }
}

// Every store is r3 + a constant and the layout byte is readable at entry, so the write set is
// exact for all five paths. For 6 and above every entry in the range is written, which is one
// contiguous span; the narrower layouts get one 8-byte span per entry (3, 6 or 12 spans).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t layout = REX_LOAD_U8(object + kLayout);
  spec.read(object + kLayout, 1);
  spec.read(object + GroupSource(0), 96);  // 272..367: the three eight-float source blocks

  const Plan plan = PlanFor(layout);
  if (plan.slots == nullptr) {
    return true;  // no writes on that path
  }
  if (layout >= 6) {
    // slots 0-5 of all three groups (48..191), plus the tail region (192..239) above 6
    spec.write(object + GroupDest(0), layout == 6 ? 144 : 192);
    return true;
  }
  for (uint32_t g = 0; g < 3; g++) {
    for (uint32_t k = 0; k < plan.count; k++) {
      spec.write(object + GroupDest(g) + 8 * plan.slots[k].slot, 8);
    }
  }
  return true;
}

}  // namespace port_82B370E8
SKATE3_PORT(82B370E8, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnNone)
