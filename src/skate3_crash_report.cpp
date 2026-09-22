// See the header for why this exists.

#include "skate3_crash_report.h"
#include "skate3_guest_trace.h"
#include "skate3_native_render.h"

#include <rex/thread/timer_queue.h>

// Timer delivery counters, defined in the runtime's Switch threading and timer
// queue. Declared here rather than in a shared header: those headers reach most
// of the tree and a change to one costs a twenty minute rebuild.
extern "C" {
extern std::atomic<uint64_t> rex_diag_timer_setonce;
extern std::atomic<uint64_t> rex_diag_timer_setonce_armed;
extern std::atomic<uint64_t> rex_diag_timer_completion;
extern std::atomic<uint64_t> rex_diag_timer_signal;
extern std::atomic<uint64_t> rex_diag_disarm_blocked;
extern std::atomic<uint64_t> rex_diag_disarm_woke;
extern std::atomic<uint64_t> rex_diag_timer_cancel;
extern std::atomic<uint64_t> rex_diag_tq_dropped;
extern std::atomic<uint32_t> rex_diag_tq_drop_state;
extern std::atomic<int64_t> rex_diag_timer_last_due_ms;
extern std::atomic<int64_t> rex_diag_timer_max_due_ms;
extern std::atomic<uint64_t> rex_diag_shmem_upload_pages;
extern std::atomic<uint64_t> rex_diag_shmem_upload_calls;
extern std::atomic<uint32_t> rex_diag_shmem_page_size;
}

#include "skate3_native_scene.h"

// For the guest X_KTHREAD pointer, which is the number
// RtlEnterCriticalSection reports as owner_thread=.
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

#if !defined(_WIN32)

#include <dirent.h>
#if defined(__ANDROID__) || defined(__SWITCH__)
// Neither bionic nor newlib has <execinfo.h>; this supplies backtrace* over
// the unwinder for both.
#include <rex/execinfo_android.h>
#else
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#if defined(__APPLE__)
// mach_vm.h is macOS-only; the vm_* entry points below are the ones the iOS
// SDK exposes, and they are equivalent for same-task reads.
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread/pthread.h>
// write() and STDERR_FILENO, used by the async-signal-safe fault path. Every
// other arm of this chain includes it; Apple got it transitively from the
// macOS SDK's headers and not from the iOS SDK's, so it only surfaced the
// first time this was built for a phone.
#include <unistd.h>
#elif defined(__SWITCH__)
// No prctl and no syscall table: thread names live on the thread object here,
// and the reporter reads them from the kernel state rather than from the OS.
#include <unistd.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif
#else
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#if !defined(__APPLE__)
// Darwin has no <malloc.h> - the equivalent is <malloc/malloc.h>, and it does
// not declare mallinfo() at all. The only caller here is the Horizon heartbeat
// below, which is already inside #if defined(__SWITCH__), so excluding Apple
// costs nothing. Added unguarded by c071953 while bringing up the Switch port,
// which broke the macOS and iOS builds silently: macOS had not rebuilt this
// translation unit since, and iOS had not been built at all.
#include <malloc.h>
#endif
#include <vector>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <skate3_version.h>

#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

REXCVAR_DECLARE(int32_t, skate3_hang_watchdog_seconds);
REXCVAR_DECLARE(int32_t, skate3_stall_watchdog_seconds);

namespace skate3::crash_report {

namespace {

// Span of the runtime's guest views measured from the virtual membase: 4 GB of
// virtual address space plus the 512 MB physical-raw view mapped above it
// (same constant as the read-recovery handler in native/skate3_native_guest_read.cpp).
constexpr uint64_t kGuestSpanBytes = 0x120000000ull;

uint8_t* g_guest_base = nullptr;
int g_crash_fd = -1;

// Everything below runs in signal context: no allocation, no locks, no
// spdlog. A fixed stack buffer plus write(2) only.
class Report {
 public:
  void Str(const char* s) {
    while (*s && len_ < kCap - 1) {
      buf_[len_++] = *s++;
    }
  }
  void Hex(uint64_t v, int digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0 && len_ < kCap - 1; --i) {
      buf_[len_++] = kDigits[(v >> (i * 4)) & 0xF];
    }
  }
  void Dec(uint64_t v) {
    char tmp[24];
    int n = 0;
    do {
      tmp[n++] = char('0' + (v % 10));
      v /= 10;
    } while (v && n < int(sizeof(tmp)));
    while (n-- > 0 && len_ < kCap - 1) {
      buf_[len_++] = tmp[n];
    }
  }
  // "  r26=00000000_400053B4" - the u64 split so a 32-bit guest pointer reads
  // straight off the low half.
  void Reg(const char* name, uint64_t v) {
    Str("  ");
    Str(name);
    Str("=");
    Hex(v >> 32, 8);
    Str("_");
    Hex(v & 0xFFFFFFFFull, 8);
  }
  void Flush() {
    if (len_ == 0) {
      return;
    }
    // Best effort on both sinks; a short write here is not worth a retry loop.
    (void)!::write(STDERR_FILENO, buf_, len_);
    if (g_crash_fd >= 0) {
      (void)!::write(g_crash_fd, buf_, len_);
    }
    len_ = 0;
  }

 private:
  static constexpr size_t kCap = 4096;
  char buf_[kCap];
  size_t len_ = 0;
};

// Thread identity, signal-safe on both OSes: Linux names threads through
// prctl and tids through a syscall; Darwin has no prctl or gettid and exposes
// both through pthread_np calls instead.
uint64_t HostTid() {
#if defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return tid;
#elif defined(__SWITCH__)
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);
  return tid;
#else
  return uint64_t(syscall(SYS_gettid));
#endif
}

bool HostThreadName(char* name, size_t len) {
#if defined(__APPLE__)
  return pthread_getname_np(pthread_self(), name, len) == 0 && name[0] != 0;
#elif defined(__SWITCH__)
  // Horizon threads carry no name the OS will hand back; the engine keeps its
  // own on the thread object. Reporting failure lets the caller fall back to
  // that rather than print something invented here.
  (void)name;
  (void)len;
  return false;
#else
  (void)len;
  return prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0) == 0;
#endif
}

// Host backtrace. Not every fault is guest code - a null HOST pointer
// dereferenced on a guest-named thread looks identical in the register dump,
// and only the host frames tell the two apart. backtrace_symbols_fd is the
// signal-safe printer (it formats into a stack buffer and writes; no malloc,
// unlike backtrace_symbols). Without -g the frames print as
// "binary(+0xoffset)", which is enough: resolve with
//   addr2line -f -e out/build/linux-release/skate3 <offset>
// or `nm -C` for the nearest symbol.
void WriteHostBacktrace() {
  void* frames[32];
  const int n = ::backtrace(frames, 32);
  if (n <= 0) {
    return;
  }
  static const char kHdr[] = "  host backtrace:\n";
  (void)!::write(STDERR_FILENO, kHdr, sizeof(kHdr) - 1);
  ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
  if (g_crash_fd >= 0) {
    (void)!::write(g_crash_fd, kHdr, sizeof(kHdr) - 1);
    ::backtrace_symbols_fd(frames, n, g_crash_fd);
  }
}

// Shared tail of every report: who was running, what the guest machine looked
// like, and the host frames. A null ThreadState is itself the answer to "was
// this guest code?".
void WriteGuestState(Report& r) {
  char name[20] = {0};
  if (HostThreadName(name, sizeof(name))) {
    r.Str("  thread        ");
    r.Str(name);
    r.Str("\n");
  }
  r.Str("  host tid      ");
  r.Dec(HostTid());
  r.Str("\n");

  auto* ts = rex::runtime::ThreadState::Get();
  if (ts == nullptr || ts->context() == nullptr) {
    r.Str("  guest thread  NO (no bound ThreadState - host-side fault)\n");
    r.Str("=== end ===\n");
    r.Flush();
    WriteHostBacktrace();
    return;
  }

  const ::PPCContext& c = *ts->context();
  r.Str("  guest thid    ");
  r.Dec(ts->thread_id());
  // lr is the guest return address: the function that made the call that went
  // wrong. It is the single most useful number in this dump - look it up with
  //   grep -n 'DEFINE_REX_FUNC' generated/*.cpp
  // and take the nearest definition below it.
  r.Str("\n  guest lr      0x");
  r.Hex(c.lr, 8);
  r.Str("   <- caller; find the nearest DEFINE_REX_FUNC below this address\n");
  r.Str("  guest ctr     0x");
  r.Hex(c.ctr.u64 & 0xFFFFFFFFull, 8);
  r.Str("   <- indirect call target\n");
  r.Str("  guest regs:\n");

  const struct {
    const char* name;
    uint64_t value;
  } gprs[] = {
      {" r0", c.r0.u64},  {" r1", c.r1.u64},  {" r2", c.r2.u64},  {" r3", c.r3.u64},
      {" r4", c.r4.u64},  {" r5", c.r5.u64},  {" r6", c.r6.u64},  {" r7", c.r7.u64},
      {" r8", c.r8.u64},  {" r9", c.r9.u64},  {"r10", c.r10.u64}, {"r11", c.r11.u64},
      {"r12", c.r12.u64}, {"r13", c.r13.u64}, {"r14", c.r14.u64}, {"r15", c.r15.u64},
      {"r16", c.r16.u64}, {"r17", c.r17.u64}, {"r18", c.r18.u64}, {"r19", c.r19.u64},
      {"r20", c.r20.u64}, {"r21", c.r21.u64}, {"r22", c.r22.u64}, {"r23", c.r23.u64},
      {"r24", c.r24.u64}, {"r25", c.r25.u64}, {"r26", c.r26.u64}, {"r27", c.r27.u64},
      {"r28", c.r28.u64}, {"r29", c.r29.u64}, {"r30", c.r30.u64}, {"r31", c.r31.u64},
  };
  for (size_t i = 0; i < sizeof(gprs) / sizeof(gprs[0]); ++i) {
    r.Reg(gprs[i].name, gprs[i].value);
    // The register block is the bulk of the report; flush per line so a
    // truncated write still leaves something readable.
    if ((i % 4) == 3) {
      r.Str("\n");
      r.Flush();
    }
  }
  r.Str("=== end ===\n");
  r.Flush();
  WriteHostBacktrace();
}

// ---- Hang watchdog --------------------------------------------------------
// A freeze produces NO signal, so the fault reporter above never fires and the
// log just stops advancing - which is exactly the state the user sees and the
// one we had no data for. This detects it from the inside: the guest frame hook
// bumps a heartbeat, a watchdog thread notices when it stops, and then every
// thread in the process is asked to print its own stack.
//
// "Ask each thread" is the only way to get other threads' stacks in-process:
// backtrace() only ever walks the CALLING thread. So the watchdog signals each
// tid in /proc/self/task and each one dumps itself from the signal handler.
// SIGRTMIN carries no default meaning and is not used by the runtime.
std::atomic<uint64_t> g_heartbeat{0};
// Work the guest has submitted, as reported at the frame boundary. Distinct
// from the heartbeat: frames can keep arriving while this stands still.
std::atomic<uint64_t> g_guest_work{0};
std::atomic<bool> g_hang_reported{false};

// Report attribution. The crash file is append-only and survives across
// sessions and builds, so without these every block in it is anonymous.
// Pre-formatted at install time and ticked from the watchdog thread; the
// signal handlers only Str()/Dec() them.
char g_stamp[160] = "";
std::atomic<uint64_t> g_uptime_seconds{0};
std::atomic<uint64_t> g_epoch0{0};

// Which front-end screen was on top when this report was written. The value is
// a relaxed atomic the scene code already publishes every frame, so reading it
// from a signal handler is safe, and it is the one piece of context that turns
// a stranger's crash report from an address into a place. Every crash in the
// v2.5.0 field reports named a screen ("selecting a photo", "adding a second
// skater") and the dump could not confirm any of them.
const char* FrontEndScreenName(uint32_t id) {
  // Ids surveyed from captures; see PortraitRttWindowActive and
  // YieldForCasEditor in skate3_native_scene.cpp, which classify the same set.
  switch (id) {
    case skate3::native_scene::kFrontEndStackEmpty: return "none/gameplay";
    case 0:  return "FE root";
    case 1:  return "photo editor";
    case 11: return "photo editor";
    case 15: return "create-a-skater editor";
    case 17: return "pause challenge map";
    case 24: return "FMV";
    case 56: return "pause root";
    case 59: return "skate-reel browser";
    case 63: return "team screen";
    default: return "unclassified";
  }
}

void WriteReportStamp(Report& r) {
  r.Str("  ");
  r.Str(g_stamp);
  r.Str(" uptime_s=");
  r.Dec(g_uptime_seconds.load(std::memory_order_relaxed));
  r.Str(" epoch0=");
  r.Dec(g_epoch0.load(std::memory_order_relaxed));
  r.Str("\n");
  const uint32_t fe = skate3::native_scene::FrontEndTopScreen();
  r.Str("  fe screen     ");
  r.Str(FrontEndScreenName(fe));
  if (fe != skate3::native_scene::kFrontEndStackEmpty) {
    r.Str(" (id ");
    r.Dec(fe);
    r.Str(")");
  }
  r.Str("\n");
}

#if defined(__linux__)
void ThreadDumpHandler(int, siginfo_t*, void*) {
  Report r;
  char name[20] = {0};
  r.Str("  --- tid ");
  r.Dec(HostTid());
  if (HostThreadName(name, sizeof(name))) {
    r.Str(" (");
    r.Str(name);
    r.Str(")");
  }
  // Guest threads carry a bound ThreadState; naming the guest pc/lr next to the
  // host frames is what tells us whether a stuck thread is spinning in guest
  // code or blocked in the runtime.
  auto* ts = rex::runtime::ThreadState::Get();
  if (ts != nullptr && ts->context() != nullptr) {
    r.Str(" guest thid=");
    r.Dec(ts->thread_id());
    // The same number RtlEnterCriticalSection prints as owner_thread=, which
    // is the guest X_KTHREAD pointer - NOT the handle set_name embeds and not
    // thread_id. Without it a "waiting 16s for cs=... owner_thread=..."
    // warning names a thread that appears nowhere else in the report, and the
    // one thread worth looking at is the one that cannot be identified.
    // IsInThread() first: GetCurrentThread() asserts on a null binding, and an
    // assert inside a signal handler is not something to find out about here.
    // Both read the same thread_local pointer, so this is signal-safe.
    if (rex::system::XThread::IsInThread()) {
      r.Str(" guest_obj=0x");
      r.Hex(rex::system::XThread::GetCurrentThread()->guest_object(), 8);
    }
    r.Str(" lr=0x");
    r.Hex(ts->context()->lr, 8);
    r.Str(" r1=0x");
    r.Hex(ts->context()->r1.u64 & 0xFFFFFFFFull, 8);
  }
  r.Str("\n");
  r.Flush();
  WriteHostBacktrace();
}
#endif  // __linux__

void DumpAllThreads() {
  Report r;
  r.Str("\n=== skate3: HANG detected - no guest frame for the watchdog period ===\n");
  WriteReportStamp(r);
#if defined(__linux__)
  r.Str("  every thread's stack follows; the one holding the lock everyone else\n");
  r.Str("  is waiting on is the interesting one.\n");
  r.Flush();
  // Enumerating /proc/self/task from a normal thread (not signal context), so
  // opendir is fine here.
  DIR* d = ::opendir("/proc/self/task");
  if (d == nullptr) {
    return;
  }
  const pid_t self_tid = pid_t(syscall(SYS_gettid));
  const pid_t pid = ::getpid();
  while (struct dirent* e = ::readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') {
      continue;
    }
    const pid_t tid = pid_t(atoi(e->d_name));
    if (tid == self_tid) {
      continue;  // the watchdog's own stack says nothing
    }
    if (syscall(SYS_tgkill, pid, tid, SIGRTMIN) != 0) {
      continue;
    }
    // Let the target finish writing before signalling the next one, so the
    // dumps do not interleave into nonsense.
    struct timespec ts = {0, 30 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  ::closedir(d);
#elif defined(__APPLE__)
  // Darwin has neither SIGRTMIN nor /proc, and on a device there is no
  // `sample` to run from outside, so the stacks are collected directly: each
  // thread is suspended, its saved pc/lr read, and its frame-pointer chain
  // walked. AArch64 keeps a reliable fp chain ([fp] = caller fp,
  // [fp+8] = caller lr), which is what makes this practical without an
  // unwinder. Not signal context - the watchdog owns this thread - so
  // dladdr and mach calls are fair game.
  r.Str("  every thread's stack follows; the one holding the lock everyone else\n");
  r.Str("  is waiting on is the interesting one.\n");
  r.Flush();

  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t thread_count = 0;
  if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS) {
    Report fail;
    fail.Str("  (task_threads failed)\n");
    fail.Flush();
    return;
  }
  const thread_t self_thread = mach_thread_self();
  for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
    if (threads[i] == self_thread) {
      continue;  // the watchdog's own stack says nothing
    }
    // Suspending keeps the register state and stack coherent while they are
    // read; without it the walk chases a chain that is still moving.
    if (thread_suspend(threads[i]) != KERN_SUCCESS) {
      continue;
    }
    arm_thread_state64_t state = {};
    mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
    const kern_return_t got_state = thread_get_state(
        threads[i], ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &state_count);

    Report t;
    t.Str("  --- thread ");
    t.Dec(uint64_t(i));
    char name[64] = {0};
    if (pthread_t pt = pthread_from_mach_thread_np(threads[i])) {
      if (pthread_getname_np(pt, name, sizeof(name)) == 0 && name[0]) {
        t.Str(" (");
        t.Str(name);
        t.Str(")");
      }
    }
    if (got_state != KERN_SUCCESS) {
      t.Str(" <no state>\n");
      t.Flush();
      thread_resume(threads[i]);
      continue;
    }
    t.Str("\n");

    // pc and lr first: for a thread parked in a syscall the pc alone usually
    // names the wait, and lr names who asked for it.
    uint64_t pc = arm_thread_state64_get_pc(state);
    uint64_t fp = arm_thread_state64_get_fp(state);
    const uint64_t lr = arm_thread_state64_get_lr(state);
    for (int frame = 0; frame < 24; ++frame) {
      t.Str("      0x");
      t.Hex(pc, 16);
      Dl_info info = {};
      if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname) {
        t.Str("  ");
        t.Str(info.dli_sname);
      } else if (info.dli_fname) {
        t.Str("  ");
        t.Str(info.dli_fname);
      }
      t.Str("\n");
      t.Flush();

      if (fp == 0 || (fp & 7) != 0) {
        break;
      }
      // Read through mach_vm_read_overwrite rather than dereferencing: a
      // garbage fp in a corrupted frame would otherwise fault the watchdog
      // while it is reporting the original problem.
      uint64_t frame_data[2] = {0, 0};
      vm_size_t read_size = 0;
      if (vm_read_overwrite(mach_task_self(), vm_address_t(fp), sizeof(frame_data),
                            vm_address_t(&frame_data[0]), &read_size) != KERN_SUCCESS ||
          read_size != sizeof(frame_data)) {
        break;
      }
      const uint64_t next_fp = frame_data[0];
      const uint64_t next_pc = frame_data[1];
      if (next_pc == 0 || next_fp <= fp) {
        break;  // end of chain, or it stopped ascending: do not loop forever
      }
      // Strip pointer authentication bits before symbolizing.
      pc = next_pc & 0x0000007fffffffffull;
      fp = next_fp;
      if (frame == 0 && pc == 0) {
        pc = lr;
      }
    }
    thread_resume(threads[i]);
  }
  mach_port_deallocate(mach_task_self(), self_thread);
  vm_deallocate(mach_task_self(), vm_address_t(threads),
                vm_size_t(thread_count * sizeof(thread_t)));
#elif defined(__SWITCH__)
  // Horizon has neither signals nor a way to read another thread's registers
  // from inside the same process, so the two approaches above are both out: a
  // process cannot debug itself, and backtrace() only ever walks the calling
  // thread. What is still reachable is the guest side, which is the half that
  // matters for a hang - a deadlock here is guest threads waiting on each
  // other, and every one of them has a PPC context the runtime keeps updated.
  //
  // The contexts are read while their threads are running, so a value can be
  // caught mid-update. That is acceptable for a report whose purpose is to say
  // which thread is parked where; a torn register is obvious when it appears.
  r.Str("  guest threads and where each one is parked. lr names the caller:\n");
  r.Str("    grep -n 'DEFINE_REX_FUNC' generated/*.cpp and take the nearest\n");
  r.Str("    definition below the address.\n");
  r.Flush();

  auto* ks = rex::system::kernel_state();
  if (ks == nullptr) {
    r.Str("  no kernel state - the guest never started\n");
    r.Flush();
  } else {
    const std::vector<rex::system::object_ref<rex::system::XThread>> threads =
        ks->object_table()->GetObjectsByType<rex::system::XThread>();
    r.Str("  ");
    r.Dec(uint64_t(threads.size()));
    r.Str(" guest thread(s)\n");
    r.Flush();

    for (const auto& thread : threads) {
      if (!thread) {
        continue;
      }
      r.Str("\n  [");
      const std::string name = thread->thread_name();
      r.Str(name.empty() ? "(unnamed)" : name.c_str());
      r.Str("] thid=");
      r.Dec(thread->thread_id());
      r.Str(" guest_obj=0x");
      r.Hex(thread->guest_object(), 8);
      r.Str(thread->is_running() ? " running" : " NOT running");
      // Which cores this thread may actually run on, and at what priority.
      // Placement decides the frame rate on a three core machine and there was
      // no way to check it after the fact: the map says what was asked for,
      // this says what the kernel gave. Read through the raw handle rather than
      // Thread::affinity_mask(), which waits for the thread to have started and
      // would hang a report whose whole purpose is to describe a hang.
      if (auto* host = thread->thread()) {
        const Handle h = Handle(uintptr_t(host->native_handle()));
        s32 ideal = 0;
        u64 mask = 0;
        s32 prio = 0;
        if (R_SUCCEEDED(svcGetThreadCoreMask(&ideal, &mask, h))) {
          r.Str(" cores=0x");
          r.Hex(uint32_t(mask), 1);
          r.Str(" ideal=");
          r.Dec(uint64_t(uint32_t(ideal)));
        }
        if (R_SUCCEEDED(svcGetThreadPriority(&prio, h))) {
          r.Str(" prio=");
          r.Dec(uint64_t(uint32_t(prio)));
        }
      }
      r.Str("\n");

      auto* ts = thread->thread_state();
      const ::PPCContext* c = ts ? ts->context() : nullptr;
      if (c == nullptr) {
        r.Str("      no guest context bound\n");
        r.Flush();
        continue;
      }
      r.Str("      lr=0x");
      r.Hex(c->lr, 8);
      r.Str("  ctr=0x");
      r.Hex(c->ctr.u64 & 0xFFFFFFFFull, 8);
      r.Str("  r1=0x");
      r.Hex(c->r1.u64 & 0xFFFFFFFFull, 8);
      // r3 is the handle argument at every NtWaitForSingleObjectEx call site,
      // and "thread 6 is waiting on F8000054" is only useful once F8000054 has
      // a name. Resolving it says what kind of object the guest is parked on
      // and, where the title named it, which one.
      const uint32_t maybe_handle = uint32_t(c->r3.u64 & 0xFFFFFFFFull);
      if ((maybe_handle & 0xFF000000u) == 0xF8000000u) {
        auto object = ks->object_table()->LookupObject<rex::system::XObject>(maybe_handle);
        if (object) {
          static const char* kTypeNames[] = {
              "Undefined", "Enumerator", "Event",   "File",  "IOCompletion",
              "Module",    "Mutant",     "Notify",  "Semaphore", "Session",
              "Socket",    "SymLink",    "Thread",  "Timer"};
          const uint32_t type_index = uint32_t(object->type());
          r.Str("\n      waiting on ");
          r.Str(type_index < (sizeof(kTypeNames) / sizeof(kTypeNames[0])) ? kTypeNames[type_index]
                                                                          : "?");
          const std::string& object_name = object->name();
          if (!object_name.empty()) {
            r.Str(" '");
            r.Str(object_name.c_str());
            r.Str("'");
          }
        }
      }

      r.Str("\n      r3=0x");
      r.Hex(c->r3.u64 & 0xFFFFFFFFull, 8);
      r.Str("  r4=0x");
      r.Hex(c->r4.u64 & 0xFFFFFFFFull, 8);
      r.Str("  r5=0x");
      r.Hex(c->r5.u64 & 0xFFFFFFFFull, 8);
      r.Str("\n");
      r.Flush();

      // Guest call stack from the PowerPC back chain. Registers alone say every
      // thread is in NtWaitForSingleObjectEx, which is true and useless; what
      // matters is who called it.
      //
      // [r1] holds the caller's frame pointer, and a function saves its return
      // address into its CALLER's frame, at [caller + 4] - not into its own. So
      // the link for a frame is read one frame up the chain. Reading [r1 + 4]
      // instead lands on the slot this frame's own callee would use, which is
      // uninitialised here and comes back as stack fill.
      auto* memory = ks->memory();
      if (memory != nullptr && c->lr != 0) {
        r.Str("      guest stack:\n");
        r.Str("        0x");
        r.Hex(c->lr, 8);
        r.Str("\n");
        uint32_t frame = uint32_t(c->r1.u64 & 0xFFFFFFFFull);
        for (int depth = 0; depth < 16; ++depth) {
          if (frame == 0 || (frame & 3) != 0 || frame < 0x10000) {
            break;
          }
          auto* next_ptr = memory->TranslateVirtual<const uint32_t*>(frame);
          if (next_ptr == nullptr) {
            break;
          }
          const uint32_t next = __builtin_bswap32(*next_ptr);  // guest is big-endian
          // Stacks grow down, so the chain must ascend; anything else is a
          // corrupt or uninitialised frame and would loop forever.
          if (next <= frame || (next & 3) != 0) {
            break;
          }
          // Where the return address lives, from the recompiler's own output:
          // __savegprlr_N does "stw r12,-8(r1)" with r12 holding the link
          // register, and that store happens before "stwu" allocates the frame.
          // So the saved address sits 8 bytes below the frame this one chains
          // to. Functions that save the link inline use the SysV slot at +4
          // instead, so both are tried and whichever lands inside the title's
          // image is the real one.
          const uint32_t candidates[] = {next - 8, next + 4};
          for (uint32_t at : candidates) {
            auto* link_ptr = memory->TranslateVirtual<const uint32_t*>(at);
            if (link_ptr == nullptr) {
              continue;
            }
            const uint32_t link = __builtin_bswap32(*link_ptr);
            // The title is loaded at 0x82000000; anything outside it is fill or
            // a frame that was never written.
            if (link >= 0x82000000u && link < 0x84000000u) {
              r.Str("        0x");
              r.Hex(link, 8);
              r.Str("\n");
              break;
            }
          }
          frame = next;
        }
        r.Flush();
      }
    }
  }
#endif
  Report tail;
  tail.Str("=== end hang report ===\n");
  tail.Flush();
}

// How long the guest may take to produce its first frame before the watchdog
// calls it a hang. Boot to gameplay measures ~25s on an iPhone 13 mini.
#if defined(__SWITCH__)
// Shorter here while the port is being brought up. The watchdog only writes a
// report and lets the process carry on, so calling a slow load a hang costs
// nothing but a diagnostic, whereas waiting 75 seconds for every attempt costs
// a real one. Worth raising once the game boots.
constexpr int kPreFirstFrameGraceSeconds = 30;
#else
constexpr int kPreFirstFrameGraceSeconds = 75;
#endif

void WatchdogMain() {
  uint64_t last_seen = 0;
  int stalled_ticks = 0;
  uint64_t last_work = 0;
  int idle_ticks = 0;
  // Uptime comes from the clock, not from counting how many times we meant to
  // sleep for a second. The count was wrong because two watchdogs were running
  // and both incremented it (see StartWatchdog); asking the clock is right
  // regardless of how many threads ask, and does not drift if a sleep runs
  // long. Every rate derived from uptime was reported at half its true value
  // while this was broken, and the trace window and thread dumps fired at half
  // the uptime they were aimed at.
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    struct timespec ts = {1, 0};
    nanosleep(&ts, nullptr);
    const uint64_t uptime = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
    // The short sleep means this loop now runs about twice a second, so skip
    // the ticks where the second has not actually changed.
    if (uptime == g_uptime_seconds.load(std::memory_order_relaxed)) {
      continue;
    }
    g_uptime_seconds.store(uptime, std::memory_order_relaxed);

#if defined(__SWITCH__)
    // A periodic sign of life. The watchdog only speaks up when it decides
    // something is wrong, which leaves "booting slowly" and "stopped dead"
    // looking identical from the outside - and turning on debug logging to tell
    // them apart is slow enough to change the behaviour being measured. Frames
    // and guest work are the two counters that answer it directly.
    if ((uptime % 5) == 0) {
      // Memory belongs on this line: the process was killed outright with the
      // log still flowing - no fault, no terminate, no crash file - and running
      // out of a 3189 MB pool is the way that happens on Horizon. The guest
      // memory backend only reports at commit time, which is all in the first
      // few seconds and says nothing about a death eight minutes in.
      const struct mallinfo mi = mallinfo();
      // Fragmentation, not just size. Frame time on this port grows steadily
      // from the moment the front end appears, and an allocator walking an
      // ever-longer free list inside a 2 GB arena degrades exactly that way.
      // in-use vs free-chunk-count separates "we are simply out of room" from
      // "every allocation now costs a search".
      REXSYS_WARN("[heap] in-use={}MB free-chunks={} free={}MB mmapped={}MB",
                  size_t(mi.uordblks) >> 20, size_t(mi.ordblks), size_t(mi.fordblks) >> 20,
                  size_t(mi.hblkhd) >> 20);
      REXSYS_WARN("[progress] uptime={}s frames={} guest_work={} heap={}MB free={}MB", uptime,
                  g_heartbeat.load(std::memory_order_relaxed),
                  g_guest_work.load(std::memory_order_relaxed),
                  size_t(mi.arena) >> 20, size_t(mi.fordblks) >> 20);
      // Frames and guest work say the guest is running. They do not say it is
      // getting anywhere, and this title renders perfectly happily while its
      // screen manager waits for something that never arrives.
      skate3::native_render::LogScreenManagerState();
      skate3::native_render::LogFrameBudget();
      skate3::native_render::LogJobScan();
      // The guest's timer thread is what drains the queue the main loop needs
      // drained, so a timer that stops arriving stops the whole title. These
      // counters say whether the dispatch thread is alive, stuck inside a
      // callback, or unable to accept new timers at all.
      REXSYS_WARN("[timerd] set={} armed={} fired={} signalled={} disarm-blocked={} woke={}",
                  rex_diag_timer_setonce.load(std::memory_order_relaxed),
                  rex_diag_timer_setonce_armed.load(std::memory_order_relaxed),
                  rex_diag_timer_completion.load(std::memory_order_relaxed),
                  rex_diag_timer_signal.load(std::memory_order_relaxed),
                  rex_diag_disarm_blocked.load(std::memory_order_relaxed),
                  rex_diag_disarm_woke.load(std::memory_order_relaxed));
      REXSYS_WARN("[timerd] cancels={} dropped={} last-drop-state={}",
                  rex_diag_timer_cancel.load(std::memory_order_relaxed),
                  rex_diag_tq_dropped.load(std::memory_order_relaxed),
                  rex_diag_tq_drop_state.load(std::memory_order_relaxed));
      REXSYS_WARN("[timerd] last-arm-due={}ms max-arm-due={}ms",
                  rex_diag_timer_last_due_ms.load(std::memory_order_relaxed),
                  rex_diag_timer_max_due_ms.load(std::memory_order_relaxed));
      {
        const uint64_t pages = rex_diag_shmem_upload_pages.load(std::memory_order_relaxed);
        const uint32_t psz = rex_diag_shmem_page_size.load(std::memory_order_relaxed);
        REXSYS_WARN("[shmem] uploads={} pages={} ({} MB total, page {} B)",
                    rex_diag_shmem_upload_calls.load(std::memory_order_relaxed), pages,
                    (pages * uint64_t(psz ? psz : 4096)) >> 20, psz);
      }
      const auto tq = rex::thread::GetTimerQueueDiagnostics();
      REXSYS_WARN("[timerq] loops={} dispatched={} completed={} queued={} claim-waits={} "
                  "pending={}{}",
                  tq.iterations, tq.dispatched, tq.completed, tq.queued, tq.claim_waits,
                  tq.pending, tq.in_callback ? "  IN CALLBACK" : "");
    }

    // Two unconditional thread dumps while the port is being brought up. The
    // hang watchdog below only fires when frames stop, which does not cover a
    // title that renders and plays audio quite happily but never advances - and
    // that is exactly where this one sits. Taken a minute apart so they can be
    // compared: identical stacks mean the guest is parked, different ones mean
    // it is working and waiting on something that never completes.
    // The trace answers "which guest functions ran in this window". Armed
    // once the title has settled into the freeze, dumped 30s later, so the
    // recorded set is the code that keeps running while nothing advances.
    // Armed late enough to catch the front end rather than the intro movie:
    // the slow part starts once menus appear, well past a minute in.
    if (uptime == 120) {
      REXSYS_WARN("[trace] arming guest call trace");
      skate3::guest_trace::Arm("switch-freeze");
    }
    if (uptime == 150) {
      REXSYS_WARN("[trace] dumping guest call trace");
      skate3::guest_trace::Dump("switch-freeze");
    }

    if (uptime == 60 || uptime == 120 || uptime == 240) {
      REXSYS_WARN("[dump] periodic thread dump at {}s (not a hang report)", uptime);
      DumpAllThreads();
    }
#endif

    // A backgrounded app is supposed to look exactly like a hang: the guest is
    // parked on purpose, no frames are being produced, and no work is being
    // submitted. Reporting that would turn every ordinary suspend into a hang
    // report with a full thread dump - expensive, and it would bury the real
    // ones in the diagnostics the reports are read from.
    //
    // The tick counters are reset, so the seconds spent in the background are
    // not counted toward a hang on the way back in. last_seen and last_work are
    // deliberately left alone: they are compared against monotonic counters,
    // and clearing them would fake a heartbeat on the first tick after resume.
    if (!rex::graphics::IsAppForeground()) {
      stalled_ticks = 0;
      idle_ticks = 0;
      continue;
    }

    const int limit = REXCVAR_GET(skate3_hang_watchdog_seconds);
    if (limit <= 0) {
      continue;
    }
    // Frames still arriving but the guest submitting no work: two guest
    // threads deadlocked against each other, with the render thread happily
    // presenting an unchanging scene. The frame heartbeat below cannot see
    // this, because the frames never stopped - so it is checked separately,
    // and given a longer grace period since a legitimate load screen submits
    // nothing for a while too.
    // Its own limit, and shorter than three times the frame-stall one it used
    // to borrow. A player who hits a hang relaunches within a few seconds -
    // long before 45 - so the dump that would explain it never got written.
    int stall_limit = REXCVAR_GET(skate3_stall_watchdog_seconds);
    if (stall_limit <= 0) {
      stall_limit = limit * 3;
    }
    const uint64_t work = g_guest_work.load(std::memory_order_relaxed);
    if (work != last_work) {
      last_work = work;
      idle_ticks = 0;
    } else if (work != 0 && ++idle_ticks >= stall_limit &&
               !g_hang_reported.exchange(true, std::memory_order_relaxed)) {
      Report r;
      r.Str("\n=== skate3: STALL - frames still presenting, guest submitting no draws ===\n");
      WriteReportStamp(r);
      r.Flush();
      DumpAllThreads();
    }

    const uint64_t now = g_heartbeat.load(std::memory_order_relaxed);
    // Before the guest has ever presented a frame the heartbeat is legitimately
    // zero for the whole of boot - ISO scan, module load, the intro - which on
    // this game is comfortably longer than the normal watchdog period. Arming
    // the watchdog early (so a guest that never starts still gets a dump) made
    // that fire on EVERY healthy launch, and because the report latches, the
    // real hang later in the same run was then never dumped at all. Give the
    // pre-first-frame phase its own, much longer grace.
    if (now == 0 && stalled_ticks < kPreFirstFrameGraceSeconds) {
      ++stalled_ticks;
      continue;
    }
    if (now != last_seen) {
      last_seen = now;
      stalled_ticks = 0;
      if (idle_ticks == 0) {
        g_hang_reported.store(false, std::memory_order_relaxed);
      }
      continue;
    }
    // The heartbeat legitimately stops while the game is not presenting
    // frames at all (alt-tabbed, a long blocking load). Only the first stall
    // past the limit is reported; the next resumed frame re-arms it.
    if (++stalled_ticks >= limit && !g_hang_reported.exchange(true, std::memory_order_relaxed)) {
      DumpAllThreads();
    }
  }
}

void StartWatchdog() {
  // Once, however many times it is asked for. There are two independent entry
  // points - StartWatchdogEarly before the guest starts, and EnsureInstalled
  // from the first guest swap - and each had its own std::once_flag, so each
  // started a watchdog and BOTH ran for the whole session. Every periodic line
  // was therefore printed twice (which is why [progress], [heap], [frame] and
  // [jobs] all appeared in alternating pairs from two thread ids), every
  // mallinfo() walk was done twice on a heap with 37,000 free chunks, and
  // uptime - a counter each thread incremented once a second - advanced at two
  // seconds per second. That last one was measured as a 1.97x clock error and
  // very nearly blamed on nanosleep.
  static std::once_flag once;
  std::call_once(once, [] {
#if defined(__linux__)
  struct sigaction sa = {};
  sa.sa_sigaction = ThreadDumpHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;  // SA_RESTART: do not break blocking syscalls
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGRTMIN, &sa, nullptr) != 0) {
    return;
  }
#endif
    std::thread(WatchdogMain).detach();
  });
}

// REX_FATAL (a guest call through a null/unregistered function pointer, among
// others) logs and then std::abort()s, which is a SIGABRT rather than an
// access violation - the runtime's exception chain never sees it. This is the
// signature that actually reproduces on map transitions, so it needs the same
// register dump: ctr names the bogus call target and lr names the guest caller
// that loaded it.
#if !defined(__SWITCH__)
struct sigaction g_prev_sigabrt;

void AbortHandler(int sig, siginfo_t* info, void* uctx) {
  static std::atomic<int> reporting{0};
  int expected = 0;
  if (reporting.compare_exchange_strong(expected, 1)) {
    Report r;
    r.Str("\n=== skate3: guest abort (REX_FATAL / assert) ===\n");
    WriteReportStamp(r);
    WriteGuestState(r);
  }
  // Chain, then let abort() finish the job: returning from here re-raises with
  // the default disposition.
  if ((g_prev_sigabrt.sa_flags & SA_SIGINFO) && g_prev_sigabrt.sa_sigaction) {
    g_prev_sigabrt.sa_sigaction(sig, info, uctx);
  } else if (g_prev_sigabrt.sa_handler != SIG_DFL && g_prev_sigabrt.sa_handler != SIG_IGN &&
             g_prev_sigabrt.sa_handler) {
    g_prev_sigabrt.sa_handler(sig);
  }
}
#endif  // !__SWITCH__

bool CrashReportHandler(rex::arch::Exception* ex, void* /*data*/) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation &&
      ex->code() != rex::arch::Exception::Code::kIllegalInstruction) {
    return false;
  }
  // A fault raised while reporting (a wild ThreadState pointer, say) must not
  // recurse - report the first one and let the process die.
  static std::atomic<int> reporting{0};
  int expected = 0;
  if (!reporting.compare_exchange_strong(expected, 1)) {
    return false;
  }

  Report r;
  r.Str("\n=== skate3: unhandled guest fault ===\n");
  WriteReportStamp(r);

  const bool illegal = ex->code() == rex::arch::Exception::Code::kIllegalInstruction;
  r.Str(illegal ? "  kind          illegal instruction\n" : "  kind          access violation\n");

  if (!illegal) {
    switch (ex->access_violation_operation()) {
      case rex::arch::Exception::AccessViolationOperation::kRead:
        r.Str("  operation     READ\n");
        break;
      case rex::arch::Exception::AccessViolationOperation::kWrite:
        r.Str("  operation     WRITE\n");
        break;
      default:
        r.Str("  operation     unknown\n");
        break;
    }
    const uint64_t fault = ex->fault_address();
    r.Str("  host fault    0x");
    r.Hex(fault, 16);
    r.Str("\n");
    const uint64_t lo = uint64_t(reinterpret_cast<uintptr_t>(g_guest_base));
    if (g_guest_base && fault >= lo && fault - lo < kGuestSpanBytes) {
      r.Str("  GUEST addr    0x");
      r.Hex(fault - lo, 8);
      r.Str("\n");
    } else {
      r.Str("  GUEST addr    (outside the guest mapping - host-side fault)\n");
    }
  }

  r.Str("  host pc       0x");
  r.Hex(ex->pc(), 16);
  r.Str("\n");

  WriteGuestState(r);
  return false;  // decline: the process must still die
}

}  // namespace

void StartWatchdogEarly() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_crash_fd < 0) {
      const std::string& log_file = REXCVAR_GET(log_file);
      const std::string path =
          log_file.empty() ? std::string("skate3_crash.txt") : log_file + ".crash";
      g_crash_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    snprintf(g_stamp, sizeof(g_stamp), "v=%s platform=%s", SKATE3_VERSION_STRING,
             SKATE3_BUILD_PLATFORM);
    g_epoch0.store(uint64_t(time(nullptr)), std::memory_order_relaxed);
    StartWatchdog();
    REXLOG_INFO("skate3 crash reporter: hang watchdog armed before the guest starts");
  });
}

void EnsureInstalled(uint8_t* guest_base) {
  static std::once_flag once;
  std::call_once(once, [guest_base] {
    g_guest_base = guest_base;
    // Pre-open the sink here, on a normal thread: opening a file is not
    // something the handler can do safely.
    const std::string& log_file = REXCVAR_GET(log_file);
    const std::string path = log_file.empty() ? std::string("skate3_crash.txt") : log_file + ".crash";
    g_crash_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (g_crash_fd < 0) {
      REXLOG_WARN("skate3 crash reporter: could not open '{}'; stderr only", path);
    }
    // Attribution for every block appended to the .crash file. Formatted here,
    // on a normal thread, so the handlers only ever copy bytes.
    snprintf(g_stamp, sizeof(g_stamp), "v=%s platform=%s", SKATE3_VERSION_STRING,
             SKATE3_BUILD_PLATFORM);
    g_epoch0.store(uint64_t(time(nullptr)), std::memory_order_relaxed);

    rex::arch::ExceptionHandler::Install(CrashReportHandler, nullptr);
    StartWatchdog();

#if !defined(__SWITCH__)
    struct sigaction sa = {};
    sa.sa_sigaction = AbortHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, &g_prev_sigabrt);
#else
    // Horizon has no signals, so a guest abort cannot be intercepted this way
    // and that one report is not produced here. Faults still are: the kernel
    // hands them to __libnx_exception_handler, which runs CrashReportHandler
    // installed just above, so an access violation reports exactly as it does
    // everywhere else. What is lost is the REX_FATAL path, where the runtime
    // has already logged the reason before calling abort.
#endif

    REXLOG_INFO("skate3 crash reporter installed (guest faults report to stderr and '{}')", path);
  });
}

void Heartbeat() { g_heartbeat.fetch_add(1, std::memory_order_relaxed); }

void NoteGuestWork(uint64_t submitted) {
  g_guest_work.store(submitted, std::memory_order_relaxed);
}

}  // namespace skate3::crash_report

#endif  // !_WIN32
