#include "McpService.hpp"
#include "McpServer.hpp"
#include "McpSettings.hpp"
#include "McpTools.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

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

// Tokens of the live grants, checked from the server threads.
struct Grants
{
    std::mutex               mutex;
    std::vector<std::string> tokens;
};

Grants& grants()
{
    static Grants g;
    return g;
}

// Constant time: the comparison does not tell how much of a guessed token is right.
bool same_token(const std::string& a, const std::string& b)
{
    return !a.empty() && a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

Grant::Grant() : m_token(McpSettings::generate_token())
{
    Grants&                     g = grants();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.tokens.push_back(m_token);
}

Grant::~Grant()
{
    Grants&                     g = grants();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.tokens.erase(std::remove(g.tokens.begin(), g.tokens.end(), m_token), g.tokens.end());
}

bool apply(const McpSettings& settings, std::string& error)
{
    State& s = state();
    s.stop();
    if (!settings.enabled)
        return true;

    s.tools  = std::make_unique<McpTools>();
    const std::string external = settings.external_access ? settings.token : std::string();
    auto authorize = [external](const std::string& token) {
        if (same_token(token, external))
            return true;
        Grants&                     g = grants();
        std::lock_guard<std::mutex> lock(g.mutex);
        return std::any_of(g.tokens.begin(), g.tokens.end(), [&token](const std::string& t) { return same_token(token, t); });
    };
    s.server = std::make_unique<McpServer>(settings.port, authorize, *s.tools);
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
