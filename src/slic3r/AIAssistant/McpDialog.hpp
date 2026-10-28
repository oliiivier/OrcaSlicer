#pragma once

#include "slic3r/GUI/GUI_Utils.hpp"

#include <string>

class CheckBox;
class SpinInput;
class TextInput;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace AIAssistant {

// Enables the MCP server and shows how to connect Claude or Gemini to it.
// Built from the slicer's own widgets so that it follows the light and dark themes.
class McpDialog : public GUI::DPIDialog
{
public:
    explicit McpDialog(wxWindow* parent);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void apply_and_refresh();
    void refresh();

    ::CheckBox*   m_enabled { nullptr };
    ::CheckBox*   m_printer_access { nullptr };
    ::CheckBox*   m_printer_control { nullptr };
    ::CheckBox*   m_auto_connect { nullptr };
    ::CheckBox*   m_suggest_adaptation { nullptr };
    SpinInput*    m_port { nullptr };
    TextInput*    m_token { nullptr };
    wxStaticText* m_status_dot { nullptr };
    wxStaticText* m_status { nullptr };
    wxTextCtrl*   m_instructions { nullptr };
    std::string   m_error;
};

}} // namespace Slic3r::AIAssistant
