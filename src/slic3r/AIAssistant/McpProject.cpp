#include "McpProject.hpp"
#include "McpIssues.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <wx/utils.h>

#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::into_u8;
using GUI::wxGetApp;
namespace fs = boost::filesystem;

namespace {

bool confirm(const std::string& reason, const wxString& question)
{
    const wxString text = (reason.empty() ? wxString() : from_u8(reason) + "\n\n") + question;
    GUI::MessageDialog dlg(wxGetApp().mainframe, text, _L("AI Assistant"), wxYES_NO | wxICON_QUESTION);
    return dlg.ShowModal() == wxID_YES;
}

json declined() { return { { "done", false }, { "message", "The user declined. Ask them what they want instead." } }; }

json timed_out() { return { { "done", false }, { "message", "The request timed out before the user answered" } }; }

// Name of an enum value in the project files ("Textured PEI Plate", "by object").
template<typename T> std::string enum_name(T value)
{
    for (const auto& [name, v] : ConfigOptionEnum<T>::get_enum_values())
        if (v == int(value))
            return name;
    return {};
}

template<typename T> T enum_value(const json& arg, const char* what)
{
    const std::string name = arg.is_string() ? arg.get<std::string>() : std::string();
    std::string       allowed;
    for (const auto& [key, v] : ConfigOptionEnum<T>::get_enum_values()) {
        if (boost::algorithm::iequals(key, name))
            return T(v);
        allowed += (allowed.empty() ? "" : ", ") + key;
    }
    throw ToolError(std::string("'") + what + "' must be one of: default, " + allowed);
}

// Absolute path of an existing file with one of the extensions; throws ToolError otherwise.
fs::path existing_file(const json& arg, const std::set<std::string>& extensions)
{
    if (!arg.is_string() || arg.get<std::string>().empty())
        throw ToolError("Each path must be a non-empty string");
    std::string text = arg.get<std::string>();
    if (text.size() > 1 && text[0] == '~' && (text[1] == '/' || text[1] == '\\'))
        text = wxGetHomeDir().ToUTF8().data() + text.substr(1);
    const fs::path path(text);
    if (!path.is_absolute())
        throw ToolError("'" + text + "' is not an absolute path");
    boost::system::error_code ec;
    if (!fs::is_regular_file(path, ec))
        throw ToolError("'" + text + "' is not an existing file");
    const std::string ext = boost::algorithm::to_lower_copy(path.extension().string());
    if (extensions.count(ext) == 0) {
        std::string allowed;
        for (const std::string& e : extensions)
            allowed += (allowed.empty() ? "" : ", ") + e;
        throw ToolError("'" + text + "': unsupported file type (supported: " + allowed + ")");
    }
    return path;
}

json project_summary(GUI::Plater& plater)
{
    json objects = json::array();
    for (const ModelObject* obj : plater.model().objects)
        objects.push_back(obj->name);
    json out = { { "project_name", into_u8(plater.get_project_name()) },
                 { "file", into_u8(plater.get_project_filename(".3mf")) },
                 { "unsaved_changes", plater.is_project_dirty() },
                 { "objects", std::move(objects) },
                 { "plates", plates_summary() } };
    return out;
}

GUI::PartPlate& plate_arg(GUI::PartPlateList& plates, const json& op)
{
    if (!op.contains("plate"))
        return *plates.get_curr_plate();
    const json& p = op["plate"];
    if (!p.is_number_integer() || p.get<int>() < 1 || p.get<int>() > plates.get_plate_count())
        throw ToolError("'plate' must be between 1 and " + std::to_string(plates.get_plate_count()));
    return *plates.get_plate(p.get<int>() - 1);
}

int plate_number(GUI::PartPlate& plate) { return plate.get_index() + 1; }

// Description of a plate operation for the user; checks its arguments.
std::string describe_plate_op(GUI::PartPlateList& plates, const json& op)
{
    const std::string action = op.value("action", std::string());
    if (action == "add")
        return _u8L("Add a plate");
    GUI::PartPlate&   plate  = plate_arg(plates, op);
    const std::string number = std::to_string(plate_number(plate));
    if (action == "select")
        return (boost::format(_u8L("Show plate %1%")) % number).str();
    if (action == "delete") {
        if (plates.get_plate_count() < 2)
            throw ToolError("The last plate cannot be deleted");
        if (!plate.get_objects_on_this_plate().empty())
            throw ToolError("Plate " + number + " holds objects: move them to another plate (edit_objects, move_to_plate) or "
                            "delete them first");
        return (boost::format(_u8L("Delete plate %1%")) % number).str();
    }
    if (action == "rename") {
        if (!op.contains("name") || !op["name"].is_string())
            throw ToolError("'rename' needs 'name'");
        return (boost::format(_u8L("Rename plate %1% to '%2%'")) % number % op["name"].get<std::string>()).str();
    }
    if (action == "set") {
        std::vector<std::string> parts;
        if (op.contains("bed_type")) {
            const bool def = op["bed_type"] == "default";
            if (!def)
                enum_value<BedType>(op["bed_type"], "bed_type");
            parts.push_back((boost::format(_u8L("bed type: %1%")) % (def ? _u8L("as the project") : op["bed_type"].get<std::string>())).str());
        }
        if (op.contains("print_sequence")) {
            const bool def = op["print_sequence"] == "default";
            if (!def)
                enum_value<PrintSequence>(op["print_sequence"], "print_sequence");
            parts.push_back((boost::format(_u8L("print sequence: %1%")) %
                             (def ? _u8L("as the process settings") : op["print_sequence"].get<std::string>()))
                                .str());
        }
        if (parts.empty())
            throw ToolError("'set' needs 'bed_type' or 'print_sequence'");
        std::string text = (boost::format(_u8L("Plate %1%")) % number).str() + ": ";
        for (size_t i = 0; i < parts.size(); ++i)
            text += (i == 0 ? "" : ", ") + parts[i];
        return text;
    }
    throw ToolError("Unknown action '" + action + "' (select, add, delete, rename or set)");
}

void run_plate_op(GUI::Plater& plater, const json& op)
{
    GUI::PartPlateList& plates = plater.get_partplate_list();
    const std::string   action = op.value("action", std::string());
    if (action == "add") {
        // As the Add plate button of the toolbar.
        plater.take_snapshot("add partplate");
        plates.create_plate();
        plater.select_plate(plates.get_plate_count() - 1);
        plater.sync_print_seq_warning_notification();
        plater.update();
        return;
    }
    GUI::PartPlate& plate = plate_arg(plates, op);
    const int       index = plate.get_index();
    if (action == "select") {
        plater.select_plate(index);
    } else if (action == "delete") {
        if (plater.delete_plate(index) != 0)
            throw ToolError("The slicer could not delete plate " + std::to_string(index + 1) + " (slicing in progress?)");
    } else if (action == "rename") {
        plate.set_plate_name(op["name"].get<std::string>());
        plater.set_plater_dirty(true);
    } else if (action == "set") {
        // As the plate settings dialog.
        if (op.contains("bed_type"))
            plate.set_bed_type(op["bed_type"] == "default" ? btDefault : enum_value<BedType>(op["bed_type"], "bed_type"));
        if (op.contains("print_sequence"))
            plate.set_print_seq(op["print_sequence"] == "default" ? PrintSequence::ByDefault :
                                                                     enum_value<PrintSequence>(op["print_sequence"], "print_sequence"));
        plater.sync_print_seq_warning_notification();
        plater.update_project_dirty_from_presets();
        plater.set_plater_dirty(true);
        plater.update();
    }
}

} // namespace

json plates_summary()
{
    GUI::PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    json                out    = json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        GUI::PartPlate* plate   = plates.get_plate(i);
        json            objects = json::array();
        for (const ModelObject* obj : plate->get_objects_on_this_plate())
            objects.push_back(obj->name);
        const BedType       bed = plate->get_bed_type(false);
        const PrintSequence seq = plate->get_print_seq();
        out.push_back({ { "plate", i + 1 },
                        { "name", plate->get_plate_name() },
                        { "current", i == plates.get_curr_plate_index() },
                        { "objects", std::move(objects) },
                        { "bed_type", bed == btDefault ? std::string("default") : enum_name(bed) },
                        { "print_sequence", seq == PrintSequence::ByDefault ? std::string("default") : enum_name(seq) },
                        { "sliced", plate->is_slice_result_valid() } });
    }
    return out;
}

json new_project(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater& plater = require_plater();
    if (!confirm(args.value("reason", std::string()),
                 _L("The AI assistant asks to start a new project. The current project is closed; you are asked to "
                    "save it first if it has unsaved changes.")))
        return declined();
    if (abandoned)
        return timed_out();
    if (plater.new_project() == wxID_CANCEL)
        return { { "done", false }, { "message", "The user kept the current project" } };
    return { { "done", true }, { "project", project_summary(plater) } };
}

json open_project(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater& plater = require_plater();
    wxString     file;
    if (args.contains("path")) {
        const fs::path path = existing_file(args["path"], { ".3mf" });
        file                = from_u8(path.string());
        if (!confirm(args.value("reason", std::string()),
                     wxString::Format(_L("The AI assistant asks to open the project %s. The current project is closed; "
                                         "you are asked to save it first if it has unsaved changes."),
                                      file)))
            return declined();
        if (abandoned)
            return timed_out();
    }
    // Without a path, the slicer asks for the file: choosing it is the confirmation.
    const wxString before = plater.get_project_filename(".3mf");
    plater.load_project(file);
    const wxString after = plater.get_project_filename(".3mf");
    if (after.empty() || (file.empty() && after == before))
        return { { "done", false }, { "message", "No project was opened (the user cancelled)" } };
    return { { "done", true }, { "project", project_summary(plater) } };
}

json import_models(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater&                 plater = require_plater();
    const size_t                 before = plater.model().objects.size();
    std::vector<fs::path>        paths;
    if (args.contains("paths")) {
        if (!args["paths"].is_array() || args["paths"].empty())
            throw ToolError("'paths' must list the files to import");
        for (const json& p : args["paths"])
            paths.push_back(existing_file(p, { ".stl", ".3mf", ".obj", ".step", ".stp", ".amf", ".svg" }));
        std::string list;
        for (const fs::path& p : paths)
            list += "- " + p.string() + "\n";
        if (!confirm(args.value("reason", std::string()),
                     from_u8(list) + "\n" + _L("The AI assistant asks to add these models to the current plate. Import them?")))
            return declined();
        if (abandoned)
            return timed_out();
        // Models only: the settings of an imported 3MF are not applied, as with the Import button.
        plater.load_files(paths, LoadStrategy::LoadModel);
    } else {
        // The slicer asks for the files: choosing them is the confirmation.
        plater.add_model();
    }

    json added = json::array();
    for (size_t i = before; i < plater.model().objects.size(); ++i)
        added.push_back({ { "index", i + 1 }, { "name", plater.model().objects[i]->name } });
    if (added.empty())
        return { { "done", false }, { "message", "No model was imported (the user cancelled, or the files hold no model)" } };
    json out = { { "done", true }, { "added", std::move(added) } };
    try {
        out["issues"] = collect_issues()["summary"].get<std::string>() + " (details: get_issues)";
    } catch (const ToolRetry&) {
        out["issues"] = "unknown, the slicer is updating (call get_issues)";
    }
    return out;
}

json save_project(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater&   plater  = require_plater();
    const wxString file    = plater.get_project_filename(".3mf");
    const bool     save_as = args.value("save_as", false) || file.empty();
    // Saving as goes through the slicer's file dialog, which is the confirmation.
    if (!save_as) {
        if (!confirm(args.value("reason", std::string()),
                     wxString::Format(_L("The AI assistant asks to save the project to %s. Save it?"), file)))
            return declined();
        if (abandoned)
            return timed_out();
    }
    const int result = plater.save_project(save_as);
    if (result != wxID_YES)
        return { { "done", false }, { "message", "The project was not saved (the user cancelled, or saving failed)" } };
    return { { "done", true }, { "file", into_u8(plater.get_project_filename(".3mf")) } };
}

json export_file(const json& args, const std::atomic<bool>& /* abandoned */)
{
    GUI::Plater&      plater = require_plater();
    const std::string format = args.value("format", std::string());
    if (format != "gcode" && format != "sliced_3mf" && format != "stl")
        throw ToolError("'format' must be gcode, sliced_3mf or stl (save_project writes the 3MF project)");
    if (plater.model().objects.empty())
        throw ToolError("The project has no object to export");
    // The slicer's save dialog is the confirmation; it writes the file in the background.
    if (format == "gcode" || format == "sliced_3mf") {
        if (!plater.get_partplate_list().get_curr_plate()->is_slice_result_valid())
            throw ToolError("The current plate is not sliced: call slice first");
        if (format == "gcode")
            plater.export_gcode(false);
        else
            plater.export_gcode_3mf(false);
    } else {
        plater.export_stl(false, false, false);
    }
    return { { "done", true },
             { "message", "The slicer showed its save dialog: the file is written unless the user cancelled it" } };
}

json edit_plates(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater& plater = require_plater();
    if (!args.contains("operations") || !args["operations"].is_array() || args["operations"].empty())
        throw ToolError("'operations' must list at least one operation");
    if (plater.is_background_process_slicing())
        throw ToolError("The slicer is slicing: try again once it is done");

    GUI::PartPlateList&      plates = plater.get_partplate_list();
    std::vector<std::string> lines;
    bool                     changes = false;
    bool                     shifted = false;
    for (const json& op : args["operations"]) {
        if (!op.is_object())
            throw ToolError("Each operation must be an object");
        const std::string action = op.value("action", std::string());
        // Adding and deleting renumber the plates: the later operations are checked when they run.
        lines.push_back(shifted ? action + (op.contains("plate") ? " " + op["plate"].dump() : std::string()) :
                                  describe_plate_op(plates, op));
        changes = changes || action != "select";
        shifted = shifted || action == "add" || action == "delete";
    }

    if (changes) {
        std::string list;
        for (const std::string& line : lines)
            list += "- " + line + "\n";
        if (!confirm(args.value("reason", std::string()),
                     from_u8(list) + "\n" + _L("The AI assistant asks to change the plates of the project. Apply these operations?")))
            return declined();
        if (abandoned)
            return timed_out();
    }

    json done = json::array();
    for (const json& op : args["operations"]) {
        try {
            describe_plate_op(plates, op);
            run_plate_op(plater, op);
        } catch (const ToolError& e) {
            return { { "done", !done.empty() }, { "applied", done }, { "failed", { { "operation", op }, { "error", e.what() } } },
                     { "plates", plates_summary() } };
        }
        done.push_back(op);
    }
    return { { "done", true }, { "plates", plates_summary() } };
}

json start_print(const json& /* args */)
{
    GUI::Plater&      plater   = require_plater();
    const McpSettings settings = McpSettings::load();
    if (!settings.printer_access || !settings.printer_control)
        throw ToolError("printer control is disabled in the slicer (Help > AI Assistant (MCP server)). Ask the user to "
                        "enable it, or to start the print with the Print plate button.");
    GUI::PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (!plate->is_slice_result_valid())
        throw ToolError("The current plate is not sliced: call slice first, and check the result with the user");
    // Same entry point as the Print plate button: its dialogs let the user check the
    // file and the filament mapping, then start the print.
    wxPostEvent(&plater, GUI::SimpleEvent(GUI::EVT_GLTOOLBAR_PRINT_PLATE));
    return { { "opened", true },
             { "plate", plate->get_index() + 1 },
             { "message", "The slicer shows its print dialog for this plate: the user checks the file name and the filament "
                          "mapping, then starts the print. Follow it with get_printer_status and watch_print." } };
}

}} // namespace Slic3r::AIAssistant
