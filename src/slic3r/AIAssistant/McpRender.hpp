#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Renders the objects of the current plate from a named viewpoint, colored by
// filament, and returns the MCP content: a PNG image plus a text legend.
// GUI thread only (needs the OpenGL context of the 3D view).
nlohmann::json render_view_content(const nlohmann::json& args);

// Shows the sliced current plate in the Preview tab from a named viewpoint, with
// the layer slider set to args.from_layer..to_layer, and captures the 3D view as
// the user sees it (PNG and legend). The view changes on the screen. Runs over
// several calls through ToolRetry (loading, frame, capture).
nlohmann::json preview_content(const nlohmann::json& args, nlohmann::json& state);

}} // namespace Slic3r::AIAssistant
