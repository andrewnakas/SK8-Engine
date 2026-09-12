// sub_82B23828  role: publish a command's value onto its target, one of two field sets   lifted: skate3_recomp.67.cpp:14026 (77 lines)
// STATUS: verified
// calls/session: boot 1910  play 1495  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B23828.md
namespace port_82B23828 {

// lis r9,-32208 ; addi r8,r9,-31232 ; lfd f13,256(r8) -- 0x82300000 - 0x7A00 + 0x100.
constexpr uint32_t kSentinelDoubleCell = 0x822F8700;
// lis r9,-32234 ; lfs f13,23056(r9) -- the same 0.0f cell the queue producer reads.
constexpr uint32_t kZeroFloatCell = 0x82165A10;
constexpr uint32_t kRecordTarget = 4;
constexpr uint32_t kRecordDouble = 8;
constexpr uint32_t kRecordFloatA = 16;
constexpr uint32_t kRecordFloatB = 20;
constexpr uint32_t kRecordWord = 24;
constexpr uint32_t kRecordSize = 32;  // returned in r3 on both paths

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

REX_FUNC(Native) {
  const uint32_t record = ctx.r3.u32;
  ctx.fpscr.disableFlushModeUnconditional();  // emitted before the lfd
  PPCRegister loaded{};
  loaded.u64 = REX_LOAD_U64(record + kRecordDouble);  // lfd f0,8(r3)
  const double value = loaded.f64;
  const uint32_t target = REX_LOAD_U32(record + kRecordTarget);  // lwz r11,4(r3)
  PPCRegister sentinel{};
  sentinel.u64 = REX_LOAD_U64(kSentinelDoubleCell);  // lfd f13,256(r8)
  ctx.r3.s64 = kRecordSize;                         // li r3,32 on both paths
  // fcmpu ; bne -- an unordered compare, so a NaN in either operand takes the second path.
  const bool matches_sentinel = value == sentinel.f64;
  if (matches_sentinel &&
      LoadSingle(base, record + kRecordFloatA) == LoadSingle(base, kZeroFloatCell)) {
    // The "clear" form: publish the second float and zero both flag bytes.
    StoreSingle(base, target + 108, LoadSingle(base, record + kRecordFloatB));  // stfs f0,108
    REX_STORE_U8(target + 113, 0);                                             // stb r9,113
    REX_STORE_U8(target + 112, 0);                                             // stb r9,112
    return;
  }
  // The "set" form: publish the double, both floats, the word, and mark the flag byte.
  ctx.fpscr.disableFlushModeUnconditional();  // emitted again before the stfd
  REX_STORE_U64(target + 56, loaded.u64);                                       // stfd f0,56
  StoreSingle(base, target + 64, LoadSingle(base, record + kRecordFloatA));     // stfs f0,64
  StoreSingle(base, target + 68, LoadSingle(base, record + kRecordFloatB));     // stfs f13,68
  REX_STORE_U32(target + 72, REX_LOAD_U32(record + kRecordWord));               // stw r8,72
  REX_STORE_U8(target + 112, 1);                                                // stb r9,112
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t target = REX_LOAD_U32(record + kRecordTarget);
  if (target == 0) return false;  // the original would store through null
  // The union of both paths, 30 bytes. A byte this call does not write compares equal anyway,
  // so covering both is exact and avoids re-deciding the branch here.
  spec.write(target + 56, 24);   // the double at +56, floats at +64/+68, word at +72
  spec.write(target + 108, 4);
  spec.write(target + 112, 2);
  spec.read(record, kRecordSize);
  spec.read(kSentinelDoubleCell, 8);
  spec.read(kZeroFloatCell, 4);
  return true;
}

}  // namespace port_82B23828
SKATE3_PORT(82B23828, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
