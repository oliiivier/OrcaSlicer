#pragma once

#include <nlohmann/json.hpp>

#include <atomic>

namespace Slic3r { namespace AIAssistant {

// Project files, plates and starting a print, through the slicer's own commands
// and dialogs. Each action is confirmed by the user, either in a confirmation or
// in the slicer's file or print dialog. GUI thread only.

// The plates of the project: index, name, objects, plate settings, sliced or not.
nlohmann::json plates_summary();

nlohmann::json new_project(const nlohmann::json& args, const std::atomic<bool>& abandoned);
// Opens a 3MF project (args.path, or the user picks it).
nlohmann::json open_project(const nlohmann::json& args, const std::atomic<bool>& abandoned);
// Adds models from files (args.paths, or the user picks them) to the current plate.
nlohmann::json import_models(const nlohmann::json& args, const std::atomic<bool>& abandoned);
// Saves the project to its file, to args.path, or with the slicer's save dialog.
nlohmann::json save_project(const nlohmann::json& args, const std::atomic<bool>& abandoned);
// Exports the G-code or the sliced 3MF of the current plate, or the STL of the
// objects, to args.path or with the slicer's save dialog.
nlohmann::json export_file(const nlohmann::json& args, const std::atomic<bool>& abandoned);

// Selects, adds, deletes, renames plates and changes their settings (bed type,
// print sequence), in order.
nlohmann::json edit_plates(const nlohmann::json& args, const std::atomic<bool>& abandoned);

// Creates a calibration project, as the Calibration menu once its dialog is
// filled: args.action temperature, flow, pressure_advance, retraction,
// max_volumetric_speed or vfa, with the parameters of that dialog.
nlohmann::json calibrate(const nlohmann::json& args, const std::atomic<bool>& abandoned);

// Opens the print dialog of the slicer for the sliced current plate, like the
// Print plate button: the user checks the filament mapping and starts the print.
nlohmann::json start_print(const nlohmann::json& args);

}} // namespace Slic3r::AIAssistant
