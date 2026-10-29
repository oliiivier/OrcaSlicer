#include "McpDialog.hpp"
#include "ClaudeProcess.hpp"
#include "McpNotes.hpp"
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
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

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
                "Claude Code (slicing and print watching take longer than its default 60 s per call:\n"
                "start it with MCP_TOOL_TIMEOUT=%3%):\n"
                "  claude mcp add --transport http snapmaker-orca %1% --header \"Authorization: Bearer %2%\"\n"
                "\n"
                "Gemini CLI (~/.gemini/settings.json):\n"
                "  \"mcpServers\": {\n"
                "    \"snapmaker-orca\": {\n"
                "      \"httpUrl\": \"%1%\",\n"
                "      \"headers\": { \"Authorization\": \"Bearer %2%\" },\n"
                "      \"timeout\": %3%\n"
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
            url % s.token % McpSettings::TOOL_TIMEOUT_MS)
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
        if (!checkbox->IsEnabled())
            return;
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
    m_external_access = new ::CheckBox(this);
    m_external_access->SetValue(settings.external_access);
    m_printer_access = new ::CheckBox(this);
    m_printer_access->SetValue(settings.printer_access);
    m_printer_control = new ::CheckBox(this);
    m_printer_control->SetValue(settings.printer_access && settings.printer_control);
    m_printer_control->Enable(settings.printer_access);
    m_auto_connect = new ::CheckBox(this);
    m_auto_connect->SetValue(settings.auto_connect_printer);
    m_suggest_adaptation = new ::CheckBox(this);
    m_suggest_adaptation->SetValue(settings.suggest_adaptation);
    m_manufacturer_docs = new ::CheckBox(this);
    m_manufacturer_docs->SetValue(settings.manufacturer_docs);

    m_port = new SpinInput(this, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(120), -1), wxSP_ARROW_KEYS, 1025,
                           65535, settings.port);
    m_token = new TextInput(this, from_u8(settings.token), wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                            wxTE_READONLY);
    auto* regenerate = new Button(this, _L("Regenerate"));
    regenerate->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_regenerate = regenerate;

    auto* grid = new wxFlexGridSizer(3, FromDIP(8), FromDIP(8));
    grid->AddGrowableCol(1);
    grid->Add(label(this, _L("Port")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_port, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);
    grid->Add(label(this, _L("Token")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_token, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(regenerate, 0, wxALIGN_CENTER_VERTICAL);

    m_claude_path = new TextInput(this, from_u8(settings.claude_path), wxEmptyString, wxEmptyString);
    m_claude_path->GetTextCtrl()->SetHint(_L("Found automatically"));
    auto* browse = new Button(this, _L("Browse..."));
    browse->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    grid->Add(label(this, _L("Claude Code")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_claude_path, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(browse, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);
    m_claude_found = label(this, wxEmptyString, Label::Body_13);
    grid->Add(m_claude_found, 1, wxEXPAND);
    grid->AddSpacer(0);

    m_notes_dir = new TextInput(this, from_u8(settings.notes_dir), wxEmptyString, wxEmptyString);
    m_notes_dir->GetTextCtrl()->SetHint(from_u8(default_notes_dir()));
    auto* browse_notes = new Button(this, _L("Browse..."));
    browse_notes->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    grid->Add(label(this, _L("Notes folder")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_notes_dir, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    grid->Add(browse_notes, 0, wxALIGN_CENTER_VERTICAL);
    m_notes_writable = new ::CheckBox(this);
    m_notes_writable->SetValue(settings.notes_writable);
    auto* open_notes = new Button(this, _L("Open"));
    open_notes->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    grid->AddSpacer(0);
    grid->Add(checkbox_row(this, m_notes_writable, _L("Let the assistant add and edit notes"), FromDIP(400)), 1, wxEXPAND);
    grid->Add(open_notes, 0, wxALIGN_CENTER_VERTICAL);

    // Free text: a native multiline control, as the client configuration below.
    m_user_instructions = new wxTextCtrl(this, wxID_ANY, from_u8(user_instructions()), wxDefaultPosition,
                                         FromDIP(wxSize(600, 120)), wxTE_MULTILINE | wxBORDER_SIMPLE);
    m_user_instructions->SetHint(_L("e.g. Answer in French. I print mostly PETG on the U1 and prefer strong parts over speed."));
    auto* user_instructions_hint = label(this,
                                         wxString::Format(_L("Added to the assistant's instructions at the start of each "
                                                             "conversation. Also editable in %s."),
                                                          from_u8(user_instructions_path())),
                                         Label::Body_13);
    user_instructions_hint->SetForegroundColour(SECONDARY_COLOR);
    user_instructions_hint->Wrap(wrap);

    m_instructions = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(600, 240)),
                                    wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL | wxBORDER_SIMPLE);
    m_instructions->SetFont(wxFont(wxFontInfo(Label::Body_13.GetPointSize()).Family(wxFONTFAMILY_TELETYPE)));

    auto* buttons = new GUI::DialogButtons(this, { "Copy", "OK" });

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(checkbox_row(this, m_enabled, _L("Enable MCP server"), wrap), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(checkbox_row(this, m_external_access,
                            _L("Allow other MCP clients on this computer (Claude Code, Gemini CLI...) to connect with the "
                               "token below. The built-in chat does not need it. Any program that can read the token can "
                               "use the server."),
                            wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(checkbox_row(this, m_printer_access,
                            _L("Allow reading the state of the connected printer (loaded filaments, nozzles, print status and "
                               "errors). The printer is never controlled."),
                            wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(checkbox_row(this, m_printer_control,
                            _L("Allow unloading filament from the toolheads. Each request has to be confirmed here, and it is "
                               "refused while a print is running."),
                            wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(checkbox_row(this, m_auto_connect,
                            _L("At startup, connect to the printer when it is the only one available."), wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(checkbox_row(this, m_suggest_adaptation,
                            _L("Offer to adapt projects made for another printer (e.g. downloaded 3MF files) to the connected one."),
                            wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(checkbox_row(this, m_manufacturer_docs,
                            _L("Allow searching the Snapmaker wiki and support site for the manufacturer's recommendations "
                               "(the searches are sent to Snapmaker)."),
                            wrap),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(label(this, _L("Instructions for the assistant"), Label::Head_14), 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(user_instructions_hint, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    sizer->Add(m_user_instructions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(label(this, _L("Client configuration"), Label::Head_14), 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(m_instructions, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    sizer->Add(buttons, 0, wxEXPAND);
    SetSizerAndFit(sizer);

    m_enabled->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        apply_and_refresh();
        e.Skip();
    });
    m_external_access->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        // A new token each time access is turned on: the clients set up before are left out.
        m_token->GetTextCtrl()->SetValue(m_external_access->GetValue() ? from_u8(McpSettings::generate_token()) : wxString());
        apply_and_refresh();
        e.Skip();
    });
    // Read by the tools at each call: no server restart needed.
    m_printer_access->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s    = McpSettings::load();
        s.printer_access = m_printer_access->GetValue();
        // Control needs the printer state (idle, loaded toolheads): it goes with read access.
        if (!s.printer_access) {
            s.printer_control = false;
            m_printer_control->SetValue(false);
        }
        m_printer_control->Enable(s.printer_access);
        s.save();
        e.Skip();
    });
    m_printer_control->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s     = McpSettings::load();
        s.printer_control = m_printer_control->GetValue();
        s.save();
        e.Skip();
    });
    m_auto_connect->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s          = McpSettings::load();
        s.auto_connect_printer = m_auto_connect->GetValue();
        s.save();
        e.Skip();
    });
    m_suggest_adaptation->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s        = McpSettings::load();
        s.suggest_adaptation = m_suggest_adaptation->GetValue();
        s.save();
        e.Skip();
    });
    m_manufacturer_docs->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s       = McpSettings::load();
        s.manufacturer_docs = m_manufacturer_docs->GetValue();
        s.save();
        e.Skip();
    });
    m_port->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { apply_and_refresh(); });
    regenerate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_token->GetTextCtrl()->SetValue(from_u8(McpSettings::generate_token()));
        apply_and_refresh();
    });
    // Read by the chat when it starts Claude Code.
    m_claude_path->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        McpSettings s = McpSettings::load();
        s.claude_path = GUI::into_u8(m_claude_path->GetTextCtrl()->GetValue().Trim().Trim(false));
        s.save();
        refresh_claude();
        e.Skip();
    });
    browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
#ifdef _WIN32
        const wxString wildcard = "Claude Code (claude.exe, claude.cmd)|claude.exe;claude.cmd|" + _L("All files") + " (*.*)|*.*";
#else
        const wxString wildcard = "Claude Code (claude)|claude|" + _L("All files") + " (*)|*";
#endif
        wxFileDialog dlg(this, _L("Choose the Claude Code executable"), wxEmptyString, wxEmptyString, wildcard,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            m_claude_path->GetTextCtrl()->SetValue(dlg.GetPath()); // saved by the wxEVT_TEXT handler
    });
    // Read by the notes tools at each call.
    m_notes_dir->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        McpSettings s = McpSettings::load();
        s.notes_dir   = GUI::into_u8(m_notes_dir->GetTextCtrl()->GetValue().Trim().Trim(false));
        s.save();
        e.Skip();
    });
    browse_notes->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dlg(this, _L("Choose the notes folder"), from_u8(notes_dir()), wxDD_DEFAULT_STYLE);
        if (dlg.ShowModal() == wxID_OK)
            m_notes_dir->GetTextCtrl()->SetValue(dlg.GetPath()); // saved by the wxEVT_TEXT handler
    });
    m_notes_writable->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        McpSettings s    = McpSettings::load();
        s.notes_writable = m_notes_writable->GetValue();
        s.save();
        e.Skip();
    });
    open_notes->Bind(wxEVT_BUTTON, [](wxCommandEvent&) {
        const std::string dir = notes_dir();
        boost::system::error_code ec;
        boost::filesystem::create_directories(dir, ec);
        GUI::desktop_open_any_folder(dir);
    });
    m_user_instructions->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        m_user_instructions_changed = true;
        e.Skip();
    });
    m_copy = buttons->GetButtonFromID(wxID_COPY);
    buttons->GetButtonFromID(wxID_COPY)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_instructions->GetValue()));
            wxTheClipboard->Close();
        }
    });
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

    refresh();
    refresh_claude();
    CenterOnParent();
    GUI::wxGetApp().UpdateDlgDarkUI(this);
    // After UpdateDlgDarkUI, which leaves the native multiline text control white on GTK.
    const bool dark = GUI::wxGetApp().dark_mode();
    m_instructions->SetBackgroundColour(dark ? StateColor::darkModeColorFor(INSTRUCTIONS_BG) : INSTRUCTIONS_BG);
    m_instructions->SetForegroundColour(dark ? wxColour("#DFDFDF") : TEXT_COLOR);
    m_user_instructions->SetBackgroundColour(dark ? StateColor::darkModeColorFor(*wxWHITE) : *wxWHITE);
    m_user_instructions->SetForegroundColour(dark ? wxColour("#DFDFDF") : TEXT_COLOR);
}

McpDialog::~McpDialog()
{
    if (!m_user_instructions_changed)
        return;
    try {
        save_user_instructions(GUI::into_u8(m_user_instructions->GetValue()));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "AI assistant: could not save the instructions: " << e.what();
    }
}

void McpDialog::on_dpi_changed(const wxRect&) { Fit(); }

void McpDialog::apply_and_refresh()
{
    McpSettings s = McpSettings::load();
    s.enabled         = m_enabled->GetValue();
    s.port            = static_cast<unsigned short>(m_port->GetValue());
    s.external_access = m_external_access->GetValue();
    s.token           = GUI::into_u8(m_token->GetTextCtrl()->GetValue());
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
    m_token->Enable(s.external_access);
    m_regenerate->Enable(s.external_access);
    m_copy->Enable(s.external_access);
    m_instructions->SetValue(s.external_access ? from_u8(connection_instructions(s))
                                               : _L("Allow other MCP clients above to get their configuration."));
    Layout();
    Refresh();
}

void McpDialog::refresh_claude()
{
    const ClaudeProcess::Launch launch = ClaudeProcess::locate(McpSettings::load().claude_path);
    if (launch.valid()) {
        wxString command = from_u8(launch.program);
        for (const std::string& arg : launch.args)
            command += " " + from_u8(arg);
        m_claude_found->SetLabel(wxString::Format(_L("Used by the chat: %s"), command));
        m_claude_found->SetForegroundColour(SECONDARY_COLOR);
    } else {
        m_claude_found->SetLabel(_L("Claude Code not found: install it, or choose its executable."));
        m_claude_found->SetForegroundColour(ERROR_COLOR);
    }
    m_claude_found->Wrap(FromDIP(440));
    Layout();
    Fit();
}

}} // namespace Slic3r::AIAssistant
