#pragma once

#include <nlohmann/json.hpp>

#include <atomic>

namespace Slic3r { namespace AIAssistant {

// Presets and filament slots, through the controls of the sidebar: the preset
// lists, the slot buttons and the color picker. GUI thread only.

// Selected presets and the presets the lists of the sidebar offer (visible and
// compatible with the selected printer), for get_project.
nlohmann::json presets_summary();

// One action after the user's confirmation: select a printer, process or
// filament preset, save the edited preset as a user preset, add or remove a
// filament slot, change the color of a slot.
nlohmann::json edit_presets(const nlohmann::json& args, const std::atomic<bool>& abandoned);

}} // namespace Slic3r::AIAssistant
