#include "McpActions.hpp"
#include "McpIssues.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <wx/utils.h>

#include <cstdlib>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::wxGetApp;
namespace fs = boost::filesystem;

// ------------------------------------------------------------ confirmation

std::optional<bool> test_confirm_answer()
{
#ifdef SNAPMAKER_AI_TEST_HOOKS
    const char* answer = std::getenv("SNAPMAKER_AI_TEST_CONFIRM");
    if (answer != nullptr && std::string(answer) == "yes")
        return true;
    if (answer != nullptr && std::string(answer) == "no")
        return false;
#endif
    return std::nullopt;
}

void show_test_mode_banner()
{
    const std::optional<bool> answer = test_confirm_answer();
    if (!answer || wxGetApp().plater() == nullptr)
        return;
    wxGetApp().plater()->get_notification_manager()->push_notification(
        NotificationType::CustomNotification, GUI::NotificationManager::NotificationLevel::ErrorNotificationLevel,
        std::string("AI assistant TEST MODE: every confirmation is answered '") + (*answer ? "yes" : "no") +
            "' without being shown. Debug build, never for real use.");
}

bool confirm(const std::string& reason, const std::vector<std::string>& lines, const wxString& question)
{
    std::string text = reason.empty() ? std::string() : reason + "\n\n";
    for (const std::string& line : lines)
        text += "- " + line + "\n";
    if (const std::optional<bool> answer = test_confirm_answer()) {
        BOOST_LOG_TRIVIAL(info) << "MCP test mode: answered " << (*answer ? "yes" : "no") << " to: " << text
                                << question.ToUTF8().data();
        return *answer;
    }
    GUI::MessageDialog dlg(wxGetApp().mainframe, from_u8(text) + (lines.empty() ? "" : "\n") + question, _L("AI Assistant"),
                           wxYES_NO | wxICON_QUESTION);
    return dlg.ShowModal() == wxID_YES;
}

// ------------------------------------------------------------ results

json done(json extra)
{
    json out = { { "done", true } };
    out.update(extra);
    return out;
}

json declined()
{
    return { { "done", false }, { "declined", true }, { "message", "The user declined. Ask them what they want instead." } };
}

json timed_out()
{
    return { { "done", false },
             { "timed_out", true },
             { "message", "The request timed out before the user answered: nothing was changed" } };
}

json operations_failed(const json& applied, const json& op, const std::string& error, size_t not_run)
{
    return { { "done", !applied.empty() },
             { "applied", applied },
             { "failed", { { "operation", op }, { "error", error } } },
             { "not_run", not_run } };
}

void add_issues_summary(json& out)
{
    try {
        out["issues"] = collect_issues()["summary"].get<std::string>() + " (details: get_issues)";
    } catch (const ToolRetry&) {
        out["issues"] = "unknown, the slicer is updating (call get_issues)";
    }
}

// ------------------------------------------------------------ references

int object_ref(const json& ref)
{
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    if (ref.is_number_integer()) {
        const int idx = ref.get<int>();
        if (idx < 1 || idx > int(objects.size()))
            throw ToolError("No object #" + std::to_string(idx) + ", the project has " + std::to_string(objects.size()));
        return idx - 1;
    }
    if (ref.is_string())
        return find_object(ref.get<std::string>());
    throw ToolError("'object' must be an object name or its 1-based index from get_project");
}

int part_ref(const ModelObject& obj, const json& ref)
{
    if (ref.is_number_integer()) {
        const int idx = ref.get<int>();
        if (idx < 1 || idx > int(obj.volumes.size()))
            throw ToolError("No part #" + std::to_string(idx) + " in '" + obj.name + "', it has " + std::to_string(obj.volumes.size()));
        return idx - 1;
    }
    if (!ref.is_string())
        throw ToolError("'part' must be a part name or its 1-based index from get_project");
    int found = -1;
    for (size_t i = 0; i < obj.volumes.size(); ++i) {
        if (obj.volumes[i]->name != ref.get<std::string>())
            continue;
        if (found >= 0)
            throw ToolError("Several parts are named '" + ref.get<std::string>() + "', use the part index");
        found = int(i);
    }
    if (found < 0)
        throw ToolError("No part named '" + ref.get<std::string>() + "' in '" + obj.name + "'");
    return found;
}

GUI::PartPlate& plate_ref(const json& op)
{
    GUI::PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    if (!op.contains("plate"))
        return *plates.get_curr_plate();
    const json& p = op["plate"];
    if (!p.is_number_integer() || p.get<int>() < 1 || p.get<int>() > plates.get_plate_count())
        throw ToolError("'plate' must be between 1 and " + std::to_string(plates.get_plate_count()));
    return *plates.get_plate(p.get<int>() - 1);
}

int filament_slot_count() { return int(wxGetApp().plater()->get_extruder_colors_from_plater_config().size()); }

int filament_slot_ref(const json& op, const char* key, bool allow_zero)
{
    if (!op.contains(key) || !op[key].is_number_integer())
        throw ToolError(std::string("'") + key + "' must be a 1-based filament slot" +
                        (allow_zero ? " (0: the object's filament, for a part)" : ""));
    const int slot = op[key].get<int>();
    if (slot < (allow_zero ? 0 : 1) || slot > filament_slot_count())
        throw ToolError("No filament slot " + std::to_string(slot) + ", the project has " + std::to_string(filament_slot_count()));
    return slot;
}

// ------------------------------------------------------------ files

fs::path absolute_path(const json& arg, const char* name)
{
    if (!arg.is_string() || arg.get<std::string>().empty())
        throw ToolError(std::string("'") + name + "' must be a non-empty string");
    std::string text = arg.get<std::string>();
    if (text.size() > 1 && text[0] == '~' && (text[1] == '/' || text[1] == '\\'))
        text = wxGetHomeDir().ToUTF8().data() + text.substr(1);
    const fs::path path(text);
    if (!path.is_absolute())
        throw ToolError("'" + text + "' is not an absolute path");
    return path;
}

fs::path existing_file(const json& arg, const std::set<std::string>& extensions)
{
    const fs::path            path = absolute_path(arg);
    boost::system::error_code ec;
    if (!fs::is_regular_file(path, ec))
        throw ToolError("'" + path.string() + "' is not an existing file");
    const std::string ext = boost::algorithm::to_lower_copy(path.extension().string());
    if (extensions.count(ext) == 0) {
        std::string allowed;
        for (const std::string& e : extensions)
            allowed += (allowed.empty() ? "" : ", ") + e;
        throw ToolError("'" + path.string() + "': unsupported file type (supported: " + allowed + ")");
    }
    return path;
}

fs::path output_file(const json& arg, const std::string& ext, bool overwrite)
{
    const fs::path path = absolute_path(arg);
    if (boost::algorithm::to_lower_copy(path.extension().string()) != ext)
        throw ToolError("'" + path.string() + "' must end with " + ext);
    boost::system::error_code ec;
    if (!fs::is_directory(path.parent_path(), ec))
        throw ToolError("The folder of '" + path.string() + "' does not exist");
    if (fs::exists(path, ec)) {
        if (!fs::is_regular_file(path, ec))
            throw ToolError("'" + path.string() + "' is not a file");
        if (!overwrite)
            throw ToolError("'" + path.string() + "' already exists: pass overwrite=true to replace it, after asking the user");
    }
    return path;
}

}} // namespace Slic3r::AIAssistant
