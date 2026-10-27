#pragma once

// Public entry points of the AI assistant module.
// This is the only header upstream code is allowed to know about; keep it tiny.

class wxMenu;

namespace Slic3r { namespace AIAssistant {

// Appends the assistant entries to the given menu (called from MainFrame's help menu).
void append_menu_items(wxMenu* menu);

// Opens (or raises) the assistant window.
void show_assistant();

}} // namespace Slic3r::AIAssistant
