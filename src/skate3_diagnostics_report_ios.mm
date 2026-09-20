// The Apple half of the diagnostic report: device, OS, memory and GPU.
//
// Separate from skate3_diagnostics_report.cpp because every line here needs
// Objective-C or a sysctl, and because this is the section most likely to use
// an API the running OS does not have. Each line is gathered independently and
// a line that cannot be produced says so - the Android original lost the whole
// report to one field that did not exist below API 31, on exactly the old
// devices whose owners most needed to send one.

#include "skate3_diagnostics_report.h"

#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#import <os/proc.h>

#include <sstream>
#include <string>

namespace skate3 {
namespace {

// sysctl string by name, empty when the key is absent. hw.machine is the one
// that actually identifies the phone: UIDevice.model says only "iPhone".
std::string SysctlString(const char* name) {
  size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return {};
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return {};
  }
  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

int64_t SysctlInt(const char* name) {
  int64_t value = 0;
  size_t size = sizeof(value);
  if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
    return -1;
  }
  return value;
}

}  // namespace

std::string DiagnosticsDeviceSection() {
  std::ostringstream o;

  @try {
    UIDevice* device = [UIDevice currentDevice];
    o << "Model " << SysctlString("hw.machine") << " ("
      << [[device model] UTF8String] << ")\n";
    o << "iOS " << [[device systemVersion] UTF8String] << "\n";
  } @catch (NSException* e) {
    o << "Model/OS unavailable: " << [[e reason] UTF8String] << "\n";
  }

  @try {
    o << "CPU " << SysctlString("machdep.cpu.brand_string") << "\n";
    o << "Cores " << SysctlInt("hw.ncpu") << " (" << SysctlInt("hw.perflevel0.logicalcpu")
      << " performance, " << SysctlInt("hw.perflevel1.logicalcpu") << " efficiency)\n";
    // Six hardware threads are pinned 1:1 to the 360's, so a device with fewer
    // than six cores is not supported and shows up as enormous lock waits
    // rather than as a clean failure. Worth stating outright in a report.
    const int64_t cores = SysctlInt("hw.ncpu");
    if (cores > 0 && cores < 6) {
      o << "  WARNING: fewer than 6 cores; this configuration is not supported\n";
    }
  } @catch (NSException* e) {
    o << "CPU detail unavailable: " << [[e reason] UTF8String] << "\n";
  }

  @try {
    const int64_t ram = SysctlInt("hw.memsize");
    o << "RAM " << (ram > 0 ? ram / (1024 * 1024) : -1) << " MB\n";
    // What is left before jetsam kills the app, which on iOS is the number
    // that decides the store budgets. Not the same as free RAM, and it is the
    // input PickStoreBudgets actually uses.
    o << "Available to this process " << (os_proc_available_memory() / (1024 * 1024)) << " MB\n";
  } @catch (NSException* e) {
    o << "Memory unavailable: " << [[e reason] UTF8String] << "\n";
  }

  // The GPU section, and the reason this file was worth writing.
  //
  // Several long bug hunts in this project turned out to be driver bugs, and
  // knowing the MoltenVK and Metal versions up front would have shortened all
  // of them. Android's driver check reports driverID/driverName/driverInfo for
  // the same reason; this is the iOS equivalent.
  @try {
    id<MTLDevice> metal = MTLCreateSystemDefaultDevice();
    if (metal != nil) {
      o << "Metal device " << [[metal name] UTF8String] << "\n";
      o << "  max texture 2D " << (unsigned long)[metal maxBufferLength] << " byte buffer limit\n";
      if (@available(iOS 16.0, *)) {
        o << "  unified memory " << ([metal hasUnifiedMemory] ? "yes" : "no")
          << ", recommended working set "
          << ([metal recommendedMaxWorkingSetSize] / (1024 * 1024)) << " MB\n";
      }
      // GPU family tells you which shader features are present far more
      // reliably than the marketing name does.
      if ([metal supportsFamily:MTLGPUFamilyApple8]) {
        o << "  GPU family Apple8 or newer\n";
      } else if ([metal supportsFamily:MTLGPUFamilyApple7]) {
        o << "  GPU family Apple7\n";
      } else if ([metal supportsFamily:MTLGPUFamilyApple6]) {
        o << "  GPU family Apple6\n";
      } else {
        o << "  GPU family Apple5 or older\n";
      }
    } else {
      o << "Metal device unavailable\n";
    }
  } @catch (NSException* e) {
    o << "Metal unavailable: " << [[e reason] UTF8String] << "\n";
  }

  // MoltenVK's own version is deliberately NOT read here. Its headers are not
  // on this target's include path - only the static library is linked, into
  // rexruntime - and adding an include directory for one line is a worse trade
  // than pointing at where the number already appears: the engine log names the
  // Vulkan device and its driver at startup, and this report carries the log.
  o << "MoltenVK: see the Vulkan device line in the engine log section below\n";

  @try {
    o << "Thermal state ";
    switch ([[NSProcessInfo processInfo] thermalState]) {
      case NSProcessInfoThermalStateNominal: o << "nominal\n"; break;
      case NSProcessInfoThermalStateFair: o << "fair\n"; break;
      case NSProcessInfoThermalStateSerious: o << "SERIOUS (the GPU is being clocked down)\n"; break;
      case NSProcessInfoThermalStateCritical: o << "CRITICAL\n"; break;
      default: o << "unknown\n"; break;
    }
    o << "Low power mode "
      << ([[NSProcessInfo processInfo] isLowPowerModeEnabled] ? "ON (caps the GPU)" : "off")
      << "\n";
  } @catch (NSException* e) {
    o << "Thermal state unavailable\n";
  }

  return o.str();
}

}  // namespace skate3
