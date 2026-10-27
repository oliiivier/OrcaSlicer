#include "AIAssistant.hpp"
#include "AssistantDialog.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <wx/menu.h>
#include <wx/weakref.h>

namespace Slic3r { namespace AIAssistant {

using GUI::wxGetApp;

// The dialog is owned by the main frame (wx parent); we only keep a weak handle on it.
static wxWeakRef<AssistantDialog> s_dialog;

void append_menu_items(wxMenu* menu)
{
    menu->AppendSeparator();
    ::append_menu_item(
        menu, wxID_ANY, _L("AI Assistant"), _L("Analyze the project and get setting recommendations"),
        [](wxCommandEvent&) { show_assistant(); }, "", nullptr,
        // The assistant works on the project, it is meaningless in the G-code viewer.
        []() { return wxGetApp().plater() != nullptr && !wxGetApp().is_gcode_viewer(); });
}

void show_assistant()
{
    if (!s_dialog)
        s_dialog = new AssistantDialog(wxGetApp().mainframe);
    s_dialog->Show();
    s_dialog->Raise();
}

}} // namespace Slic3r::AIAssistant
