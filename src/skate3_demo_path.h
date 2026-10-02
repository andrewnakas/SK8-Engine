#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::input {
class InputSystem;
}

namespace skate3::demo_path {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);
// Provider for the merged UI pad state consumed by the user-facing frontend
// movie skip. Set during app setup, before the guest boots (the provider is
// read from the guest thread without further synchronization).
void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider);
// The provider's current input system, or null before app setup. For other
// host features that poll the merged UI pad state.
rex::input::InputSystem* GetUiInputSystem();
bool ShouldForceIntroMovieComplete();
bool ShouldSkipIntroMovieEarly();

// Macro progress, for the loading overlay: how many pad inputs of the boot
// sequence have been injected, how many there are in total, and whether the
// sequence has finished. All zero/false when no macro is running.
// Replay an arbitrary pad sequence right now, without waiting for the gameplay
// context (the caller is already in gameplay). Used by the in-game level
// selector to drive the pause menu the same way the boot macro does.
bool RunGameplayInputs(const std::string& sequence, int32_t settle_ms);

// Press START until the frontend stack actually shows the pause root, rather
// than pressing once and hoping. The gameplay presence context flips BEFORE
// the game will take input, and how long before varies per map, so no fixed
// wait is correct. Degrades to a single press when the stack is unreadable.
bool PressStartUntilPaused(int32_t attempts, int32_t wait_ms);

int32_t GameplayInputsInjected();
int32_t GameplayInputsTotal();
bool GameplayInputSequenceComplete();

}  // namespace skate3::demo_path
