#pragma once

#include <nlohmann/json.hpp>

#include <atomic>

namespace Slic3r {
class ModelObject;

namespace AIAssistant {

// Size and position of an object as get_project and edit_objects report them:
// bounding box size, center (x, y) and bottom (z) in the coordinates of its plate.
nlohmann::json object_geometry(size_t obj_idx);

// Parts, modifiers and other volumes of an object: 1-based index, name, type and
// filament (0 = the object's filament).
nlohmann::json object_parts(const ModelObject& obj);

// Height ranges of an object with their own settings: from_mm, to_mm, settings.
nlohmann::json object_height_ranges(const ModelObject& obj);

// Undoes or redoes args.steps steps of the slicer's history (args.action), after
// the user's confirmation listing them.
nlohmann::json undo_redo(const nlohmann::json& args, const std::atomic<bool>& abandoned);

// Runs the object operations of the Prepare view (scale, rotate, move, mirror,
// cut, split, duplicate, delete, add or delete a part or modifier, filament,
// rename, height ranges, orient, arrange) one after the other once the
// user confirmed them. Orient and arrange are background jobs: the call answers
// ToolRetry until they end and resumes with `state`. `abandoned` is set by the
// server thread when the client stopped waiting: nothing more is changed then.
// GUI thread only.
nlohmann::json edit_objects(const nlohmann::json& args, const std::atomic<bool>& abandoned, nlohmann::json& state);

} // namespace AIAssistant
} // namespace Slic3r
