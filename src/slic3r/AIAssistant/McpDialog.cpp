#include "McpDialog.hpp"
#include "McpService.hpp"
#include "McpSettings.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/SpinInput.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"

#include <wx/clipbrd.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <boost/format.hpp>

namespace Slic3r { namespace AIAssistant {

using GUI::from_u8;

namespace {

// Light theme colors, converted for the dark theme by GUI_App::UpdateDlgDarkUI.
const wxColour TEXT_COLOR(38, 46, 48);      // DESIGN_GRAY900_COLOR
const wxColour SECONDARY_COLOR(144, 144, 144); // DESIGN_GRAY600_COLOR
const wxColour RUNNING_COLOR("#009688");    // slicer accent
const wxColour ERROR_COLOR("#E14747");
const wxColour INSTRUCTIONS_BG("#F8F8F8"); // panel background, #36363C in the dark theme

std::string connection_instructions(const McpSettings& s)
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

wxStaticText* label(wxWindow* parent, const wxString& text, const wxFont& font = Label::Body_14)
{
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetFont(font);
    t->SetForegroundColour(TEXT_COLOR);
    return t;
}

// The slicer's check box with a clickable label, as in the preferences.
wxSizer* checkbox_row(wxWindow* parent, ::CheckBox* checkbox, const wxString& text, int wrap)
{
    auto* title = label(parent, text);
    title->Wrap(wrap);
    title->Bind(wxEVT_LEFT_DOWN, [checkbox](wxMouseEvent&) {
        checkbox->SetValue(!checkbox->GetValue());
        wxCommandEvent evt(wxEVT_TOGGLEBUTTON, checkbox->GetId());
        evt.SetEventObject(checkbox);
        checkbox->GetEventHandler()->ProcessEvent(evt);
    });
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(8));
    return row;
}

} // namespace

McpDialog::McpDialog(wxWindow* parent)
    : GUI::DPIDialog(parent, wxID_ANY, _L("AI Assistant (MCP server)"), wxDefaultPosition, wxDefaultSize,
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const McpSettings settings = McpSettings::load();
    const int         wrap     = FromDIP(600);
    const int         margin   = FromDIP(16);

    auto* intro = label(this,
                        _L("Lets an AI assistant (Claude, Gemini...) analyze the open project, fix the problems reported by "
                           "the slicer and tune the print settings. Every change has to be confirmed here. "
                           "The server only accepts connections from this computer."),
                        Label::Body_13);
    intro->SetForegroundColour(SECONDARY_COLOR);
    intro->Wrap(wrap);

    // Server state, shown first and in color so that it can be seen at a glance.
    m_status_dot = label(this, wxString::FromUTF8("\xE2\x97\x8F"), Label::Head_14); // U+25CF BLACK CIRCLE
    m_status     = label(this, wxEmptyString, Label::Head_14);
    auto* status = new wxBoxSizer(wxHORIZONTAL);
    status->Add(m_status_dot, 0, wxALIGN_CENTER_VERTICAL);
    status->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));

    m_enabled = new ::CheckBox(this);
    m_enabled->SetValue(settings.enabled);
    m_printer_access = new ::CheckBox(this);
    m_printer_access->SetValue(settings.printer_access);

    m_port = new SpinInput(this, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(120), -1), wxSP_ARROW_KEYS, 1025,
                           65535, settings.port);
    m_token = new TextInput(this, from_u8(settings.token), wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                            wxTE_READONLY);
    auto* regenerate = new Button(this, _L("Regenerate"));
    regenerate->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    auto* grid = new wxFlexGridSizer(3, FromDIP(8), FromDIP(8));
    grid->AddGrowableCol(1);
    grid->Add(label(this, _L("Port")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_port, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);
    grid->Add(label(this, _L("Token")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_token, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(regenerate, 0, wxALIGN_CENTER_VERTICAL);

    m_instructions = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(600, 240)),
                                    wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL | wxBORDER_SIMPLE);
    m_instructions->SetFont(wxFont(wxFontInfo(Label::Body_13.GetPointSize()).Family(wxFONTFAMILY_TELETYPE)));

    auto* buttons = new GUI::DialogButtons(this, { "Copy", "OK" });

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(checkbox_row(this, m_enabled, _L("Enable MCP server"), wrap), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(checkbox_row(this, m_printer_access,
                            _L("Allow reading the state of the connected printer (loaded filaments, nozzles, print status and "
                               "errors). The printer is never controlled."),
                            wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(label(this, _L("Client configuration"), Label::Head_14), 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(m_instructions, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(buttons, 0, wxEXPAND);
    SetSizerAndFit(sizer);

    m_enabled->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        apply_and_refresh();
        e.Skip();
    });
    // Read by the tools at each call: no server restart needed.
    m_printer_access->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s    = McpSettings::load();
        s.printer_access = m_printer_access->GetValue();
        s.save();
        e.Skip();
    });
    m_port->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { apply_and_refresh(); });
    regenerate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_token->GetTextCtrl()->SetValue(from_u8(McpSettings::generate_token()));
        apply_and_refresh();
    });
    buttons->GetButtonFromID(wxID_COPY)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_instructions->GetValue()));
            wxTheClipboard->Close();
        }
    });
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

    refresh();
    CenterOnParent();
    GUI::wxGetApp().UpdateDlgDarkUI(this);
    // After UpdateDlgDarkUI, which leaves the native multiline text control white on GTK.
    const bool dark = GUI::wxGetApp().dark_mode();
    m_instructions->SetBackgroundColour(dark ? StateColor::darkModeColorFor(INSTRUCTIONS_BG) : INSTRUCTIONS_BG);
    m_instructions->SetForegroundColour(dark ? wxColour("#DFDFDF") : TEXT_COLOR);
}

void McpDialog::on_dpi_changed(const wxRect&) { Fit(); }

void McpDialog::apply_and_refresh()
{
    McpSettings s = McpSettings::load();
    s.enabled     = m_enabled->GetValue();
    s.port        = static_cast<unsigned short>(m_port->GetValue());
    s.token       = GUI::into_u8(m_token->GetTextCtrl()->GetValue());
    s.save();

    m_error.clear();
    McpService::apply(s, m_error);
    refresh();
}

void McpDialog::refresh()
{
    const McpSettings s = McpSettings::load();
    wxColour          color;
    if (McpService::is_running()) {
        color = RUNNING_COLOR;
        m_status->SetLabel(wxString::Format(_L("Running on %s"), from_u8(McpService::url())));
    } else if (!m_error.empty()) {
        color = ERROR_COLOR;
        m_status->SetLabel(wxString::Format(_L("Could not start the server: %s"), from_u8(m_error)));
    } else {
        color = SECONDARY_COLOR;
        m_status->SetLabel(_L("Stopped"));
    }
    // Same color in both themes: the state colors are readable on light and dark backgrounds.
    m_status_dot->SetForegroundColour(color);
    m_status->SetForegroundColour(color);
    m_status->Wrap(FromDIP(580));
    m_instructions->SetValue(from_u8(connection_instructions(s)));
    Layout();
    Refresh();
}

}} // namespace Slic3r::AIAssistant
