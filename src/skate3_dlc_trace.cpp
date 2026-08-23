#include "skate3_dlc_trace.h"

#include <cctype>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_BOOL(skate3_dlc_trace, false, "Skate 3",
                    "Log every call through the guest's DLC content path: the content "
                    "manager, the DLC driver's mount, each archive added by path, and "
                    "each asynchronous file open under a content package. Off by "
                    "default - it is loud, and it is a diagnostic for packs that never "
                    "mount rather than something to run normally.");

namespace {

bool Enabled() { return REXCVAR_GET(skate3_dlc_trace); }

std::string ReadGuestString(uint8_t* base, uint32_t address, size_t max_length = 512) {
  if (!address) {
    return {};
  }
  std::string value;
  value.reserve(128);
  for (size_t i = 0; i < max_length; ++i) {
    const char character = static_cast<char>(REX_LOAD_U8(address + i));
    if (!character) {
      break;
    }
    value.push_back(character);
  }
  return value;
}

void LogCall(const char* operation, PPCContext& ctx) {
  if (!Enabled()) {
    return;
  }
  REXLOG_WARN("skate3-dlc: {} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} caller=0x{:08X}",
              operation, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.lr);
}

}  // namespace

// Each hook logs and then runs the original. Addresses are the same in this
// fork as in SK8-Engine's - both are the same recompilation of the same TU3.
extern "C" REX_FUNC(Skate3DlcTrace_ManagerConstructor) {
  LogCall("Manager::Manager", ctx);
  sub_82580980(ctx, base);
}

extern "C" REX_FUNC(Skate3DlcTrace_ManagerRun) {
  LogCall("Manager::Run", ctx);
  sub_82580E68(ctx, base);
}

extern "C" REX_FUNC(Skate3DlcTrace_ManagerEnumerate) {
  LogCall("Manager::EnumerateContent", ctx);
  sub_825810B0(ctx, base);
}

extern "C" REX_FUNC(Skate3DlcTrace_ManagerRefresh) {
  LogCall("Manager::Refresh", ctx);
  sub_82581698(ctx, base);
}

extern "C" REX_FUNC(Skate3DlcTrace_DriverMount) {
  LogCall("XenonDLCDriver::Mount", ctx);
  sub_82581918(ctx, base);
}

extern "C" REX_FUNC(Skate3DlcTrace_ContentManagerEnumerate) {
  if (Enabled()) {
    // The active user index matters: content discovery treats the title
    // screen's sentinel 0xFF as "nobody is signed in" and returns before it
    // ever enumerates. A pack that "does not exist" can be this.
    constexpr uint32_t kActiveUserIndexAddress = 0x82FC8851;
    const auto active_user = REX_LOAD_U8(kActiveUserIndexAddress);
    REXLOG_WARN(
        "skate3-dlc: ContentManager::EnumerateContent manager=0x{:08X} "
        "active_user={} (raw=0x{:02X}) caller=0x{:08X}",
        ctx.r3.u32, static_cast<int8_t>(active_user), active_user, ctx.lr);
  }
  sub_82582410(ctx, base);
}

extern "C" REX_FUNC(Skate3DlcTrace_AddArchiveFromFile) {
  if (Enabled()) {
    REXLOG_WARN(
        "skate3-dlc: BigHandler::AddArchiveFromFile handler=0x{:08X} path='{}' "
        "flags=0x{:08X} caller=0x{:08X}",
        ctx.r3.u32, ReadGuestString(base, ctx.r4.u32), ctx.r5.u32, ctx.lr);
  }
  sub_82A83080(ctx, base);
}

// Content-looking paths only. This is the hot one - the guest opens files
// constantly - and logging every call at WARN during boot floods the log hard
// enough to STALL the boot: measured 2026-08-23, a working map that boots in
// 48 s never reached gameplay in 143 s with this unfiltered. The filter keeps
// what the diagnostic is for (which DLC file the guest is waiting on) and
// drops the rest.
bool LooksLikeContent(const std::string& path) {
  static constexpr const char* kNeedles[] = {"dlc", "content", ".big", "downl"};
  std::string lower;
  lower.reserve(path.size());
  for (char c : path) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  for (const char* needle : kNeedles) {
    if (lower.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

extern "C" REX_FUNC(Skate3DlcTrace_AsyncFileOpen) {
  if (Enabled()) {
    // The one that matters most for a stalled pack: the guest parks in a file
    // wait holding a handle, and this names the file it asked for.
    const std::string path = ReadGuestString(base, ctx.r4.u32);
    if (LooksLikeContent(path)) {
      REXLOG_WARN("skate3-dlc: AsyncFileOpen path='{}' caller=0x{:08X}", path, ctx.lr);
    }
  }
  sub_8298F430(ctx, base);
}

namespace skate3::dlc_trace {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (!dispatcher || !REXCVAR_GET(skate3_dlc_trace)) {
    return;
  }
  dispatcher->SetFunction(0x82580980, &Skate3DlcTrace_ManagerConstructor);
  dispatcher->SetFunction(0x82580E68, &Skate3DlcTrace_ManagerRun);
  dispatcher->SetFunction(0x825810B0, &Skate3DlcTrace_ManagerEnumerate);
  dispatcher->SetFunction(0x82581698, &Skate3DlcTrace_ManagerRefresh);
  dispatcher->SetFunction(0x82581918, &Skate3DlcTrace_DriverMount);
  dispatcher->SetFunction(0x82582410, &Skate3DlcTrace_ContentManagerEnumerate);
  dispatcher->SetFunction(0x82A83080, &Skate3DlcTrace_AddArchiveFromFile);
  dispatcher->SetFunction(0x8298F430, &Skate3DlcTrace_AsyncFileOpen);
}

}  // namespace skate3::dlc_trace
