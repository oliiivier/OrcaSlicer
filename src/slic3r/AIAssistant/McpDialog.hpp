#pragma once

#include "slic3r/GUI/GUI_Utils.hpp"

#include <string>

class CheckBox;
class SpinInput;
class TextInput;
class wxSizer;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace AIAssistant {

// Enables the MCP server and shows how to connect Claude or Gemini to it.
// Built from the slicer's own widgets so that it follows the light and dark themes.
class McpDialog : public GUI::DPIDialog
{
public:
    explicit McpDialog(wxWindow* parent);
    // Saves the user's instructions, whichever way the dialog was closed.
    ~McpDialog() override;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void apply_and_refresh();
    void refresh();
    // Shows which Claude Code executable the chat uses.
    void refresh_claude();

    ::CheckBox*   m_enabled { nullptr };
    ::CheckBox*   m_external_access { nullptr };
    ::CheckBox*   m_printer_access { nullptr };
    ::CheckBox*   m_printer_control { nullptr };
    ::CheckBox*   m_auto_connect { nullptr };
    ::CheckBox*   m_suggest_adaptation { nullptr };
    ::CheckBox*   m_manufacturer_docs { nullptr };
    SpinInput*    m_port { nullptr };
    TextInput*    m_token { nullptr };
    wxWindow*     m_copy { nullptr };
    wxSizer*      m_external_section { nullptr };
    TextInput*    m_claude_path { nullptr };
    wxStaticText* m_claude_found { nullptr };
    wxStaticText* m_status_dot { nullptr };
    wxStaticText* m_status { nullptr };
    wxTextCtrl*   m_instructions { nullptr };
    wxTextCtrl*   m_user_instructions { nullptr };
    bool          m_user_instructions_changed { false };
    TextInput*    m_notes_dir { nullptr };
    ::CheckBox*   m_notes_writable { nullptr };
    std::string   m_error;
};

}} // namespace Slic3r::AIAssistant
