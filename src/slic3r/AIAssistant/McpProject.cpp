#include "McpProject.hpp"
#include "McpActions.hpp"
#include "McpIssues.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
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

// Confirmation line telling that the file at `path` exists and will be replaced.
std::vector<std::string> replaced_file_line(const fs::path& path)
{
    boost::system::error_code ec;
    if (!fs::exists(path, ec))
        return {};
    return { _u8L("The existing file is replaced.") };
}

int plate_number(GUI::PartPlate& plate) { return plate.get_index() + 1; }

// Description of a plate operation for the user; checks its arguments.
std::string describe_plate_op(GUI::PartPlateList& plates, const json& op)
{
    const std::string action = op.value("action", std::string());
    if (action == "add")
        return _u8L("Add a plate");
    GUI::PartPlate&   plate  = plate_ref(op);
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
    GUI::PartPlate& plate = plate_ref(op);
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
    if (!confirm(args.value("reason", std::string()), {},
                 _L("The AI assistant asks to start a new project. The current project is closed; you are asked to "
                    "save it first if it has unsaved changes.")))
        return declined();
    if (abandoned)
        return timed_out();
    // The test mode cannot answer the slicer's own question about unsaved changes: they are dropped.
    if (plater.new_project(test_confirm_answer().has_value()) == wxID_CANCEL)
        return { { "done", false }, { "message", "The user kept the current project" } };
    return done({ { "project", project_summary(plater) } });
}

json open_project(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater& plater = require_plater();
    wxString     file;
    if (args.contains("path")) {
        const fs::path path = existing_file(args["path"], { ".3mf" });
        file                = from_u8(path.string());
        if (!confirm(args.value("reason", std::string()), {},
                     wxString::Format(_L("The AI assistant asks to open the project %s. The current project is closed; "
                                         "you are asked to save it first if it has unsaved changes."),
                                      file)))
            return declined();
        if (abandoned)
            return timed_out();
    }
    // Without a path, the slicer asks for the file: choosing it is the confirmation.
    const wxString before = plater.get_project_filename(".3mf");
    // The test mode cannot answer the slicer's own question about unsaved changes: they are dropped.
    if (test_confirm_answer() && !file.empty())
        plater.new_project(true);
    plater.load_project(file);
    const wxString after = plater.get_project_filename(".3mf");
    if (after.empty() || (file.empty() && after == before))
        return { { "done", false }, { "message", "No project was opened (the user cancelled)" } };
    return done({ { "project", project_summary(plater) } });
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
        std::vector<std::string> lines;
        for (const fs::path& p : paths)
            lines.push_back(p.string());
        if (!confirm(args.value("reason", std::string()), lines,
                     _L("The AI assistant asks to add these models to the current plate. Import them?")))
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
    json out = done({ { "added", std::move(added) } });
    add_issues_summary(out);
    return out;
}

json save_project(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater&      plater = require_plater();
    const std::string reason = args.value("reason", std::string());
    if (args.contains("path")) {
        const fs::path path = output_file(args["path"], ".3mf", args.value("overwrite", false));
        if (!confirm(reason, replaced_file_line(path),
                     wxString::Format(_L("The AI assistant asks to save the project to %s. Save it?"), from_u8(path.string()))))
            return declined();
        if (abandoned)
            return timed_out();
        // As Plater::save_project() once the file is chosen.
        if (plater.export_3mf(path, SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::FullPathSources) < 0)
            throw ToolError("The slicer could not write '" + path.string() + "'");
        plater.set_project_filename(from_u8(path.string()));
        plater.up_to_date(true, false);
        plater.up_to_date(true, true);
        wxGetApp().update_saved_preset_from_current_preset();
        plater.reset_project_dirty_after_save();
        plater.update_title_dirty_status();
        return done({ { "file", path.string() } });
    }

    const wxString file    = plater.get_project_filename(".3mf");
    const bool     save_as = args.value("save_as", false) || file.empty();
    // Saving as goes through the slicer's file dialog, which is the confirmation.
    if (!save_as) {
        if (!confirm(reason, {}, wxString::Format(_L("The AI assistant asks to save the project to %s. Save it?"), file)))
            return declined();
        if (abandoned)
            return timed_out();
    }
    const int result = plater.save_project(save_as);
    if (result != wxID_YES)
        return { { "done", false }, { "message", "The project was not saved (the user cancelled, or saving failed)" } };
    return done({ { "file", into_u8(plater.get_project_filename(".3mf")) } });
}

json export_file(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater&      plater = require_plater();
    const std::string format = args.value("format", std::string());
    if (format != "gcode" && format != "sliced_3mf" && format != "stl")
        throw ToolError("'format' must be gcode, sliced_3mf or stl (save writes the 3MF project)");
    if (plater.model().objects.empty())
        throw ToolError("The project has no object to export");
    GUI::PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (format != "stl" && !plate->is_slice_result_valid())
        throw ToolError("The current plate is not sliced: call slice first");

    if (!args.contains("path")) {
        // The slicer's save dialog is the confirmation; it writes the file in the background.
        if (format == "gcode")
            plater.export_gcode(false);
        else if (format == "sliced_3mf")
            plater.export_gcode_3mf(false);
        else
            plater.export_stl(false, false, false);
        return { { "done", true },
                 { "message", "The slicer showed its save dialog: the file is written unless the user cancelled it" } };
    }

    const std::string ext  = format == "gcode" ? ".gcode" : format == "stl" ? ".stl" : ".3mf";
    const fs::path    path = output_file(args["path"], ext, args.value("overwrite", false));
    const wxString    question =
        format == "gcode"      ? _L("The AI assistant asks to export the G-code of the current plate to %s. Export it?") :
        format == "sliced_3mf" ? _L("The AI assistant asks to export the sliced current plate (3MF with its G-code) to %s. Export it?") :
                                 _L("The AI assistant asks to export the objects as STL to %s. Export them?");
    if (!confirm(args.value("reason", std::string()), replaced_file_line(path), wxString::Format(question, from_u8(path.string()))))
        return declined();
    if (abandoned)
        return timed_out();

    if (format == "gcode") {
        // The G-code of the plate as sliced, like the export without post-processing scripts.
        std::string error;
        if (copy_file(plate->get_tmp_gcode_path(), path.string(), error) != SUCCESS)
            throw ToolError("Could not write '" + path.string() + "': " + error);
    } else if (format == "sliced_3mf") {
        // As Plater::export_gcode_3mf() once the file is chosen.
        if (plater.export_3mf(path, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel,
                              plate->get_index()) < 0)
            throw ToolError("The slicer could not write '" + path.string() + "'");
    } else {
        // As Plater::export_stl(): one mesh holding every object and instance.
        TriangleMesh mesh;
        for (const ModelObject* obj : plater.model().objects)
            mesh.merge(GUI::Plater::combine_mesh_fff(*obj, -1));
        if (!store_stl(path.string().c_str(), &mesh, true))
            throw ToolError("Could not write '" + path.string() + "'");
    }
    return done({ { "file", path.string() } });
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
        if (!confirm(args.value("reason", std::string()), lines,
                     _L("The AI assistant asks to change the plates of the project. Apply these operations?")))
            return declined();
        if (abandoned)
            return timed_out();
    }

    const json& ops     = args["operations"];
    json        applied = json::array();
    for (size_t i = 0; i < ops.size(); ++i) {
        try {
            describe_plate_op(plates, ops[i]);
            run_plate_op(plater, ops[i]);
        } catch (const ToolError& e) {
            json out      = operations_failed(applied, ops[i], e.what(), ops.size() - i - 1);
            out["plates"] = plates_summary();
            return out;
        }
        applied.push_back(ops[i]);
    }
    return done({ { "applied", std::move(applied) }, { "plates", plates_summary() } });
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
                          "mapping, then starts the print. Follow it with get_printer (status, then watch)." } };
}

}} // namespace Slic3r::AIAssistant
