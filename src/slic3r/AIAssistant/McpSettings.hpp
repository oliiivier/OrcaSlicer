#pragma once

#include <string>

namespace Slic3r { namespace AIAssistant {

// MCP server preferences, persisted in the [ai_assistant] section of AppConfig
// (the user's config, never inside project files).
struct McpSettings
{
    static constexpr unsigned short DEFAULT_PORT = 13700;

    bool           enabled { false };
    unsigned short port { DEFAULT_PORT };
    // Bearer token required from MCP clients. Generated on first load.
    std::string    token;
    // Lets the tools read the state of the connected printer (never control it).
    bool           printer_access { false };

    static McpSettings load();
    void               save() const;

    static std::string generate_token();
};

}} // namespace Slic3r::AIAssistant
