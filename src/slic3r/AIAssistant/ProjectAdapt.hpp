#pragma once

#include <nlohmann/json.hpp>

#include <atomic>

namespace Slic3r { namespace AIAssistant {

// Adaptation of a project made for another printer (e.g. a MakerWorld 3MF) to
// the connected printer. GUI thread only.
//
// After the user's confirmation: selects the printer preset of the connected
// printer with its nozzle, the process preset of that printer with the closest
// layer height, and moves each filament slot to the closest loaded toolhead
// (same material first, then closest color): the slots are renumbered like the
// toolheads, since slot N prints with toolhead N by default, and take their
// presets and colors; the model follows. The print dialog still lets the user
// map toolheads. Returns the original settings that
// differ after the switch, so that the assistant can restore the designer's
// intent within the new printer's limits.
//
// args: { filament_mapping?: [{ slot, toolhead }] } to override the automatic choice.
nlohmann::json adapt_to_printer(const nlohmann::json& args, const std::atomic<bool>& abandoned);

// Watches the open project and, when its printer differs from the connected
// one, shows a notification offering the adaptation (once per project and
// printer preset). Enabled by McpSettings::suggest_adaptation.
void start_adaptation_watch();

}} // namespace Slic3r::AIAssistant
