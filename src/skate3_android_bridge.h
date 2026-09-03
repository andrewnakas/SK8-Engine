// Calls from the engine into the Android activity that hosts it.
//
// Both go through STATIC methods on the concrete activity class - whatever
// subclass of SDLActivity the app registered - resolved at run time from the
// activity object SDL already holds, so nothing here names the app's package.
// The Kotlin side is the single place that knows what these do.
#pragma once

#include <filesystem>
#include <string_view>

namespace skate3::android {

// Shows the system document picker (ACTION_OPEN_DOCUMENT) and blocks the
// calling SDL thread until the player chooses or cancels. Returns a
// /proc/self/fd/<n> path for a descriptor the process now owns, or an empty
// path. Callable from the SDL thread only, never from the Android main thread.
std::filesystem::path PickDocument(std::string_view title);

// Asks the activity to relaunch the app once this process exits. Returns
// whether it accepted; the caller still has to quit.
bool RequestRestart();

}  // namespace skate3::android
