#pragma once

#include <string>

namespace Slic3r { namespace AIAssistant {

struct McpSettings;

// Owns the MCP server and its toolbox for the lifetime of the application.
// All functions must be called from the GUI thread.
namespace McpService {

// Stops the server if running, then starts it again when settings.enabled.
// Returns false and fills `error` when the server could not be started.
bool apply(const McpSettings& settings, std::string& error);

void        shutdown();
bool        is_running();
std::string url();

} // namespace McpService

}} // namespace Slic3r::AIAssistant
