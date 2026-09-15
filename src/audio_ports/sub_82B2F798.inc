// sub_82B2F798  role: settle two level terms from log10 of a stored single and of the largest of up to 255 candidates, store one at +32 and add the pair's delta into (*(+12))+40   lifted: skate3_recomp.67.cpp:43282 (178 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 4 comparable calls in the weaker profile, 4 in the stronger, against 178 lifted lines -- four calls. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 4  play 4  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2F798.md
namespace port_82B2F798 {

// r3 = the object. Nothing in docs/rw_audio_structs.h names this layout; the offsets stay numbers.
constexpr uint32_t kLinked = 12;         // u32  pointer to a second object; its +40 is accumulated
constexpr uint32_t kLinkedLevel = 40;    // f32  on the linked object: += (this call's delta)
constexpr uint32_t kLevel = 32;          // f32  overwritten with this call's sum
constexpr uint32_t kFirstCount = 408;    // s32  converted to a single, the first term's n
constexpr uint32_t kFirstValue = 460;    // f32  log10 argument of the first term
constexpr uint32_t kCandidates = 1060;   // f32[]  max-reduced (floor 0.0) for the second log10
constexpr uint32_t kCounts = 1072;       // s32[]  max-reduced (floor 0) for the second term's n
constexpr uint32_t kCandidateCount = 1089;  // u8  trip count of both reductions

constexpr uint32_t kTen = 0x821963E4;    // lis -32231 ; lfs 25572  == 10.0 (read from the image)
constexpr uint32_t kZero = 0x82165A10;   // lis -32234 ; lfs 23056  == 0.0

// The read-only pools sub_82F55068 and its natural log pull in -- port_82F55068's own read list,
// mirrored so the vector replays.
constexpr uint32_t kLogPool = 0x82050000u + 8344;
constexpr uint32_t kLogPoolBytes = 144;
constexpr uint32_t kZeroOnePool = 0x82010000u + 248;
constexpr uint32_t kZeroOneBytes = 24;
constexpr uint32_t kHalfDouble = 0x82010000u + 648;
constexpr uint32_t kInfinityPool = 0x82FB0000u + 22720;
constexpr uint32_t kInfinityBytes = 16;

// stwu r1,-128(r1), and the std/lfd scratch at 80(r1) the two int-to-float conversions go through.
// Reproduced, as in port_82B43CC0: sub_82F55068 builds its own 96-byte frame under this one and
// sub_82F54ED8 spills into it, so without the move their scratch would land on this function's
// spill slots instead of where the original put it.
constexpr uint32_t kFrameSize = 128;
constexpr uint32_t kScratch = 80;
// The deepest stack byte the call chain writes: this frame, sub_82F55068's 96, and sub_82F54ED8's
// -16(r1) spill below that. Used only to prove the +12 pointer is not re-read from stack scratch.
constexpr uint32_t kStackReach = 256;

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

double RoundSingle(double a) { return double(float(a)); }              // frsp
double MulSingle(double a, double b) { return double(float(a * b)); }  // fmuls
double AddSingle(double a, double b) { return double(float(a + b)); }  // fadds
double SubSingle(double a, double b) { return double(float(a - b)); }  // fsubs
double DivSingle(double a, double b) { return double(float(a / b)); }  // fdivs

// extsw ; std rN,80(r1) ; lfd f0,80(r1) ; fcfid f13,f0 ; frsp -- through the frame's scratch
// doubleword, as the original does it.
double WordToSingle(uint8_t* base, uint32_t frame, uint32_t word) {
  PPCRegister wide{};
  wide.s64 = int32_t(word);                        // extsw
  REX_STORE_U64(frame + kScratch, wide.u64);       // std rN,80(r1)
  PPCRegister f0{};
  f0.u64 = REX_LOAD_U64(frame + kScratch);         // lfd f0,80(r1)
  return RoundSingle(double(f0.s64));              // fcfid f13,f0 ; frsp
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r31,r3
  // r31 and f29-f31 are spilled by the original and restored on exit; the harness compares both.
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister saved_f29 = ctx.f29;
  const PPCRegister saved_f30 = ctx.f30;
  const PPCRegister saved_f31 = ctx.f31;

  ctx.fpscr.disableFlushModeUnconditional();  // stfd f29,-40(r1)
  const uint32_t frame = ctx.r1.u32 - kFrameSize;  // stwu r1,-128(r1)
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;

  const uint32_t first_count = REX_LOAD_U32(object + kFirstCount);  // lwz r11,408(r3)
  ctx.f1.f64 = LoadSingle(base, object + kFirstValue);              // lfs f1,460(r3)
  const double n1 = WordToSingle(base, frame, first_count);          // ... frsp f30,f13

  skate3::audio::GuestCall(ctx, base, sub_82F55068);  // bl 0x82f55068 -- log10(f1)
  ctx.fpscr.disableFlushModeUnconditional();
  const double log1 = RoundSingle(ctx.f1.f64);  // frsp f12,f1
  const uint32_t count = REX_LOAD_U8(object + kCandidateCount);  // lbz r9,1089(r31)
  const double ten = LoadSingle(base, kTen);    // lfs f31,25572(r8)
  const double n1_ten = MulSingle(n1, ten);     // fmuls f11,f30,f31
  double largest = LoadSingle(base, kZero);     // lfs f1,23056(r7)
  const double term1 = SubSingle(n1, DivSingle(n1_ten, log1));  // fdivs f10,f11,f12 ; fsubs f30

  // cmpwi cr6,r9,0 ; ble -- a byte, so this is simply "count != 0".
  if (int32_t(count) > 0) {
    uint32_t cursor = object + kCandidates;  // addi r11,r31,1060
    uint64_t trips = count & 0xFF;           // clrlwi r10,r9,24 ; mtctr r10
    do {
      ctx.fpscr.disableFlushModeUnconditional();
      const double candidate = LoadSingle(base, cursor);  // lfs f0,0(r11)
      // fcmpu cr6,f0,f1 ; ble -- an unordered compare (NaN) keeps the running value.
      if (candidate > largest) largest = candidate;       // fmr f1,f0
      cursor += 4;                                        // addi r11,r11,4
    } while (uint32_t(--trips) != 0);                     // bdnz 0x82b2f810
  }

  int32_t most = 0;  // li r10,0
  if (int32_t(count) > 0) {  // cmpwi cr6,r9,0 ; ble
    uint64_t trips = REX_LOAD_U8(object + kCandidateCount);  // lbz r11,1089(r31) ; mtctr r11
    uint32_t cursor = object + kCounts;                       // addi r9,r31,1072
    do {
      const int32_t value = int32_t(REX_LOAD_U32(cursor));    // lwz r11,0(r9)
      if (value > most) most = value;                         // cmpw cr6,r11,r10 ; ble ; mr r10,r11
      cursor += 4;                                            // addi r9,r9,4
    } while (uint32_t(--trips) != 0);                         // bdnz 0x82b2f840
  }

  ctx.fpscr.disableFlushModeUnconditional();
  const double n2 = WordToSingle(base, frame, uint32_t(most));  // extsw ... frsp f29,f13

  ctx.f1.f64 = largest;
  skate3::audio::GuestCall(ctx, base, sub_82F55068);  // bl 0x82f55068 -- log10(largest)
  ctx.fpscr.disableFlushModeUnconditional();
  const double log2 = RoundSingle(ctx.f1.f64);                   // frsp f12,f1
  const double level = LoadSingle(base, object + kLevel);        // lfs f11,32(r31)
  const double n2_ten = MulSingle(n2, ten);                      // fmuls f10,f29,f31
  const uint32_t linked = REX_LOAD_U32(object + kLinked);        // lwz r10,12(r31)
  const double linked_level = LoadSingle(base, linked + kLinkedLevel);  // lfs f9,40(r10)
  const double term2 = SubSingle(n2, DivSingle(n2_ten, log2));   // fdivs f8,f10,f12 ; fsubs f7,f29,f8
  const double sum = AddSingle(term2, term1);                    // fadds f6,f7,f30
  const double delta = SubSingle(sum, level);                    // fsubs f5,f6,f11
  StoreSingle(base, linked + kLinkedLevel, AddSingle(delta, linked_level));  // fadds f4 ; stfs f4,40(r10)
  StoreSingle(base, object + kLevel, sum);                                   // stfs f6,32(r31)

  ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,128
  ctx.r31 = saved_r31;                   // ld r31,-16(r1)
  ctx.f29 = saved_f29;                   // lfd f29,-40(r1)
  ctx.f30 = saved_f30;                   // lfd f30,-32(r1)
  ctx.f31 = saved_f31;                   // lfd f31,-24(r1)
}

// Two words: object+32, and linked+40 where linked = *(object+12). That pointer is loaded only after
// both log10 calls, but the whole sub_82F55068 closure writes nothing above its own frames
// (port_82F55068: empty write set), so it is the entry value -- unless the object itself sits in
// the stack range the chain scribbles on, which is checked rather than assumed.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;  // the original would load through null
  const uint64_t sp = ctx.r1.u32;
  const uint64_t link_word = uint64_t(object) + kLinked;
  if (link_word + 4 > sp - kStackReach && link_word < sp) return false;  // re-read from scratch
  const uint32_t linked = REX_LOAD_U32(object + kLinked);
  if (linked == 0) return false;  // the original would store to guest address 40

  spec.write(linked + kLinkedLevel, 4);  // stfs f4,40(r10)
  spec.write(object + kLevel, 4);        // stfs f6,32(r31)

  const uint32_t count = REX_LOAD_U8(object + kCandidateCount);
  spec.read(object + kLinked, 4);
  spec.read(object + kFirstCount, 4);
  spec.read(object + kFirstValue, 4);
  spec.read(object + kCandidateCount, 1);
  if (count != 0) spec.read(object + kCandidates, (kCounts - kCandidates) + 4 * count);
  spec.read(kTen, 4);
  spec.read(kZero, 4);
  spec.read(kLogPool, kLogPoolBytes);
  spec.read(kZeroOnePool, kZeroOneBytes);
  spec.read(kHalfDouble, 8);
  spec.read(kInfinityPool, kInfinityBytes);
  return true;
}

}  // namespace port_82B2F798
SKATE3_PORT(82B2F798, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnNone)
