// One file a player can send that answers "what is different about this
// device".
//
// The useful evidence is spread across places a phone player cannot reach: the
// engine writes its log and, when it catches a fault, a crash report beside it,
// both inside the app sandbox; stderr goes to a third file; and the device
// details that decide which code path even runs are in none of them. On iOS
// there is no adb and no file manager that can open another app's container,
// so without this there is no way to get any of it off the phone.
//
// Ported from the Android launcher's Diagnostics.kt, which learned the lesson
// this file is built around: that report read Build.SOC_MANUFACTURER, an
// API-31 field, under minSdk 28. It threw NoSuchFieldError and killed the whole
// report on exactly the older devices whose owners most needed to send one. So
// every section here is gathered behind its own try/catch and a section that
// fails is reported as failed rather than taking the report down with it.

#ifndef SKATE3_DIAGNOSTICS_REPORT_H_
#define SKATE3_DIAGNOSTICS_REPORT_H_

#include <filesystem>
#include <string>

namespace skate3 {

// The whole report as plain text. Never throws and never returns empty: a
// section that cannot be gathered contributes a line saying so.
std::string CollectDiagnosticsReport(const std::filesystem::path& game_root,
                                     const std::filesystem::path& user_root);

// Writes the report next to the engine log, where file sharing can reach it,
// and returns the path. Empty on failure, with `error` filled.
std::filesystem::path WriteDiagnosticsReport(const std::filesystem::path& game_root,
                                             const std::filesystem::path& user_root,
                                             std::string& error);

// Device, OS and GPU identity. Implemented per platform - on Apple in
// skate3_diagnostics_report_ios.mm, because the model name, the memory
// headroom and the Metal device all need Objective-C.
//
// Reporting the graphics driver up front is worth its own mention: several long
// bug hunts in this project turned out to be driver bugs, and knowing the
// MoltenVK and Metal versions at the start would have shortened all of them.
std::string DiagnosticsDeviceSection();

}  // namespace skate3

#endif  // SKATE3_DIAGNOSTICS_REPORT_H_
