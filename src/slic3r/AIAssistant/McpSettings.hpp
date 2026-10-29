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
    // Lets the tools send the few commands they support (e.g. unload filament),
    // each one confirmed by the user. Requires printer_access.
    bool           printer_control { false };
    // At startup, connects to the printer when it is the only one online.
    bool           auto_connect_printer { true };
    // Offers to adapt a project made for another printer to the connected one.
    bool           suggest_adaptation { true };
    // Claude Code executable for the chat, empty to look for it.
    std::string    claude_path;
    // Folder of Markdown notes shared with the assistant, empty for the default one.
    std::string    notes_dir;
    // Lets the assistant create and edit notes (reading is always allowed).
    bool           notes_writable { true };
    // Lets the tools search and read the Snapmaker wiki and support site.
    bool           manufacturer_docs { true };

    static McpSettings load();
    void               save() const;

    static std::string generate_token();
};

}} // namespace Slic3r::AIAssistant
