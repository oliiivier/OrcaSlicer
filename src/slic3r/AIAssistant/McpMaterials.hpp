#pragma once

#include <nlohmann/json.hpp>

#include <vector>

namespace Slic3r {
class ModelObject;

namespace AIAssistant {

// Printer toolheads and material setup (filament slots, mixed filaments, filament
// assigned to each print feature). Multi-toolhead printers such as the Snapmaker U1
// have one toolhead per filament slot. GUI thread only.
nlohmann::json describe_materials();

// 1-based filament IDs printed by the object's parts, painted regions included.
std::vector<int> object_filaments(const ModelObject& object);

}} // namespace Slic3r::AIAssistant
