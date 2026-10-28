#include "AIAssistant.hpp"
#include "ChatPanel.hpp"
#include "McpDialog.hpp"
#include "McpService.hpp"
#include "McpSettings.hpp"
#include "PrinterAutoConnect.hpp"
#include "ProjectAdapt.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <boost/log/trivial.hpp>
#include <wx/aui/auibar.h>
#include <wx/menu.h>

namespace Slic3r { namespace AIAssistant {

using GUI::wxGetApp;

// Deferred to the next event loop iteration: the help menu is built while the
// main frame is still being constructed.
static void init_once()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    wxGetApp().CallAfter([]() {
        GUI::MainFrame* frame = wxGetApp().mainframe;
        if (frame == nullptr || wxGetApp().is_gcode_viewer())
            return;
        // Stop the server while the plater still exists.
        frame->Bind(wxEVT_DESTROY, [frame](wxWindowDestroyEvent& evt) {
            if (evt.GetEventObject() == frame)
                McpService::shutdown();
            evt.Skip();
        });

        start_printer_auto_connect();
        start_adaptation_watch();

        const McpSettings settings = McpSettings::load();
        std::string       error;
        if (!McpService::apply(settings, error))
            BOOST_LOG_TRIVIAL(error) << "MCP: could not start the server on port " << settings.port << ": " << error;
    });
}

static wxString menu_label()
{
    return McpService::is_running() ? _L("AI Assistant (MCP server): running") : _L("AI Assistant (MCP server): stopped");
}

void append_menu_items(wxMenu* menu)
{
    init_once();

    menu->AppendSeparator();
    ::append_menu_item(
        menu, wxID_ANY, _L("Chat with Claude"), _L("Ask Claude about this project and let it propose setting changes (requires Claude Code)"),
        [](wxCommandEvent&) { ChatPanel::show(); }, "", nullptr,
        []() { return wxGetApp().plater() != nullptr && !wxGetApp().is_gcode_viewer(); });
    wxMenuItem* item = ::append_menu_item(
        menu, wxID_ANY, menu_label(), _L("Let Claude, Gemini or another MCP client analyze and fix the print settings"),
        [](wxCommandEvent&) { show_settings(); }, "", nullptr,
        []() { return wxGetApp().plater() != nullptr && !wxGetApp().is_gcode_viewer(); });
    // The server state, refreshed each time the menu is opened.
    menu->Bind(wxEVT_MENU_OPEN, [item](wxMenuEvent& evt) {
        item->SetItemLabel(menu_label());
        evt.Skip();
    });
}

void show_settings()
{
    McpDialog dlg(wxGetApp().mainframe);
    dlg.ShowModal();
}

// Next to the top bar's own ids (3100-3200, BBLTopbar.cpp).
static const int ID_TOPBAR_CHAT = 3190;
static const int TOPBAR_ICON_SIZE = 18;

void add_topbar_button(wxAuiToolBar* bar)
{
    wxAuiToolBarItem* item = bar->AddTool(ID_TOPBAR_CHAT, _L("Claude"), create_scaled_bitmap("topbar_claude", nullptr, TOPBAR_ICON_SIZE));
    item->SetShortHelp(_L("Chat with Claude about this project"));
    // The top bar reports clicks on its tools as drop-down events.
    bar->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, [](wxAuiToolBarEvent&) { ChatPanel::toggle(); }, ID_TOPBAR_CHAT);
}

void rescale_topbar_button(wxAuiToolBar* bar)
{
    if (wxAuiToolBarItem* item = bar->FindTool(ID_TOPBAR_CHAT))
        item->SetBitmap(create_scaled_bitmap("topbar_claude", nullptr, TOPBAR_ICON_SIZE));
}

}} // namespace Slic3r::AIAssistant
