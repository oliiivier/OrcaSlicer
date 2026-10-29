#pragma once

#include <string>

namespace Slic3r { namespace AIAssistant {

struct McpSettings;

// Owns the MCP server and its toolbox for the lifetime of the application.
// All functions must be called from the GUI thread.
namespace McpService {

// A token for one MCP client the slicer starts itself (the chat's Claude Code), accepted
// until the grant is destroyed. Kept in memory only, and still valid when the server restarts.
class Grant
{
public:
    Grant();
    ~Grant();

    Grant(const Grant&) = delete;
    Grant& operator=(const Grant&) = delete;

    const std::string& token() const { return m_token; }

private:
    std::string m_token;
};

// Stops the server if running, then starts it again when settings.enabled. It accepts the
// tokens of the live grants, and settings.token when settings.external_access.
// Returns false and fills `error` when the server could not be started.
bool apply(const McpSettings& settings, std::string& error);

void        shutdown();
bool        is_running();
std::string url();

} // namespace McpService

}} // namespace Slic3r::AIAssistant
