#include "McpService.hpp"
#include "McpServer.hpp"
#include "McpSettings.hpp"
#include "McpTools.hpp"

#include <memory>

namespace Slic3r { namespace AIAssistant { namespace McpService {

namespace {

struct State
{
    // The server references the toolbox: declared after it so it is destroyed first.
    std::unique_ptr<McpTools>  tools;
    std::unique_ptr<McpServer> server;

    ~State() { stop(); }

    void stop()
    {
        // Cancel first: the server thread may be waiting for a GUI-thread tool call.
        if (tools)
            tools->cancel();
        server.reset();
        tools.reset();
    }
};

State& state()
{
    static State s;
    return s;
}

} // namespace

bool apply(const McpSettings& settings, std::string& error)
{
    State& s = state();
    s.stop();
    if (!settings.enabled)
        return true;

    s.tools  = std::make_unique<McpTools>();
    s.server = std::make_unique<McpServer>(settings.port, settings.token, *s.tools);
    if (!s.server->start(error)) {
        s.stop();
        return false;
    }
    return true;
}

void shutdown() { state().stop(); }

bool is_running() { return state().server && state().server->is_running(); }

std::string url() { return state().server ? state().server->url() : std::string(); }

}}} // namespace Slic3r::AIAssistant::McpService
