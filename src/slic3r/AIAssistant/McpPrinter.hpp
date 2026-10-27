#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Read-only view of the printer connected through the Snapmaker Connect gateway:
// filament and nozzle of each toolhead, print state, last error, and optionally
// raw Klipper objects for diagnosis. Never sends commands to the printer.
// GUI thread only; returns { connected: false, reason } when unavailable.
nlohmann::json printer_status(const nlohmann::json& args);

// Appends to `issues` the differences between the project and the printer
// (missing filament, other material or nozzle diameter on a used toolhead).
// Does nothing when printer access is disabled or no printer is connected.
void add_printer_issues(nlohmann::json& issues);

}} // namespace Slic3r::AIAssistant
