#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Slices the current plate like the "Slice plate" button (or every plate like
// "Slice all" when args.all_plates) and reports the result.
// Called repeatedly by the tool runner until done: `state` persists between the
// calls, ToolRetry is thrown while slicing. GUI thread only.
nlohmann::json slice_plate(const nlohmann::json& args, nlohmann::json& state);

}} // namespace Slic3r::AIAssistant
