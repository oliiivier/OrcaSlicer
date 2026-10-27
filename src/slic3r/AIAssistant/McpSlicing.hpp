#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Slices the current plate like the "Slice plate" button and reports the result.
// Called repeatedly by the tool runner until done: `state` persists between the
// calls, ToolRetry is thrown while slicing. GUI thread only.
nlohmann::json slice_plate(nlohmann::json& state);

}} // namespace Slic3r::AIAssistant
