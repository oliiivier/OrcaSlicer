#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace AIAssistant {

// Read-only view of the printer connected through the Snapmaker Connect gateway:
// filament and nozzle of each toolhead, print state, last error, and optionally
// raw Klipper objects for diagnosis. Never sends commands to the printer (see
// McpPrinterControl for the few commands the tools may send).
// GUI thread only; returns { connected: false, reason } when unavailable.
nlohmann::json printer_status(const nlohmann::json& args);

// Appends to `issues` the differences between the project and the printer
// (missing filament, other material or nozzle diameter on a used toolhead).
// Does nothing when printer access is disabled or no printer is connected.
void add_printer_issues(nlohmann::json& issues);

// Klipper objects of the connected printer, read now rather than from the cache.
// Returns null with the reason in `reason` when printer access is disabled or the
// printer cannot be reached.
nlohmann::json printer_objects(std::string& reason);

}} // namespace Slic3r::AIAssistant
