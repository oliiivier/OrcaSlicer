#include "McpProject.hpp"
#include "McpActions.hpp"
#include "McpIssues.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/calib.hpp"
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

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::into_u8;
using GUI::wxGetApp;
namespace fs = boost::filesystem;

namespace {

// ------------------------------------------------------------ layer events

// Top of each layer of the sliced current plate, lowest first: the heights of its
// extrusions, as the layer slider of the preview lists them.
std::vector<double> layer_heights(GUI::Plater& plater, GUI::PartPlate& plate)
{
    const GCodeProcessorResult* result = plate.get_slice_result();
    if (plate.get_index() != plater.get_partplate_list().get_curr_plate_index() || !plate.is_slice_result_valid() || result == nullptr)
        throw ToolError("Layer events need the layers of the plate: select it (edit_plates select) and slice it first");
    std::vector<double> zs;
    for (const GCodeProcessorResult::MoveVertex& move : result->moves)
        if (move.type == EMoveType::Extrude)
            zs.push_back(std::round(double(move.position.z()) * 1000.) / 1000.);
    std::sort(zs.begin(), zs.end());
    zs.erase(std::unique(zs.begin(), zs.end(), [](double a, double b) { return std::abs(a - b) < 0.002; }), zs.end());
    if (zs.empty())
        throw ToolError("The plate has no layer: slice it first");
    return zs;
}

// Layer of op (1-based 'layer', or the layer whose top is closest to 'height_mm'): its index in `zs`.
size_t layer_arg(const json& op, const std::vector<double>& zs)
{
    if (op.contains("layer")) {
        if (!op["layer"].is_number_integer() || op["layer"].get<int>() < 1 || op["layer"].get<size_t>() > zs.size())
            throw ToolError("'layer' must be between 1 and " + std::to_string(zs.size()));
        return op["layer"].get<size_t>() - 1;
    }
    if (!op.contains("height_mm") || !op["height_mm"].is_number())
        throw ToolError("A layer event needs 'layer' (1-based) or 'height_mm'");
    const double height = op["height_mm"].get<double>();
    size_t       best   = 0;
    for (size_t i = 1; i < zs.size(); ++i)
        if (std::abs(zs[i] - height) < std::abs(zs[best] - height))
            best = i;
    return best;
}

std::string event_type_name(CustomGCode::Type type)
{
    return type == CustomGCode::PausePrint ? "pause" : type == CustomGCode::ToolChange ? "filament_change" :
           type == CustomGCode::ColorChange ? "color_change" : type == CustomGCode::Custom ? "custom_gcode" : "other";
}

json layer_events(int plate_idx)
{
    json        out    = json::array();
    const auto& gcodes = wxGetApp().model().plates_custom_gcodes;
    if (const auto it = gcodes.find(plate_idx); it != gcodes.end())
        for (const CustomGCode::Item& item : it->second.gcodes) {
            json e = { { "height_mm", std::round(item.print_z * 1000.) / 1000. }, { "type", event_type_name(item.type) } };
            if (item.type == CustomGCode::ToolChange || item.type == CustomGCode::ColorChange)
                e["filament_slot"] = item.extruder;
            out.push_back(std::move(e));
        }
    return out;
}

// ------------------------------------------------------------ calibrations

double calib_number(const json& args, const char* name)
{
    if (!args.contains(name) || !args[name].is_number())
        throw ToolError(std::string("'") + name + "' must be a number");
    return args[name].get<double>();
}

// start, end and step of a tower, checked as the calibration dialogs do.
void calib_range(const json& args, Calib_Params& params, double min_start)
{
    params.start = calib_number(args, "start");
    params.end   = calib_number(args, "end");
    params.step  = calib_number(args, "step");
    if (params.start < min_start || params.step < EPSILON || params.end < params.start + params.step)
        throw ToolError("Needs start >= " + float_to_string_decimal_point(min_start, 3) + ", step > 0 and end >= start + step");
}

std::vector<double> number_list(const json& args, const char* name)
{
    std::vector<double> out;
    if (!args.contains(name))
        return out;
    if (!args[name].is_array())
        throw ToolError(std::string("'") + name + "' must be a list of numbers");
    for (const json& v : args[name]) {
        if (!v.is_number() || v.get<double>() <= 0.)
            throw ToolError(std::string("'") + name + "' must be a list of positive numbers");
        out.push_back(v.get<double>());
    }
    return out;
}

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
    if (action == "add_layer_event") {
        const std::vector<double> zs   = layer_heights(*wxGetApp().plater(), plate);
        const size_t              i    = layer_arg(op, zs);
        const std::string         type = op.value("type", std::string());
        const std::string         where = (boost::format(_u8L("Plate %1%, layer %2% (%3% mm)")) % number % (i + 1) %
                                           float_to_string_decimal_point(zs[i], 2)).str();
        if (type == "pause")
            return where + ": " + _u8L("pause the print");
        if (type == "filament_change") {
            if (filament_slot_count() < 2)
                throw ToolError("A filament change needs at least two filament slots (edit_presets add_slot)");
            return where + ": " + (boost::format(_u8L("change to filament %1%")) % filament_slot_ref(op, "filament_slot")).str();
        }
        throw ToolError("'type' must be pause or filament_change (custom G-code is not allowed)");
    }
    if (action == "remove_layer_event") {
        const std::vector<double> zs = layer_heights(*wxGetApp().plater(), plate);
        const size_t              i  = layer_arg(op, zs);
        const auto&               gcodes = wxGetApp().model().plates_custom_gcodes;
        const auto                it     = gcodes.find(plate.get_index());
        if (it == gcodes.end() ||
            std::none_of(it->second.gcodes.begin(), it->second.gcodes.end(),
                         [&](const CustomGCode::Item& item) { return std::abs(item.print_z - zs[i]) < EPSILON; }))
            throw ToolError("No layer event at layer " + std::to_string(i + 1) + " of plate " + number + " (see get_project.plates[].layer_events)");
        return (boost::format(_u8L("Plate %1%, layer %2% (%3% mm)")) % number % (i + 1) % float_to_string_decimal_point(zs[i], 2)).str() +
               ": " + _u8L("remove the event");
    }
    throw ToolError("Unknown action '" + action + "' (select, add, delete, rename, set, add_layer_event or remove_layer_event)");
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
    } else if (action == "add_layer_event" || action == "remove_layer_event") {
        // As the + and - of the layer slider of the preview.
        const std::vector<double> zs   = layer_heights(plater, plate);
        const double              z    = zs[layer_arg(op, zs)];
        CustomGCode::Info&        info = plater.model().plates_custom_gcodes[index];
        auto& gcodes = info.gcodes;
        gcodes.erase(std::remove_if(gcodes.begin(), gcodes.end(), [z](const CustomGCode::Item& item) { return std::abs(item.print_z - z) < EPSILON; }),
                     gcodes.end());
        if (action == "add_layer_event") {
            CustomGCode::Item item { z, CustomGCode::PausePrint, 0, "", "" };
            if (op.value("type", std::string()) == "filament_change") {
                const int   slot   = filament_slot_ref(op, "filament_slot");
                const auto* colors = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
                item.type          = CustomGCode::ToolChange;
                item.extruder      = slot;
                item.color         = colors != nullptr && size_t(slot) <= colors->values.size() ? colors->values[slot - 1] : "";
            }
            gcodes.push_back(item);
            std::sort(gcodes.begin(), gcodes.end());
        }
        CustomGCode::check_mode_for_custom_gcode_per_print_z(info);
        plate.update_slice_result_valid_state(false);
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
                        { "sliced", plate->is_slice_result_valid() },
                        { "layer_events", layer_events(i) } });
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

json calibrate(const json& args, const std::atomic<bool>& abandoned)
{
    GUI::Plater&      plater = require_plater();
    const std::string action = args.contains("action") && args["action"].is_string() ? args["action"].get<std::string>() : std::string();
    Calib_Params      params;
    std::string       line;
    // The parameters of each calibration dialog of the Calibration menu, with the same checks.
    if (action == "temperature") {
        params.mode  = CalibMode::Calib_Temp_Tower;
        params.start = std::round(calib_number(args, "start"));
        params.end   = std::round(calib_number(args, "end"));
        if (params.start > 350 || params.end < 170 || params.end > params.start - 5)
            throw ToolError("Needs start <= 350 °C, end >= 170 °C and start > end + 5 (the tower goes from hot to cold)");
        line = (boost::format(_u8L("Temperature tower from %1% to %2% °C")) % params.start % params.end).str();
    } else if (action == "flow") {
        const std::string method = args.value("method", std::string("classic"));
        const int         pass   = args.value("pass", 1);
        if ((method != "classic" && method != "yolo") || (pass != 1 && pass != 2))
            throw ToolError("'method' must be classic or yolo and 'pass' 1 or 2");
        line = (boost::format(_u8L("Flow rate test (%1%, pass %2%)")) % method % pass).str();
        params.mode  = CalibMode::Calib_Flow_Rate;
        params.start = pass;
        params.print_numbers = method == "yolo";
    } else if (action == "pressure_advance") {
        const std::string method = args.value("method", std::string("line"));
        params.mode = method == "line" ? CalibMode::Calib_PA_Line : method == "pattern" ? CalibMode::Calib_PA_Pattern :
                      method == "tower" ? CalibMode::Calib_PA_Tower : CalibMode::Calib_None;
        if (params.mode == CalibMode::Calib_None)
            throw ToolError("'method' must be line, pattern or tower");
        calib_range(args, params, 0.);
        params.print_numbers = args.value("print_numbers", true);
        params.accelerations = number_list(args, "accelerations");
        params.speeds        = number_list(args, "speeds");
        line = (boost::format(_u8L("Pressure advance (%1%) from %2% to %3%, step %4%")) % method %
                float_to_string_decimal_point(params.start, 4) % float_to_string_decimal_point(params.end, 4) %
                float_to_string_decimal_point(params.step, 4)).str();
    } else if (action == "retraction" || action == "max_volumetric_speed" || action == "vfa") {
        params.mode = action == "retraction" ? CalibMode::Calib_Retraction_tower :
                      action == "vfa"        ? CalibMode::Calib_VFA_Tower : CalibMode::Calib_Vol_speed_Tower;
        calib_range(args, params, action == "retraction" ? 0. : action == "vfa" ? 10. + EPSILON : EPSILON);
        const std::string what = action == "retraction" ? _u8L("Retraction tower (mm)") :
                                 action == "vfa"        ? _u8L("VFA tower (mm/s)") : _u8L("Max volumetric speed test (mm³/s)");
        line = what + ": " + float_to_string_decimal_point(params.start, 3) + " - " + float_to_string_decimal_point(params.end, 3) +
               ", " + float_to_string_decimal_point(params.step, 3);
    } else
        throw ToolError("'action' must be temperature, flow, pressure_advance, retraction, max_volumetric_speed or vfa");

    if (!confirm(args.value("reason", std::string()), { line },
                 _L("The AI assistant asks to create this calibration project, as the Calibration menu does. The current "
                    "project is closed; you are asked to save it first if it has unsaved changes. Create it?")))
        return declined();
    if (abandoned)
        return timed_out();
    // The test mode cannot answer the slicer's own question about unsaved changes: they are dropped.
    if (test_confirm_answer())
        plater.new_project(true);

    switch (params.mode) {
    case CalibMode::Calib_Temp_Tower: plater.calib_temp(params); break;
    case CalibMode::Calib_Flow_Rate: plater.calib_flowrate(params.print_numbers, int(params.start)); break;
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
    case CalibMode::Calib_PA_Tower: plater.calib_pa(params); break;
    case CalibMode::Calib_Retraction_tower: plater.calib_retraction(params); break;
    case CalibMode::Calib_VFA_Tower: plater.calib_VFA(params); break;
    default: plater.calib_max_vol_speed(params); break;
    }
    return done({ { "project", project_summary(plater) },
                  { "next", "Check it with get_issues, slice, and let the user print it. Then read the matching calibration "
                            "guide (get_docs page) to tell the user how to read the result and which setting to change." } });
}

}} // namespace Slic3r::AIAssistant
