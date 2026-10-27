#pragma once

#include <wx/dialog.h>

#include <string>

class wxCheckBox;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace AIAssistant {

// Enables the MCP server and shows how to connect Claude or Gemini to it.
class McpDialog : public wxDialog
{
public:
    explicit McpDialog(wxWindow* parent);

private:
    void apply_and_refresh();
    void refresh();

    wxCheckBox*   m_enabled { nullptr };
    wxCheckBox*   m_printer_access { nullptr };
    wxSpinCtrl*   m_port { nullptr };
    wxTextCtrl*   m_token { nullptr };
    wxStaticText* m_status { nullptr };
    wxTextCtrl*   m_instructions { nullptr };
    std::string   m_error;
};

}} // namespace Slic3r::AIAssistant
