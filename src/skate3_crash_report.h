// Guest fault reporter.
//
// A SIGSEGV on a guest thread currently produces NO output at all: the
// runtime's exception chain declines it (the MMIO handler only reports write
// faults, and guest threads are armed for read recovery only inside
// native-scene entry points), then DispatchUnhandledSignal re-raises with the
// default disposition and the process dies silently. Three sessions of log
// reading found nothing because there was nothing to find.
//
// EnsureInstalled registers a terminal handler that describes the fault -
// faulting host and GUEST address, read vs write, host pc, thread name, and
// the full guest register file - and then DECLINES it, so the process still
// dies exactly as before. It diagnoses; it does not recover.
//
// Must be installed AFTER the runtime's own handlers (MMIO write-watch
// recovery, GuestTryCopy's siglongjmp guard, the raw-load read recovery) so
// those keep first refusal on the faults they legitimately handle; the chain
// runs handlers in registration order. Call it from the same place
// ArmGuestReadRecoveryForThread is called - by then everything else is up.
// Idempotent and cheap.

#ifndef SKATE3_CRASH_REPORT_H_
#define SKATE3_CRASH_REPORT_H_

#include <cstdint>

namespace skate3::crash_report {

#if defined(_WIN32)
inline void EnsureInstalled(uint8_t* /*guest_base*/) {}
inline void Heartbeat() {}
#else
// guest_base is the runtime's virtual membase, used to render the faulting
// host address as a guest address (the only form that means anything when
// reading recompiled code).
void EnsureInstalled(uint8_t* guest_base);

// Called at the guest frame boundary. When it stops advancing for
// skate3_hang_watchdog_seconds, every thread's stack is dumped - a freeze
// raises no signal, so this is the only way to see one from the inside.
void Heartbeat();
#endif

}  // namespace skate3::crash_report

#endif  // SKATE3_CRASH_REPORT_H_
