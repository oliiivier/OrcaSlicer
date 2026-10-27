#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Renders the objects of the current plate from a named viewpoint, colored by
// filament, and returns the MCP content: a PNG image plus a text legend.
// GUI thread only (needs the OpenGL context of the 3D view).
nlohmann::json render_view_content(const nlohmann::json& args);

}} // namespace Slic3r::AIAssistant
