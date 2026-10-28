#pragma once

#include "slic3r/GUI/GUI_Utils.hpp"

#include <string>
#include <vector>

class CheckBox;
class wxScrolledWindow;

namespace Slic3r { namespace AIAssistant {

// One setting change proposed by the assistant, as shown to the user.
struct ProposedChange
{
    std::string label;     // setting label and target (object, slot, toolhead)
    std::string key;
    std::string old_value;
    std::string new_value;
    std::string tooltip;   // help text of the setting
    std::string comment;   // assistant's reason for this change, may be empty
};

// Lists the proposed changes one per line with a check box each, all checked:
// the user applies them at once or unchecks the ones to keep. The chevron of a
// line unfolds the assistant's comment and the help of the setting, also shown
// when hovering the line.
// Built from the slicer's own widgets so that it follows the light and dark themes.
class McpChangesDialog : public GUI::DPIDialog
{
public:
    McpChangesDialog(wxWindow* parent, const std::string& reason, const std::vector<ProposedChange>& changes);

    // Valid after ShowModal() returned wxID_APPLY.
    std::vector<bool> selected() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void refresh_buttons();
    void fit_list();

    std::vector<::CheckBox*> m_checks;
    ::CheckBox*              m_all { nullptr };
    wxScrolledWindow*        m_list { nullptr };
    wxWindow*                m_apply { nullptr };
};

}} // namespace Slic3r::AIAssistant
