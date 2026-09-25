// sub_82B222D8  role: per-block driver for one streamed voice -- install the mixer pair {sub_82B3D4F8, sub_82B3D578} at +76, borrow 2816 bytes of scratch from the owner's arena (+108, 704 words at +128), take this block's length as round(float[+52] * the format's rate), step the start/run/drain state at +64 (publishing the clamped level at +60 into the mixer's pair at +92/+96 and rewinding the block object at +100 on a start), run sub_82B3D8E0 over the stream's two buffers and swap them, republish through sub_82B225A0, give the scratch back, return 1   lifted: skate3_recomp.67.cpp:10574 (419 lines)
// STATUS: gate-1 (the mixer's reset entry through the pointer it has just installed at +80: sub_82B222D8 indirect call, `bctrl` at 0x82B224DC; sub_82B3D8E0 reaches two more)
// calls/session: boot 111801  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B222D8.md
namespace port_82B222D8 {

// Constant addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye.
constexpr uint32_t kMixDispatch = 0x82B3D4F8;  // lis r11,-32076 ; addi r9,r11,-11016
constexpr uint32_t kMixReset = 0x82B3D578;     // lis r10,-32076 ; addi r8,r10,-10888
constexpr uint32_t kZeroFloat = 0x82165A10;    // lis r6,-32234 ; lfs f30,23056(r6)
constexpr uint32_t kRoundHalf = 0x8209975C;    // lis r11,-32246 ; lfs f13,-26788(r11)
constexpr uint32_t kLevelLimit = 0x820ED5E8;   // lis r11,-32241 ; lfs f13,-10776(r11)
constexpr uint32_t kLevelFloor = 0x822F8E60;   // lis r11,-32208 ; addi r10,r11,-31232 ; lfs 2144(r10)

// The voice (r3/r29). Nothing in docs/rw_audio_structs.h names it; the names below come from the
// two callees that share its fields (sub_82B225A0 reads +64/+92/+140, sub_82B3D8E0 takes +100).
constexpr uint32_t kOwner = 8;          // ->owner; owner+0 is the scratch arena
constexpr uint32_t kGain = 52;          // float, multiplied by the format's rate
constexpr uint32_t kLevel = 60;         // float, clamped and published into the mixer pair
constexpr uint32_t kState = 64;         // u32: 0 start, 1 run, 2 drain, >=3 idle (sub_82B225A0's mode)
constexpr uint32_t kMixer = 76;         // addi r30,r3,76 -- an embedded two-entry mixer object
constexpr uint32_t kBlock = 100;        // addi r31,r3,100 -- the object sub_82B3D8E0 runs
constexpr uint32_t kScratch = 108;      // == block+8: the borrowed scratch pointer
constexpr uint32_t kScratchWords = 128; // == block+28: li r7,704
constexpr uint32_t kScratchWordCount = 704;
constexpr uint64_t kScratchBytes = 2816;  // addi r11,r26,-2816 (704 words)

// The embedded mixer (voice+76): two function pointers, then the level pair sub_82B3D578 zeroes.
constexpr uint32_t kMixFn = 0;          // sub_82B3D8E0 calls through this one
constexpr uint32_t kResetFn = 4;        // this function calls through this one
constexpr uint32_t kMixLevel = 16;      // float, the new level (voice+92)
constexpr uint32_t kMixLevelPrev = 20;  // float, the previous level (voice+96)

// The block object (voice+100), named as sub_82B3D8E0's port names them.
constexpr uint32_t kObjMixer = 4;          // stw r30,104(r3)
constexpr uint32_t kObjLimitA = 12;        // min(+12, +32) bounds the block length
constexpr uint32_t kObjSpanHigh = 20;
constexpr uint32_t kObjSpanLow = 24;
constexpr uint32_t kObjSecondCursor = 32;
constexpr uint32_t kObjCursor = 36;
constexpr uint32_t kObjLength = 40;        // this block's length (sub_82B225A0's +140)
constexpr uint32_t kObjLengthPrev = 44;
constexpr uint32_t kObjPosition = 52;
constexpr uint32_t kObjTwoBuffers = 56;    // u8

// The stream (r4/r27) and the owner's arena.
constexpr uint32_t kStreamIn = 28;       // swapped with +32 after the block runs
constexpr uint32_t kStreamOut = 32;
constexpr uint32_t kStreamFormat = 40;   // ->format; format+12 is the float rate
constexpr uint32_t kFormatRate = 12;
constexpr uint32_t kArenaTop = 32;       // lowered by 2816 on entry, restored on exit

constexpr uint32_t kFrameBytes = 160;    // stwu r1,-160(r1)

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

// fctiwz ; stfd f13,80(r1) ; lwz r28,84(r1): NaN gives 0x80000000, above INT_MAX saturates, and
// only the low word of the spilled doubleword reaches the register.
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(static_cast<uint64_t>(fixed) & 0xFFFFFFFFu);
}

// lwz r11,32(r31) ; lwz r10,12(r31) ; cmpw cr6,r11,r10 ; blt keep ; mr r11,r10 -- signed min.
int32_t BlockLimit(uint8_t* base, uint32_t block) {
  const int32_t second = int32_t(REX_LOAD_U32(block + kObjSecondCursor));
  const int32_t limit = int32_t(REX_LOAD_U32(block + kObjLimitA));
  return second < limit ? second : limit;
}

// The clamp all three arms repeat: a level whose magnitude exceeds the limit becomes the limit
// when positive and the pool floor otherwise. `ble` is "not greater", so a NaN keeps the level.
double ClampLevel(PPCContext& ctx, uint8_t* base, double level, double zero) {
  ctx.fpscr.disableFlushModeUnconditional();          // fabs f12,f31
  PPCRegister magnitude{};
  magnitude.f64 = level;
  magnitude.u64 &= ~0x8000000000000000ull;
  double result = level;                              // fmr f0,f31
  const double limit = LoadSingle(base, kLevelLimit); // lfs f13,-10776(r11)
  if (magnitude.f64 > limit) {                        // fcmpu cr6,f12,f13 ; ble
    if (level > zero) {                               // fcmpu cr6,f31,f30 ; ble
      result = limit;                                 // fmr f0,f13
    } else {
      ctx.fpscr.disableFlushModeUnconditional();      // lfs f0,2144(r10)
      result = LoadSingle(base, kLevelFloor);
    }
  }
  return result;
}

REX_FUNC(Native) {
  // Prologue: mflr r12 ; bl __savegprlr_26 ; stfd f30/f31 ; stwu. The spills land in this call's
  // own frame and are not reproduced: r26-r31, f30 and f31 are never written here (their roles
  // are C++ locals), so they still hold the entry values the epilogue would reload. The frame
  // itself is reproduced because the three callees run on it.
  const uint64_t entry_lr = ctx.lr;
  ctx.r12.u64 = entry_lr;                          // mflr r12
  ctx.fpscr.disableFlushModeUnconditional();       // stfd f30,-72(r1)
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;   // stwu r1,-160(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t self64 = ctx.r3.u64;              // mr r29,r3
  const uint32_t self = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;              // mr r27,r4
  const uint64_t mixer64 = self64 + kMixer;        // addi r30,r3,76 -- 64-bit, as lifted
  const uint64_t block64 = self64 + kBlock;        // addi r31,r3,100
  const uint32_t mixer = uint32_t(mixer64);
  const uint32_t block = uint32_t(block64);

  REX_STORE_U32(self + kMixer + kMixFn, kMixDispatch);         // stw r9,76(r3)
  REX_STORE_U32(self + kMixer + kResetFn, kMixReset);          // stw r8,80(r3)
  const uint32_t owner = REX_LOAD_U32(self + kOwner);          // lwz r5,8(r3)
  REX_STORE_U32(self + kBlock + kObjMixer, mixer);             // stw r30,104(r3)
  const double zero = LoadSingle(base, kZeroFloat);            // lfs f30,23056(r6)
  const uint32_t arena = REX_LOAD_U32(owner + 0);              // lwz r4,0(r5)
  const uint64_t saved_top = REX_LOAD_U32(arena + kArenaTop);  // lwz r26,32(r4)
  const uint64_t scratch = saved_top - kScratchBytes;          // addi r11,r26,-2816 (64-bit)
  REX_STORE_U32(arena + kArenaTop, uint32_t(scratch));         // stw r11,32(r4)
  REX_STORE_U32(self + kScratch, uint32_t(scratch));           // stw r11,108(r3)
  REX_STORE_U32(self + kScratchWords, kScratchWordCount);      // stw r7,128(r3)

  const double gain = LoadSingle(base, self + kGain);                // lfs f0,52(r3)
  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);      // lwz r3,40(r27)
  const double level = LoadSingle(base, self + kLevel);              // lfs f31,60(r29)
  const double rate = LoadSingle(base, format + kFormatRate);        // lfs f13,12(r3)
  double scaled = double(float(rate * gain));                        // fmuls f0,f13,f0
  const double half = LoadSingle(base, kRoundHalf);                  // lfs f13,-26788(r11)
  // fcmpu cr6,f0,f30 ; blt cr6,0x82b22378 -- round half away from zero, then truncate.
  if (scaled < zero) {
    ctx.fpscr.disableFlushModeUnconditional();
    scaled = double(float(scaled - half));  // fsubs f0,f0,f13
  } else {
    scaled = double(float(scaled + half));  // fadds f0,f0,f13
  }
  const uint32_t length = FctiwzLowWord(scaled);  // fctiwz ; stfd ; lwz r28,84(r1)
  const int32_t length_s = int32_t(length);

  // loc_82B22388 -- lwz r11,64(r29) ; cmplwi cr6,r11,1 ; blt -> start ; beq -> run ;
  // cmplwi cr6,r11,3 ; bge -> tail ; otherwise drain (2).
  const uint32_t state = REX_LOAD_U32(self + kState);
  if (state == 2) {
    // cmpwi cr6,r28,0 ; bgt cr6,0x82b223b4
    if (!(length_s > 0)) {
      REX_STORE_U32(self + kState, 0);  // li r11,0 ; stw r11,64(r29)
    } else if (!(length_s > BlockLimit(base, block))) {  // loc_82B223C8: cmpw r28,r11 ; bgt tail
      const double clamped = ClampLevel(ctx, base, level, zero);
      // loc_82B22404
      ctx.fpscr.disableFlushModeUnconditional();
      const double previous = LoadSingle(base, mixer + kMixLevel);  // lfs f13,16(r30)
      StoreSingle(base, mixer + kMixLevel, clamped);                // stfs f0,16(r30)
      StoreSingle(base, mixer + kMixLevelPrev, previous);           // stfs f13,20(r30)
      const uint32_t last = REX_LOAD_U32(block + kObjLength);       // lwz r11,40(r31)
      // cmpw cr6,r28,r11 ; beq cr6,0x82b22544
      if (length_s != int32_t(last)) {
        REX_STORE_U32(block + kObjLengthPrev, last);  // stw r11,44(r31)
        REX_STORE_U8(block + kObjTwoBuffers, 1);      // stb r10,56(r31)
        REX_STORE_U32(block + kObjLength, length);    // loc_82B22540: stw r28,40(r31)
      }
      REX_STORE_U32(self + kState, 1);  // loc_82B22544: stw r10,64(r29)
    }
  } else if (state == 1) {
    // loc_82B2242C -- cmpwi cr6,r28,0 ; bgt cr6,0x82b22440
    if (!(length_s > 0)) {
      REX_STORE_U32(self + kState, 0);  // li r11,0 ; stw r11,64(r29)
    } else if (BlockLimit(base, block) < length_s) {  // loc_82B22454: cmpw r11,r28 ; bge
      REX_STORE_U32(self + kState, 2);  // li r11,2 ; stw r11,64(r29)
    } else {
      // loc_82B22468
      const double clamped = ClampLevel(ctx, base, level, zero);
      // loc_82B2249C
      ctx.fpscr.disableFlushModeUnconditional();
      const double previous = LoadSingle(base, mixer + kMixLevel);  // lfs f13,16(r30)
      StoreSingle(base, mixer + kMixLevel, clamped);                // stfs f0,16(r30)
      StoreSingle(base, mixer + kMixLevelPrev, previous);           // stfs f13,20(r30)
      const uint32_t last = REX_LOAD_U32(block + kObjLength);       // lwz r11,40(r31)
      // cmpw cr6,r28,r11 ; beq cr6,0x82b22548 -- no state store on this arm
      if (length_s != int32_t(last)) {
        REX_STORE_U32(block + kObjLengthPrev, last);  // stw r11,44(r31)
        REX_STORE_U32(block + kObjLength, length);    // stw r28,40(r31)
        REX_STORE_U8(block + kObjTwoBuffers, 1);      // li r10,1 ; stb r10,56(r31)
      }
    }
  } else if (state == 0) {
    // loc_82B224C8 -- cmpwi cr6,r28,0 ; ble cr6,0x82b22548
    if (length_s > 0) {
      // lwz r11,4(r30) ; mr r3,r30 ; mtctr r11 ; bctrl -- the pointer is RELOADED from +80, where
      // this call stored sub_82B3D578 (which zeroes mixer+16 and +20) on entry.
      ctx.r3.u64 = mixer64;
      ctx.ctr.u64 = REX_LOAD_U32(mixer + kResetFn);
      ctx.lr = 0x82B224E0;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

      const double clamped = ClampLevel(ctx, base, level, zero);
      // loc_82B22514 -- +20 is stored BEFORE +16 on this arm, and +16 is a genuine reload of
      // what the reset entry just wrote.
      ctx.fpscr.disableFlushModeUnconditional();
      const double previous = LoadSingle(base, mixer + kMixLevel);   // lfs f13,16(r30)
      StoreSingle(base, mixer + kMixLevelPrev, previous);            // stfs f13,20(r30)
      StoreSingle(base, mixer + kMixLevel, clamped);                 // stfs f0,16(r30)
      const uint32_t span_low = REX_LOAD_U32(block + kObjSpanLow);   // lwz r8,24(r31)
      REX_STORE_U32(block + kObjCursor, 0);                          // stw r11,36(r31)
      REX_STORE_U8(block + kObjTwoBuffers, 0);                       // stb r11,56(r31)
      REX_STORE_U32(block + kObjPosition, span_low);                 // stw r8,52(r31)
      const uint32_t span_high = REX_LOAD_U32(block + kObjSpanHigh); // lwz r9,20(r31)
      REX_STORE_U32(block + kObjSecondCursor, span_high);            // stw r9,32(r31)
      REX_STORE_U32(block + kObjLength, length);                     // loc_82B22540: stw r28,40(r31)
      REX_STORE_U32(self + kState, 1);                               // loc_82B22544: stw r10,64(r29)
    }
  }
  // state >= 3: cmplwi cr6,r11,3 ; bge cr6,0x82b22548 -- straight to the tail.

  // loc_82B22548 -- the state is RELOADED: every arm above may have rewritten it.
  // lwz r11,64(r29) ; cmpwi cr6,r11,0 ; beq cr6,0x82b22578
  if (REX_LOAD_U32(self + kState) != 0) {
    const uint32_t out_buffer = REX_LOAD_U32(stream + kStreamOut);  // lwz r5,32(r27)
    const uint32_t in_buffer = REX_LOAD_U32(stream + kStreamIn);    // lwz r4,28(r27)
    ctx.lr = 0x82B22568;                                            // bl 0x82b3d8e0
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, block64, in_buffer, out_buffer, 0);
    // lwz r11,32(r27) ; lwz r10,28(r27) ; stw r11,28(r27) ; stw r10,32(r27) -- both reloaded.
    const uint32_t second = REX_LOAD_U32(stream + kStreamOut);
    const uint32_t first = REX_LOAD_U32(stream + kStreamIn);
    REX_STORE_U32(stream + kStreamIn, second);
    REX_STORE_U32(stream + kStreamOut, first);
  }

  // loc_82B22578 -- mr r3,r29 ; bl 0x82b225a0
  ctx.lr = 0x82B22580;
  skate3::audio::GuestCall(ctx, base, sub_82B225A0, self64);

  // lwz r11,8(r29) ; li r3,1 ; lwz r10,0(r11) ; stw r26,32(r10) -- owner and arena reloaded.
  const uint32_t owner_again = REX_LOAD_U32(self + kOwner);
  ctx.r3.s64 = 1;
  const uint32_t arena_again = REX_LOAD_U32(owner_again + 0);
  REX_STORE_U32(arena_again + kArenaTop, uint32_t(saved_top));

  // Epilogue: addi r1,r1,160 ; lfd f30 ; lfd f31 ; b __restgprlr_26. The flush-mode point on the
  // lfd is the state this call leaves for its caller. r12 and lr come back as the zero-extended
  // low word __savegprlr_26 spilled with `stw r12,-8(r1)`.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.r12.u64 = uint32_t(entry_lr);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;              // mtlr r12
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B222D8
SKATE3_PORT(82B222D8, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
