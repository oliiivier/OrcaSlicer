#include "AIAssistant.hpp"
#include "McpDialog.hpp"
#include "McpService.hpp"
#include "McpSettings.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <boost/log/trivial.hpp>
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

        const McpSettings settings = McpSettings::load();
        std::string       error;
        if (!McpService::apply(settings, error))
            BOOST_LOG_TRIVIAL(error) << "MCP: could not start the server on port " << settings.port << ": " << error;
    });
}

void append_menu_items(wxMenu* menu)
{
    init_once();

    menu->AppendSeparator();
    ::append_menu_item(
        menu, wxID_ANY, _L("AI Assistant (MCP server)"), _L("Let Claude, Gemini or another MCP client analyze and fix the print settings"),
        [](wxCommandEvent&) { show_settings(); }, "", nullptr,
        []() { return wxGetApp().plater() != nullptr && !wxGetApp().is_gcode_viewer(); });
}

void show_settings()
{
    McpDialog dlg(wxGetApp().mainframe);
    dlg.ShowModal();
}

}} // namespace Slic3r::AIAssistant
