#pragma once

#include <nlohmann/json.hpp>

#include <atomic>

namespace Slic3r { namespace AIAssistant {

// The only commands the MCP tools send to the printer. Each one needs the
// "printer control" setting and is confirmed by the user in the slicer. GUI
// thread only.

// Unloads the filament of the given toolheads (args.toolheads, 1-based) with the
// same G-code sequence as the printer screen (AUTO_FEEDING_BATCH: the firmware
// heats the nozzle itself). Runs over several calls through ToolRetry, keeping
// its progress in `state`; toolheads that are empty or hold a soft filament are
// skipped.
nlohmann::json unload_filament(const nlohmann::json& args, const std::atomic<bool>& abandoned, nlohmann::json& state);

// Called when unload_filament stops being retried before it finished: ends the
// sequence so that no nozzle is left hot.
void unload_filament_abandoned(nlohmann::json& state);

// Pauses, resumes or cancels the running print (args.action), like the buttons of
// the device page, then waits until the printer reports the new state. Runs over
// several calls through ToolRetry.
nlohmann::json control_print(const nlohmann::json& args, const std::atomic<bool>& abandoned, nlohmann::json& state);

}} // namespace Slic3r::AIAssistant
