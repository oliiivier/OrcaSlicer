#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Errors and warnings the slicer currently reports for the current plate, most
// severe first: { plate, issues: [{ severity, blocks_slicing, source, message,
// setting?, object?, filament_slots? }], summary }.
// GUI thread only. Throws ToolRetry while the slicer state is being updated.
nlohmann::json collect_issues();

}} // namespace Slic3r::AIAssistant
