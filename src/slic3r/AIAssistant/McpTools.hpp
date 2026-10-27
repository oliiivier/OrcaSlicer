#pragma once

#include "McpServer.hpp"

#include <atomic>
#include <memory>

namespace Slic3r { namespace AIAssistant {

// Slicer tools exposed over MCP. Each call is executed on the GUI thread
// (the model and presets are not thread-safe); the server thread waits for it.
class McpTools : public McpToolbox
{
public:
    McpTools();

    nlohmann::json list_tools() const override;
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& arguments) override;

    // Makes pending and future calls fail immediately. Call before stopping the
    // server from the GUI thread, otherwise the server thread may wait for a
    // GUI-thread callback that never runs.
    void cancel();

private:
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};

}} // namespace Slic3r::AIAssistant
