#include "skate3_performance_profile.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

REXCVAR_DEFINE_STRING(
    skate3_performance_profile, "", "Skate 3",
    "Apply a video preset at startup: 'potato' (every frame the machine has - no "
    "shadows, no effects, no HDR, half the original draw distance), 'performance', "
    "'balanced', 'quality' or 'ultra'. Empty (the default) leaves the settings alone. "
    "A preset only fills in the settings you have not named yourself: anything set "
    "explicitly on the command line, or in ios_args.txt, wins over it.");

namespace skate3 {

void ApplyRequestedPerformanceProfile() {
  const std::string requested = REXCVAR_GET(skate3_performance_profile);
  if (requested.empty() || requested == "none") {
    return;
  }
  if (rex::ui::ApplyGraphicsPresetByName(requested)) {
    return;
  }

  // Say which names would have worked. A misspelled preset that silently does
  // nothing looks exactly like a preset that applied and changed nothing, and
  // telling those apart by watching frame times is the guessing this exists to
  // remove.
  std::string valid;
  for (const std::string& name : rex::ui::GraphicsPresetNames()) {
    valid += valid.empty() ? "" : ", ";
    valid += name;
  }
  REXLOG_WARN("skate3_performance_profile='{}' is not a preset; nothing applied. Try one of: {}",
              requested, valid);
}

}  // namespace skate3
