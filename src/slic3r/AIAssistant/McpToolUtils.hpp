#pragma once

// Internal helpers shared by the MCP tool implementations. GUI thread only.

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {
class ConfigOption;
class ConfigOptionDef;
namespace GUI { class Plater; }

namespace AIAssistant {

// Raised by a tool for a problem the model should see and can fix (bad key...).
struct ToolError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Raised by a tool when the slicer state is being updated; the call is retried shortly.
struct ToolRetry : std::runtime_error
{
    ToolRetry() : std::runtime_error("The slicer is still updating, try again") {}
};

// Returns the plater, or throws ToolError when no FFF project is open.
GUI::Plater& require_plater();

std::vector<std::string> string_list(const nlohmann::json& args, const char* name);

// Short description of a setting, embedded in tool results next to its key.
nlohmann::json setting_summary(const ConfigOptionDef& def);

// Value in the slicer format; per-filament-slot and per-toolhead settings become
// a JSON array (slot or toolhead order) instead of a comma-separated string.
nlohmann::json option_value(const ConfigOption& opt);

} // namespace AIAssistant
} // namespace Slic3r
