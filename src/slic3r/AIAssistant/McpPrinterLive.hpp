#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace AIAssistant {

// Live reads of the printer connected through the Snapmaker Connect gateway,
// for what changes within seconds (the gateway device view lags). They need
// the "printer access" setting and change nothing on the printer. GUI thread only.

// Values of the given Klipper objects ({ "print_stats": ["state"], ... }, null
// for all fields), read from the printer now. Returns null with the reason in
// `reason` when printer access is disabled or the printer cannot be reached.
nlohmann::json live_printer_objects(const nlohmann::json& objects, std::string& reason);

// MCP content (JPEG image and caption) of a picture taken now by the printer
// camera (the webcam snapshot declared to Moonraker).
nlohmann::json printer_snapshot(const nlohmann::json& args);

// Waits, for a bounded time, until the print state changes (or the print ends,
// or a new layer starts) and reports the progress. Retried through ToolRetry.
nlohmann::json watch_print(const nlohmann::json& args, nlohmann::json& state);

}} // namespace Slic3r::AIAssistant
