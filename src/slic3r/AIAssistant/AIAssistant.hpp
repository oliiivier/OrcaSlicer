#pragma once

// Public entry points of the AI assistant module.
// This is the only header upstream code is allowed to know about; keep it tiny.

class wxMenu;

namespace Slic3r { namespace AIAssistant {

// Appends the assistant entries to the given menu (called from MainFrame's help menu).
// The first call also starts the MCP server when it is enabled, so that no other
// hook in upstream code is needed.
void append_menu_items(wxMenu* menu);

// Opens the MCP server settings.
void show_settings();

}} // namespace Slic3r::AIAssistant
