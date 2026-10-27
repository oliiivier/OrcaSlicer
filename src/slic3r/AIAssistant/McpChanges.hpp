#pragma once

#include <nlohmann/json.hpp>

#include <atomic>

namespace Slic3r { namespace AIAssistant {

// Validates the requested setting changes, asks the user to confirm them in a
// modal dialog, then applies them to the edited presets or to object overrides.
// `abandoned` is set by the server thread when the client stopped waiting: the
// changes are then not applied even if the user confirms.
// GUI thread only.
nlohmann::json apply_changes(const nlohmann::json& args, const std::atomic<bool>& abandoned);

}} // namespace Slic3r::AIAssistant
