#include "McpDialog.hpp"
#include "McpService.hpp"
#include "McpSettings.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <boost/format.hpp>

namespace Slic3r { namespace AIAssistant {

using GUI::from_u8;

static std::string connection_instructions(const McpSettings& s)
{
    const std::string url = "http://127.0.0.1:" + std::to_string(s.port) + "/mcp";
    return (boost::format(
                "Claude Code:\n"
                "  claude mcp add --transport http snapmaker-orca %1% --header \"Authorization: Bearer %2%\"\n"
                "\n"
                "Gemini CLI (~/.gemini/settings.json):\n"
                "  \"mcpServers\": {\n"
                "    \"snapmaker-orca\": {\n"
                "      \"httpUrl\": \"%1%\",\n"
                "      \"headers\": { \"Authorization\": \"Bearer %2%\" }\n"
                "    }\n"
                "  }\n"
                "\n"
                "Claude Desktop (claude_desktop_config.json, requires Node.js):\n"
                "  \"mcpServers\": {\n"
                "    \"snapmaker-orca\": {\n"
                "      \"command\": \"npx\",\n"
                "      \"args\": [\"mcp-remote\", \"%1%\", \"--header\", \"Authorization: Bearer %2%\"]\n"
                "    }\n"
                "  }\n") %
            url % s.token)
        .str();
}

McpDialog::McpDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("AI Assistant (MCP server)"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const McpSettings settings = McpSettings::load();

    auto* intro = new wxStaticText(this, wxID_ANY,
                                   _L("Lets an AI assistant (Claude, Gemini...) analyze the open project, fix the problems reported by "
                                      "the slicer and tune the print settings. Every change has to be confirmed here. "
                                      "The server only accepts connections from this computer."));
    intro->Wrap(FromDIP(560));

    m_enabled = new wxCheckBox(this, wxID_ANY, _L("Enable MCP server"));
    m_enabled->SetValue(settings.enabled);

    m_port = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1025, 65535,
                            settings.port);
    m_token = new wxTextCtrl(this, wxID_ANY, from_u8(settings.token), wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    auto* regenerate = new wxButton(this, wxID_ANY, _L("Regenerate"));

    auto* grid = new wxFlexGridSizer(3, FromDIP(6), FromDIP(6));
    grid->AddGrowableCol(1);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Port")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_port, 0);
    grid->AddSpacer(0);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Token")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_token, 1, wxEXPAND);
    grid->Add(regenerate, 0);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);

    m_instructions = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(560, 260)),
                                    wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
    m_instructions->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    auto* copy = new wxButton(this, wxID_ANY, _L("Copy"));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(intro, 0, wxALL, FromDIP(10));
    sizer->Add(m_enabled, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    sizer->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Client configuration")), 0, wxLEFT | wxRIGHT, FromDIP(10));
    sizer->Add(m_instructions, 1, wxEXPAND | wxALL, FromDIP(10));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(copy, 0, wxRIGHT, FromDIP(6));
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_CLOSE, _L("Close")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    SetSizerAndFit(sizer);
    SetEscapeId(wxID_CLOSE);

    m_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { apply_and_refresh(); });
    m_port->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { apply_and_refresh(); });
    regenerate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_token->SetValue(from_u8(McpSettings::generate_token()));
        apply_and_refresh();
    });
    copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_instructions->GetValue()));
            wxTheClipboard->Close();
        }
    });

    refresh();
    CenterOnParent();
    GUI::wxGetApp().UpdateDlgDarkUI(this);
}

void McpDialog::apply_and_refresh()
{
    McpSettings s = McpSettings::load();
    s.enabled     = m_enabled->GetValue();
    s.port        = static_cast<unsigned short>(m_port->GetValue());
    s.token       = GUI::into_u8(m_token->GetValue());
    s.save();

    m_error.clear();
    McpService::apply(s, m_error);
    refresh();
}

void McpDialog::refresh()
{
    const McpSettings s = McpSettings::load();
    if (McpService::is_running())
        m_status->SetLabel(wxString::Format(_L("Running on %s"), from_u8(McpService::url())));
    else if (!m_error.empty())
        m_status->SetLabel(wxString::Format(_L("Could not start the server: %s"), from_u8(m_error)));
    else
        m_status->SetLabel(_L("Stopped"));
    m_instructions->SetValue(from_u8(connection_instructions(s)));
    Layout();
}

}} // namespace Slic3r::AIAssistant
