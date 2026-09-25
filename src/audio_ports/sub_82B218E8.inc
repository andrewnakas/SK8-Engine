// sub_82B218E8  role: start the Dac profiler worker -- when the run-state word is clear, arm the global byte, and for mode 0 drive the sink at +76 through its vtable slot +48 with 1.0f before publishing run-state 1 (mode 1 publishes 2, any other mode publishes nothing)   lifted: skate3_recomp.67.cpp:9086 (84 lines)
// STATUS: partial -- compared 0, skipped 1, zero divergence (session g1_splits, 150 s scripted); armed and waiting: the one call took the calling path. NEVER promoted: the skipped path has a native body but no comparison. (comparable unless the run state is zero AND the mode is 0 AND the sink at +76 is non-null -- only that combination reaches the `bctrl` at 0x82B21944; the already-running, null-sink, mode-1 and other-mode paths all call nothing, and three of the four write the armed byte and the run state)
// calls/session: boot 1  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B218E8.md
namespace port_82B218E8 {

// All three computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye.
constexpr uint32_t kRunState = 0x83080000u + 17868u;  // lis r31,-31992 ; lwz r11,17868(r31)
constexpr uint32_t kArmedByte = 0x83060000u + 28759u; // lis r10,-31994 ; stb r11,28759(r10)
constexpr uint32_t kOneFloat = 0x82320000u - 22460u;  // lis r11,-32206 ; lfs f1,-22460(r11)
                                                      // 0x8231A844, 1.0f per docs/ports notes

constexpr uint32_t kSink = 76;       // lwz r11,76(r3)
constexpr uint32_t kSinkSlot = 48;   // lwz r9,48(r10) -- the entry that takes one float

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const int32_t mode = ctx.r4.s32;  // cmpwi: signed compares against 0 and 1

  // lwz r11,17868(r31) ; cmpwi cr6,r11,0 ; bne cr6,0x82b21960
  // Non-zero means already running: no stores at all, r3 and r4 left as they arrived.
  if (REX_LOAD_U32(kRunState) != 0) return;

  // stb r11,28759(r10) -- armed before either branch decides, and never undone.
  REX_STORE_U8(kArmedByte, 1);

  uint32_t state;
  // cmpwi cr6,r4,0 ; bne cr6,0x82b21950
  if (mode == 0) {
    const uint32_t sink = REX_LOAD_U32(object + kSink);  // lwz r11,76(r3)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b21948
    if (sink != 0) {
      ctx.r3.u64 = sink;  // rotlwi r3,r11,0 -- a zero-extending move into the argument
      ctx.r5.s64 = 0;     // li r5,0
      const uint32_t vtable = REX_LOAD_U32(sink + 0);  // lwz r10,0(r3)
      // lfs f1,-22460(r11) -- between the vtable load and the slot load, as lifted.
      ctx.fpscr.disableFlushModeUnconditional();
      PPCRegister temp{};
      temp.u32 = REX_LOAD_U32(kOneFloat);
      ctx.f1.f64 = double(temp.f32);
      ctx.ctr.u64 = REX_LOAD_U32(vtable + kSinkSlot);  // lwz r9,48(r10) ; mtctr r9
      ctx.lr = 0x82B21948;                             // bctrl
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    }
    state = 1;  // li r11,1 -- reached from both arms of the mode-0 path
  } else if (mode == 1) {
    // cmpwi cr6,r4,1 ; bne cr6,0x82b21960
    state = 2;  // li r11,2
  } else {
    // Any other mode falls through to the epilogue with the armed byte set and the run state
    // still zero, so the next call arms it again and re-runs. Reproduced, not fixed.
    return;
  }

  // loc_82B2195C
  REX_STORE_U32(kRunState, state);  // stw r11,17868(r31)
}

// Split by path. Three of the four store shapes here call nothing at all, and the one `bctrl`
// needs a specific entry state: run state zero, mode 0, and a non-null sink. Every term is a fixed
// global or an entry register, so the predicate is exact.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  spec.read(kRunState, 4);
  // lwz r11,17868(r31) ; cmpwi ; bne cr6,0x82b21960 -- already running: no stores at all, and r3
  // and r4 are left exactly as they arrived.
  if (REX_LOAD_U32(kRunState) != 0) return true;

  // stb r11,28759(r10) is issued before either mode branch, and the sink pointer is read after it.
  // If the armed byte were inside that word the entry read would not be the value the body reads,
  // so decline instead of assuming the two globals are disjoint from the object.
  const uint32_t sink_ea = object + kSink;
  if (kArmedByte >= sink_ea && kArmedByte < static_cast<uint64_t>(sink_ea) + 4u) return false;

  // cmpwi cr6,r4,0 ; lwz r11,76(r3) ; cmplwi ; beq -- mode 0 with a live sink drives slot +48 of
  // the sink's vtable with f1 = 1.0f. The target is read from memory the harness rewinds and the
  // effect (a rate or gain setter) is not memory at all, so that one combination is not replayable.
  if (ctx.r4.s32 == 0 && REX_LOAD_U32(sink_ea) != 0) return false;

  spec.read(sink_ea, 4);
  spec.write(kArmedByte, 1);  // stb r11,28759(r10) -- on every zero-run-state path
  // stw r11,17868(r31) -- 1 on the mode-0 path, 2 on mode 1, and NOT written when the mode is
  // neither; declared as the union, which costs nothing since an unwritten window compares equal.
  spec.write(kRunState, 4);
  return true;
}

}  // namespace port_82B218E8
SKATE3_PORT(82B218E8, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
