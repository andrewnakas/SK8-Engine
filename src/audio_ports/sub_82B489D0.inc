// sub_82B489D0  role: detach a scheduler instance -- unlink its node now, or park a pending removal when it is the one the tick is running   lifted: skate3_recomp.68.cpp:37283 (71 lines)
// STATUS: verified
// calls/session: boot 1997  play 2214  map ?          thread: MoviePlayer2 De
// note: probe/ports/notes/sub_82B489D0.md
namespace port_82B489D0 {

// r3 = rw_system; the scheduler lives at +0x70 (docs/rw_audio_structs.h).
constexpr uint32_t kScheduler = 112;          // addi r11,r3,112
constexpr uint32_t kSchedCurrent = 68;        // +0x44 rw_scheduler.current_node -- read as r3+180
constexpr uint32_t kSchedPendingBucket = 72;  // +0x48, the _pad48 word sub_82B48A50 scales by 32
constexpr uint32_t kSchedNodeRemoved = 76;    // +0x4C rw_scheduler.node_removed
constexpr uint32_t kBucketStride = 32;        // rlwinm r10,r10,5,0,26

// r4 = rw_instance. docs/rw_audio_structs.h calls +0 `descriptor`; what this function does with it
// is follow it to a node and clear the node's back pointer, so it is named for that here.
constexpr uint32_t kInstanceNode = 0;
constexpr uint32_t kInstanceElapsed = 16;  // +0x10 rw_instance.elapsed
constexpr uint32_t kInstanceBucket = 20;   // +0x14 u8 -- which scheduler bucket the node sits in
constexpr uint32_t kDetached = 3;          // li r11,3; also the bucket value meaning "not linked"

// rw_node, as sub_82B39690 addresses it.
constexpr uint32_t kNodeLinkA = 0;
constexpr uint32_t kNodeLinkB = 4;
constexpr uint32_t kNodeInstance = 8;  // +0x08 the back pointer to the instance
constexpr uint32_t kNodeWhich = 12;    // u8 -- selects which of the two heads can name the node

// The bucket record sub_82B39690 manages, at scheduler + 32*bucket.
constexpr uint32_t kBucketFreeHead = 12;
constexpr uint32_t kBucketHeadA = 16;
constexpr uint32_t kBucketHeadB = 20;
constexpr uint32_t kBucketCount = 24;

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t system = ctx.r3.u32;
  const uint32_t instance = ctx.r4.u32;  // mr r9,r4
  // addi r11,r3,112 -- kept 64-bit, because it is one operand of the 64-bit `add` that forms
  // sub_82B39690's first argument, so the high half of the entry r3 has to survive.
  const uint64_t scheduler_reg = uint64_t(ctx.r3.s64 + kScheduler);
  const uint32_t scheduler = uint32_t(scheduler_reg);

  const uint32_t current = REX_LOAD_U32(system + kScheduler + kSchedCurrent);  // lwz r10,180(r3)
  const uint32_t bucket = REX_LOAD_U8(instance + kInstanceBucket);            // lbz r10,20(r4)

  // cmplw cr6,r4,r10 ; bne 0x82b48a10 -- fall through only when this instance is the one the
  // scheduler tick is running right now.
  if (instance == current) {
    // The unlink cannot happen while sub_82B48A50 walks the list, so the bucket index and the
    // node are parked for it to finish after the process fn returns. No bucket test here: the
    // running instance takes this path whatever its bucket byte says.
    REX_STORE_U32(scheduler + kSchedPendingBucket, bucket);        // stw r10,72(r11)
    // lwz r7,0(r4) -- loaded AFTER the store above, and reloaded rather than hoisted.
    const uint32_t node = REX_LOAD_U32(instance + kInstanceNode);
    REX_STORE_U32(instance + kInstanceNode, 0);          // stw r8,0(r4)
    REX_STORE_U32(node + kNodeInstance, 0);              // stw r8,8(r7)
    REX_STORE_U32(scheduler + kSchedNodeRemoved, node);  // stw r7,76(r11)
  } else if (bucket != kDetached) {  // cmplwi cr6,r10,3 ; beq 0x82b48a30
    const uint32_t node = REX_LOAD_U32(instance + kInstanceNode);  // lwz r4,0(r9)
    // rlwinm r10,r10,5,0,26 on a zero-extended byte: bucket*32, the low word masked to ~31.
    const uint64_t bucket_base = (uint64_t(bucket) << 5) & 0xFFFFFFE0u;
    REX_STORE_U32(instance + kInstanceNode, 0);  // stw r8,0(r9)
    REX_STORE_U32(node + kNodeInstance, 0);      // stw r8,8(r4)
    // add r3,r10,r11 ; bl 0x82b39690 -- unlink the node from its bucket and push it on the bucket's
    // free list. 64-bit add, so the argument carries the entry r3's high half.
    skate3::audio::GuestCall(ctx, base, sub_82B39690, bucket_base + scheduler_reg, uint64_t(node));
  }

  // loc_82B48A30 -- the tail runs on all three paths.
  REX_STORE_U32(instance + kInstanceElapsed, 0);        // stw r8,16(r9)
  REX_STORE_U8(instance + kInstanceBucket, uint8_t(kDetached));  // stb r11,20(r9)
}

// Which of the three paths runs is decided entirely on entry state: the instance pointer against
// rw_scheduler.current_node, then the instance's bucket byte against 3. Both are loaded before the
// first store in the original too, so the prediction is exact rather than a union.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t system = ctx.r3.u32;
  const uint32_t instance = ctx.r4.u32;
  if (system == 0 || instance == 0) return false;
  const uint32_t scheduler = system + kScheduler;
  const uint32_t current = REX_LOAD_U32(scheduler + kSchedCurrent);
  const uint32_t bucket = REX_LOAD_U8(instance + kInstanceBucket);

  spec.read(scheduler + kSchedCurrent, 4);
  spec.read(instance + kInstanceNode, 4);
  spec.read(instance + kInstanceBucket, 1);

  // The tail: elapsed at +16 then the bucket byte at +20, one span of five bytes.
  spec.write(instance + kInstanceElapsed, 5);

  if (instance == current) {
    spec.write(scheduler + kSchedPendingBucket, 8);  // 72 and 76 are adjacent words
    // `lwz r7,0(r4)` runs after that store. If the store lands on the node pointer, the node
    // address is produced during the call and is not entry state: the gate-2 exit.
    if (Overlaps(scheduler + kSchedPendingBucket, 8, instance + kInstanceNode, 4)) return false;
    const uint32_t node = REX_LOAD_U32(instance + kInstanceNode);
    if (node == 0) return false;  // the original would store to guest address 8
    spec.write(instance + kInstanceNode, 4);
    spec.write(node + kNodeInstance, 4);
    return true;
  }
  if (bucket == kDetached) return true;  // nothing but the tail is written

  const uint32_t node = REX_LOAD_U32(instance + kInstanceNode);
  if (node == 0) return false;  // stw r8,8(r4) through null, then sub_82B39690 off null
  spec.write(instance + kInstanceNode, 4);
  spec.write(node + kNodeInstance, 4);

  // sub_82B39690's own write set, on manager = scheduler + 32*bucket and this node. Re-derived
  // here from its lifted form (and matching its verified port) rather than assumed: unlink from
  // one of two heads, fix both neighbours, rewrite the node's two links, push onto the free list,
  // decrement the count.
  const uint32_t manager = scheduler + bucket * kBucketStride;
  const uint32_t free_field = manager + kBucketFreeHead;
  // Its own gate-2 guard: the free head is re-read after the node's two link words are written.
  if (Overlaps(free_field, 4, node + kNodeLinkA, 8)) return false;

  // The two stores this function makes BEFORE the call must not land on anything the callee
  // addresses from entry state, or the callee's write set stops being derivable from entry.
  const uint32_t reread[6][2] = {{node + kNodeLinkA, 4},  {node + kNodeLinkB, 4},
                                 {node + kNodeWhich, 1},  {free_field, 4},
                                 {manager + kBucketHeadA, 4}, {manager + kBucketHeadB, 4}};
  const uint32_t pre[2] = {instance + kInstanceNode, node + kNodeInstance};
  for (uint32_t w = 0; w < 2; w++) {
    for (uint32_t k = 0; k < 6; k++) {
      if (Overlaps(pre[w], 4, reread[k][0], reread[k][1])) return false;
    }
  }

  const uint32_t which = REX_LOAD_U8(node + kNodeWhich);              // lbz r11,12(r4)
  const uint32_t head_field = which != 0 ? kBucketHeadA : kBucketHeadB;
  const uint32_t head = REX_LOAD_U32(manager + head_field);
  if (node == head) spec.write(manager + head_field, 4);
  const uint32_t next = REX_LOAD_U32(node + kNodeLinkB);
  const uint32_t prev = REX_LOAD_U32(node + kNodeLinkA);
  if (int32_t(next) != 0) spec.write(next + kNodeLinkA, 4);
  if (int32_t(prev) != 0) spec.write(prev + kNodeLinkB, 4);
  spec.write(node + kNodeLinkA, 8);
  const uint32_t free_head = REX_LOAD_U32(free_field);
  if (free_head != 0) spec.write(free_head + kNodeLinkB, 4);
  spec.write(free_field, 4);
  spec.write(manager + kBucketCount, 4);

  spec.read(free_field, 16);  // the free head and both bucket heads, plus the count
  spec.read(node, 16);
  if (node == head && head != 0) spec.read(head + kNodeLinkA, 4);
  return true;
}

}  // namespace port_82B489D0
SKATE3_PORT(82B489D0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
