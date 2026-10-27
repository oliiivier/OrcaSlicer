#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace Slic3r { namespace AIAssistant {

// What the server exposes to MCP clients. Called from the server thread.
class McpToolbox
{
public:
    virtual ~McpToolbox() = default;

    // MCP "tools/list" result entries: [{ name, description, inputSchema }, ...]
    virtual nlohmann::json list_tools() const = 0;

    // MCP "tools/call" result: { content: [...], isError: bool }.
    // Throws std::invalid_argument for an unknown tool or invalid arguments.
    virtual nlohmann::json call_tool(const std::string& name, const nlohmann::json& arguments) = 0;
};

// Minimal MCP server over the Streamable HTTP transport (single "/mcp" endpoint,
// JSON responses, no SSE stream). Listens on 127.0.0.1 only and requires
// "Authorization: Bearer <token>". Requests are served one at a time.
class McpServer
{
public:
    McpServer(unsigned short port, std::string token, McpToolbox& toolbox);
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Returns false and fills `error` when the port cannot be bound.
    bool start(std::string& error);
    void stop();
    bool is_running() const;

    unsigned short port() const { return m_port; }
    std::string    url() const;

    // Transport-independent handling of one JSON-RPC message.
    // Returns a null json for notifications and responses (nothing to send back).
    nlohmann::json handle_message(const nlohmann::json& msg);

private:
    struct Impl;

    unsigned short        m_port;
    std::string           m_token;
    McpToolbox&           m_toolbox;
    std::unique_ptr<Impl> m_impl;

    friend class McpSession;
};

}} // namespace Slic3r::AIAssistant
