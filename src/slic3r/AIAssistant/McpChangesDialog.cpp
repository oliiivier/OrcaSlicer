#include "McpChangesDialog.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include <wx/display.h>
#include <wx/gbsizer.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>

namespace Slic3r { namespace AIAssistant {

using GUI::from_u8;

namespace {

// Light theme colors, converted for the dark theme by GUI_App::UpdateDlgDarkUI.
const wxColour TEXT_COLOR(38, 46, 48);         // DESIGN_GRAY900_COLOR
const wxColour SECONDARY_COLOR(144, 144, 144); // DESIGN_GRAY600_COLOR

const wxString CHEVRON_CLOSED = wxString::FromUTF8("\xE2\x96\xB8"); // U+25B8 small right-pointing triangle
const wxString CHEVRON_OPEN   = wxString::FromUTF8("\xE2\x96\xBE"); // U+25BE small down-pointing triangle

wxStaticText* label(wxWindow* parent, const wxString& text, const wxFont& font, const wxColour& color = TEXT_COLOR)
{
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetFont(font);
    t->SetForegroundColour(color);
    return t;
}

void toggle(::CheckBox* checkbox)
{
    checkbox->SetValue(!checkbox->GetValue());
    wxCommandEvent evt(wxEVT_TOGGLEBUTTON, checkbox->GetId());
    evt.SetEventObject(checkbox);
    checkbox->GetEventHandler()->ProcessEvent(evt);
}

wxString tooltip_text(const ProposedChange& c)
{
    wxString text;
    if (!c.comment.empty())
        text = _L("AI assistant:") + " " + from_u8(c.comment);
    if (!c.tooltip.empty())
        text += (text.empty() ? wxString() : wxString("\n\n")) + from_u8(c.tooltip);
    return text;
}

} // namespace

McpChangesDialog::McpChangesDialog(wxWindow* parent, const std::string& reason, const std::vector<ProposedChange>& changes)
    : GUI::DPIDialog(parent, wxID_ANY, _L("AI Assistant"), wxDefaultPosition, wxDefaultSize,
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const int wrap   = FromDIP(560);
    const int margin = FromDIP(16);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(label(this, _L("Changes proposed by the AI assistant"), Label::Head_14), 0, wxLEFT | wxRIGHT | wxTOP, margin);
    if (!reason.empty()) {
        auto* intro = label(this, from_u8(reason), Label::Body_13, SECONDARY_COLOR);
        intro->Wrap(wrap);
        sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    }

    m_all = new ::CheckBox(this);
    m_all->SetValue(true);
    auto* all_title = label(this, _L("Select all"), Label::Body_14);
    all_title->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { toggle(m_all); });
    auto* all_row = new wxBoxSizer(wxHORIZONTAL);
    all_row->Add(m_all, 0, wxALIGN_CENTER_VERTICAL);
    all_row->Add(all_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    sizer->Add(all_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    // One line per change, values aligned in a column; the details line under it is folded.
    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_list->SetBackgroundColour(*wxWHITE);
    m_list->SetScrollRate(0, FromDIP(10));
    auto* grid = new wxGridBagSizer(FromDIP(2), FromDIP(12));
    for (size_t i = 0; i < changes.size(); ++i) {
        const ProposedChange& c   = changes[i];
        const int             row = int(i) * 2;

        auto* check = new ::CheckBox(m_list);
        check->SetValue(true);
        m_checks.push_back(check);
        auto* title   = label(m_list, from_u8(c.label), Label::Body_14);
        auto* values  = label(m_list, from_u8(c.old_value) + wxString::FromUTF8("  \xE2\x86\x92  ") + from_u8(c.new_value), // U+2192
                              Label::Head_13);
        auto* chevron = label(m_list, CHEVRON_CLOSED, Label::Head_14, SECONDARY_COLOR);
        chevron->SetCursor(wxCursor(wxCURSOR_HAND));

        wxString details_text = c.comment.empty() ? wxString() : from_u8(c.comment) + "\n\n";
        details_text += (c.tooltip.empty() ? wxString() : from_u8(c.tooltip) + "\n") + _L("Parameter name") + ": " + from_u8(c.key);
        auto* details = label(m_list, details_text, Label::Body_12, SECONDARY_COLOR);
        details->Wrap(wrap);
        details->Hide();

        const wxString tip = tooltip_text(c);
        for (wxWindow* part : { (wxWindow*) check, (wxWindow*) title, (wxWindow*) values }) {
            if (!tip.empty())
                part->SetToolTip(tip);
            if (part != check)
                part->Bind(wxEVT_LEFT_DOWN, [check](wxMouseEvent&) { toggle(check); });
        }
        auto unfold = [this, chevron, details](wxMouseEvent&) {
            details->Show(!details->IsShown());
            chevron->SetLabel(details->IsShown() ? CHEVRON_OPEN : CHEVRON_CLOSED);
            fit_list();
        };
        chevron->Bind(wxEVT_LEFT_DOWN, unfold);
        details->Bind(wxEVT_LEFT_DOWN, unfold);
        check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            refresh_buttons();
            e.Skip();
        });

        grid->Add(check, wxGBPosition(row, 0), wxDefaultSpan, wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(6));
        grid->Add(title, wxGBPosition(row, 1), wxDefaultSpan, wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(6));
        grid->Add(values, wxGBPosition(row, 2), wxDefaultSpan, wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(6));
        grid->Add(chevron, wxGBPosition(row, 3), wxDefaultSpan, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT, FromDIP(6));
        grid->Add(details, wxGBPosition(row + 1, 1), wxGBSpan(1, 3), wxBOTTOM, FromDIP(6));
    }
    grid->AddGrowableCol(1);
    auto* list_sizer = new wxBoxSizer(wxVERTICAL);
    list_sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_list->SetSizer(list_sizer);
    sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    auto* buttons = new GUI::DialogButtons(this, { "Cancel", "Apply" }, _L("Apply"));
    m_apply       = buttons->GetAPPLY();
    sizer->Add(buttons, 0, wxEXPAND);
    SetSizer(sizer);
    fit_list();

    m_all->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        for (::CheckBox* check : m_checks)
            check->SetValue(m_all->GetValue());
        refresh_buttons();
        e.Skip();
    });
    buttons->GetAPPLY()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_APPLY); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    CenterOnParent();
    GUI::wxGetApp().UpdateDlgDarkUI(this);
}

std::vector<bool> McpChangesDialog::selected() const
{
    std::vector<bool> out;
    for (const ::CheckBox* check : m_checks)
        out.push_back(check->GetValue());
    return out;
}

void McpChangesDialog::on_dpi_changed(const wxRect&) { fit_list(); }

// Sizes the list to its content, scrolling only beyond most of the screen height.
void McpChangesDialog::fit_list()
{
    const int    display    = wxDisplay::GetFromWindow(this);
    const int    max_height = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea().GetHeight() * 6 / 10;
    const wxSize best       = m_list->GetSizer()->GetMinSize();
    m_list->SetMinSize(wxSize(best.x + FromDIP(20), std::min(best.y, max_height)));
    m_list->FitInside();
    // Grows with unfolded details, never shrinks below a width the user chose.
    const wxSize width = GetSize();
    Fit();
    SetSize(std::max(width.x, GetSize().x), GetSize().y);
    Layout();
}

void McpChangesDialog::refresh_buttons()
{
    const auto checked = std::count_if(m_checks.begin(), m_checks.end(), [](const ::CheckBox* c) { return c->GetValue(); });
    m_all->SetValue(checked == long(m_checks.size()));
    m_apply->Enable(checked > 0);
}

}} // namespace Slic3r::AIAssistant
