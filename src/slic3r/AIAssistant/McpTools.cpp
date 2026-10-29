#include "McpTools.hpp"
#include "McpChanges.hpp"
#include "McpDocs.hpp"
#include "McpExperiences.hpp"
#include "McpIssues.hpp"
#include "McpManufacturerDocs.hpp"
#include "McpMaterials.hpp"
#include "McpNotes.hpp"
#include "McpObjects.hpp"
#include "McpPrinter.hpp"
#include "McpPrinterControl.hpp"
#include "McpPrinterLive.hpp"
#include "McpPresets.hpp"
#include "McpProject.hpp"
#include "McpRender.hpp"
#include "McpSlicing.hpp"
#include "McpToolUtils.hpp"
#include "ProjectAdapt.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;
using namespace std::chrono_literals;

namespace {

json text_result(const std::string& text, bool is_error = false)
{
    return { { "content", json::array({ { { "type", "text" }, { "text", text } } }) }, { "isError", is_error } };
}

// ---------------------------------------------------------------- tools

json get_project(const json& /* args */)
{
    GUI::Plater&        p      = require_plater();
    const PresetBundle& bundle = *wxGetApp().preset_bundle;

    json objects = json::array();
    for (size_t idx = 0; idx < p.model().objects.size(); ++idx) {
        const ModelObject* obj = p.model().objects[idx];
        json overrides = json::object();
        for (const std::string& key : obj->config.keys())
            overrides[key] = obj->config.opt_serialize(key);

        json o = { { "index", idx + 1 },
                   { "name", obj->name },
                   { "instances", obj->instances.size() },
                   { "parts", object_parts(*obj) },
                   { "filaments", object_filaments(*obj) },
                   { "setting_overrides", std::move(overrides) },
                   { "height_ranges", object_height_ranges(*obj) } };
        o.update(object_geometry(idx));
        objects.push_back(std::move(o));
    }

    json out = { { "project_name", GUI::into_u8(p.get_project_name()) },
                 { "presets", presets_summary() },
                 { "materials", describe_materials() },
                 { "objects", std::move(objects) },
                 { "plates", plates_summary() } };
    try {
        out["issues"] = collect_issues()["summary"].get<std::string>() + " (details: get_issues)";
    } catch (const ToolRetry&) {
        out["issues"] = "unknown, the slicer is updating (call get_issues)";
    }
    return out;
}

json get_config(const json& args)
{
    require_plater();
    const std::vector<std::string> keys   = string_list(args, "keys");
    const std::string              search = boost::algorithm::to_lower_copy(args.value("search", std::string()));
    const DynamicPrintConfig       config = wxGetApp().preset_bundle->full_config();

    json values = json::object();
    if (!keys.empty()) {
        json unknown = json::array();
        for (const std::string& key : keys) {
            if (const ConfigOption* opt = config.option(key))
                values[key] = option_value(*opt);
            else
                unknown.push_back(key);
        }
        json out = { { "values", std::move(values) } };
        if (!unknown.empty())
            out["unknown_keys"] = std::move(unknown);
        return out;
    }

    for (const std::string& key : config.keys()) {
        const ConfigOptionDef* def = print_config_def.get(key);
        // G-code templates and other long texts are only returned when asked for by key.
        if (def != nullptr && (def->is_code || def->multiline))
            continue;
        if (!search.empty()) {
            const std::string label = def != nullptr ? boost::algorithm::to_lower_copy(def->label) : std::string();
            if (!boost::algorithm::contains(key, search) && !boost::algorithm::contains(label, search))
                continue;
        }
        values[key] = option_value(*config.option(key));
    }
    return { { "values", std::move(values) } };
}

json describe_setting(const json& args)
{
    require_plater();
    const std::vector<std::string> keys = string_list(args, "keys");
    if (keys.empty())
        throw ToolError("'keys' must list at least one setting");
    const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();

    json out = json::object();
    for (const std::string& key : keys) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr) {
            out[key] = { { "error", "unknown setting" } };
            continue;
        }
        json d = setting_summary(*def);
        if (const ConfigOption* opt = config.option(key))
            d["current"] = option_value(*opt);
        json doc         = setting_documentation(key);
        d["documentation"] = doc.is_null() ? json(no_documentation_note(!def->tooltip.empty())) : std::move(doc);
        out[key] = std::move(d);
    }
    return out;
}

// ---------------------------------------------------------------- registry

struct ToolContext
{
    // Set when the client stopped waiting: nothing must be changed anymore.
    const std::atomic<bool>& abandoned;
    // Kept between the calls of a tool retried with ToolRetry.
    json& state;
};

using ToolFn = std::function<json(const json& args, ToolContext& ctx)>;

// Key marking a tool result that is already a list of MCP content items (e.g. an
// image) instead of data to be sent as JSON text.
const char* MCP_CONTENT = "$mcp_content";

// Adapts a tool that needs neither the retry state nor to know whether the client stopped waiting.
ToolFn simple(json (*fn)(const json&))
{
    return [fn](const json& args, ToolContext&) { return fn(args); };
}

json get_issues(const json& /* args */) { return collect_issues(); }

json render_view(const json& args) { return { { MCP_CONTENT, render_view_content(args) } }; }

struct Tool
{
    const char*               name;
    const char*               description;
    ToolFn                    run;
    json                      input_schema;
    // Time the GUI thread may take for one call (the user may have to confirm).
    std::chrono::milliseconds timeout;
    // Time during which a call answering ToolRetry is repeated.
    std::chrono::milliseconds retry_for { 5s };
    // Run on the GUI thread with the retry state when the calls stop before the
    // tool returned (retry window over, slicer busy, shutdown).
    std::function<void(json& state)> abandon {};
};

struct Action
{
    const char* name;
    ToolFn      run;
};

// A tool grouping several actions: runs the one named by args.action, or
// `default_action` without it.
ToolFn by_action(std::vector<Action> actions, const char* default_action = nullptr)
{
    return [actions = std::move(actions), default_action](const json& args, ToolContext& ctx) {
        std::string name = default_action != nullptr ? default_action : "";
        if (args.contains("action")) {
            if (!args["action"].is_string())
                throw ToolError("'action' must be a string");
            name = args["action"].get<std::string>();
        }
        std::string allowed;
        for (const Action& a : actions) {
            if (name == a.name)
                return a.run(args, ctx);
            allowed += (allowed.empty() ? "" : ", ") + std::string(a.name);
        }
        throw ToolError("'action' must be one of: " + allowed);
    };
}

json action_schema(std::initializer_list<const char*> names, const char* default_action = nullptr)
{
    json schema = { { "type", "string" }, { "enum", json::array() } };
    for (const char* name : names)
        schema["enum"].push_back(name);
    if (default_action != nullptr)
        schema["default"] = default_action;
    return schema;
}

const json REASON = { { "type", "string" }, { "description", "Why, shown to the user in the confirmation." } };

json control_print_action(const json& args, ToolContext& ctx) { return control_print(args, ctx.abandoned, ctx.state); }

const std::vector<Tool>& tools()
{
    static const json string_array = { { "type", "array" }, { "items", { { "type", "string" } } } };
    static const std::vector<Tool> list = {
        // ------------------------------------------------ settings and issues
        { "get_issues",
          "Errors and warnings currently reported by the slicer for the current plate, errors first: objects outside "
          "the build volume, invalid settings (with the setting key and its limits), incompatible filaments, slicing "
          "warnings, toolpath collisions. Call it first and after every change.",
          simple(&get_issues),
          { { "type", "object" }, { "properties", json::object() } },
          60s },
        { "get_project",
          "Summary of the open project: printer toolheads, filament slots (preset, type, color, temperature, toolhead, "
          "used on the plate), mixed filaments, filament assigned to walls/infill/supports, for each object its "
          "index, size, plate and position on it (center x/y, bottom z), parts (name, type, filament), filaments and "
          "per-object setting overrides and height ranges, the plates (objects, bed type, print sequence, sliced, "
          "current one, layer events) and the presets (selected, available, unsaved edits).",
          simple(&get_project),
          { { "type", "object" }, { "properties", json::object() } },
          60s },
        { "get_config",
          "Current values of the active print, filament and printer settings, including unsaved edits. Filament "
          "settings are arrays with one value per filament slot, per-toolhead printer settings arrays with one value "
          "per toolhead. Without arguments returns every setting except G-code templates and other long texts.",
          simple(&get_config),
          { { "type", "object" },
            { "properties",
              { { "keys", string_array },
                { "search", { { "type", "string" }, { "description", "Substring of a setting key or label." } } } } } },
          60s },
        { "get_docs",
          "Official documentation bundled with this slicer version; rely on it rather than on web searches or memory of "
          "other slicers.\n"
          "setting: meaning of the settings of 'keys': label, tooltip, unit, min/max, allowed values, default and "
          "current value, and the section of the documentation describing each one.\n"
          "page: without 'page', lists the pages (settings, calibration guides, multi-material, printer features, and "
          "kind=experience guides reviewed from the user's prints); with 'page' (and optionally 'section', an anchor "
          "from a documentation link), returns its text.",
          by_action({ { "setting", simple(&describe_setting) }, { "page", simple(&read_documentation) } }),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "setting", "page" }) },
                { "keys", string_array },
                { "page", { { "type", "string" } } },
                { "section", { { "type", "string" }, { "description", "Section anchor, e.g. scarf-joint-seam." } } } } },
            { "required", { "action" } } },
          60s },
        { "apply_changes",
          "Proposes setting changes to the user, who confirms or declines them in the slicer. Applied to the edited "
          "(unsaved) presets, or as per-object overrides when 'object' is given. Filament settings need "
          "'filament_slot' (one slot per call; the change applies to that slot's filament preset, so to every slot "
          "using the same preset). Per-toolhead printer settings take 'toolhead' to change a single toolhead, "
          "otherwise the value lists every toolhead. Invalid changes are all rejected with the reason. The user can "
          "uncheck some changes: the result lists them under 'unchecked'.",
          [](const json& args, ToolContext& ctx) { return apply_changes(args, ctx.abandoned); },
          { { "type", "object" },
            { "properties",
              { { "reason",
                  { { "type", "string" },
                    { "description", "Short explanation shown to the user, e.g. the issue being fixed." } } },
                { "changes",
                  { { "type", "array" },
                    { "items",
                      { { "type", "object" },
                        { "properties",
                          { { "key", { { "type", "string" } } },
                            { "value",
                              { { "type", { "string", "number", "boolean", "array" } },
                                { "description", "Value in the get_config format." } } },
                            { "comment",
                              { { "type", "string" },
                                { "description", "One short sentence on why this value, shown to the user next to the change." } } },
                            { "object", { { "type", "string" }, { "description", "Object name for a per-object override." } } },
                            { "filament_slot",
                              { { "type", "integer" }, { "minimum", 1 }, { "description", "1-based slot, for filament settings." } } },
                            { "toolhead",
                              { { "type", "integer" },
                                { "minimum", 1 },
                                { "description", "1-based toolhead, for one entry of a per-toolhead printer setting." } } } } },
                        { "required", { "key", "value" } } } } } } } },
            { "required", { "reason", "changes" } } },
          // The user has to read and confirm the changes.
          10min },
        { "edit_presets",
          "Presets and filament slots, like the lists and buttons of the sidebar, after the user's confirmation.\n"
          "select: the preset 'name' of 'type' printer, process or filament (for 'filament_slot'), among "
          "get_project.presets.available; the slicer asks the user what to do with unsaved changes.\n"
          "save: the edited preset of 'type' as the user preset 'name' (the filament preset edited is the one of the "
          "last slot changed by apply_changes).\n"
          "add_slot: a new filament slot, with the preset 'name' and 'color' if given.\n"
          "remove_slot: 'filament_slot'; the objects printed with it move to 'replace_with' (otherwise the slicer picks one) and "
          "the next slots move down by one.\n"
          "set_color: 'color' (#RRGGBB) of 'filament_slot'.",
          [](const json& args, ToolContext& ctx) { return edit_presets(args, ctx.abandoned); },
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "select", "save", "add_slot", "remove_slot", "set_color" }) },
                { "type", { { "type", "string" }, { "enum", { "printer", "process", "filament" } } } },
                { "name", { { "type", "string" }, { "description", "select, add_slot: preset name; save: new preset name." } } },
                { "filament_slot", { { "type", "integer" }, { "minimum", 1 } } },
                { "replace_with", { { "type", "integer" }, { "minimum", 1 } } },
                { "color", { { "type", "string" }, { "description", "e.g. #FF8000" } } },
                { "reason", REASON } } },
            { "required", { "action" } } },
          10min },
        // ------------------------------------------------ objects, plates, project, slicing
        { "edit_objects",
          "Changes the objects like the Prepare view, after the user's confirmation in the slicer (one undo step): "
          "scale (factor, or size_mm with one dimension, or several with keep_proportions=false), rotate (degrees "
          "around the bed axes, then put back on the bed), move (to_mm: center x/y on its plate, or by_mm), center "
          "on its plate, move_to_plate (to the 1-based 'plate' of get_project, keeping its position on the plate), drop to the bed, mirror, cut horizontally (height_mm from the object bottom; the upper part "
          "becomes <name>_A, placed on its cut face by default, the lower part <name>_B), split into objects, "
          "duplicate, delete, auto-orient (orient), arrange the current plate, add_part (a box, cylinder, cone or "
          "sphere of size_mm added to an object as a solid part, negative part, modifier, support blocker or support "
          "enforcer, at_mm = center x/y on the plate and bottom z; e.g. a block to fill a gap or a blocker under an "
          "overhang), delete_part, set_filament (of an object, or of one 'part'), rename (an object or one 'part'), "
          "add_height_range (from_mm to to_mm above the object bottom, with its own 'settings': layer_height, extruder "
          "= filament slot, wall, infill, surface and speed settings, as the Height range Modifier of the object list), "
          "remove_height_range (from_mm, to_mm). "
          "Operations run in order; 'object' is a name or the 1-based index of get_project, 'part' a name or the "
          "1-based index of its parts, both looked up when the operation runs (cut, split, duplicate and delete "
          "change the list). Returns the objects afterwards. Check sizes and positions with get_project and the "
          "result with get_view.",
          [](const json& args, ToolContext& ctx) { return edit_objects(args, ctx.abandoned, ctx.state); },
          { { "type", "object" },
            { "properties",
              { { "reason", { { "type", "string" }, { "description", "Why, shown to the user in the confirmation." } } },
                { "operations",
                  { { "type", "array" },
                    { "minItems", 1 },
                    { "items",
                      { { "type", "object" },
                        { "properties",
                          { { "action",
                              { { "type", "string" },
                                { "enum",
                                  { "scale", "rotate", "move", "move_to_plate", "center", "drop", "mirror", "cut", "split", "duplicate",
                                    "delete", "orient", "arrange", "add_part", "delete_part", "set_filament",
                                    "rename", "add_height_range", "remove_height_range" } } } },
                            { "object",
                              { { "type", { "string", "integer" } },
                                { "description", "Object name or 1-based index; not used by arrange." } } },
                            { "factor", { { "type", "number" }, { "description", "scale: 1.5 = 150 %." } } },
                            { "size_mm", { { "type", "object" }, { "description", "scale: target size, e.g. {\"z\": 50}; add_part: box x/y/z, diameter x and height z for a cylinder or cone, diameter x for a sphere." } } },
                            { "keep_proportions", { { "type", "boolean" }, { "default", true } } },
                            { "degrees", { { "type", "object" }, { "description", "rotate: e.g. {\"x\": 90}." } } },
                            { "plate", { { "type", "integer" }, { "minimum", 1 }, { "description", "move_to_plate: target plate." } } },
                            { "to_mm", { { "type", "object" }, { "description", "move: {\"x\": .., \"y\": ..} on the plate." } } },
                            { "by_mm", { { "type", "object" }, { "description", "move: relative {\"x\": .., \"y\": ..}." } } },
                            { "axis", { { "type", "string" }, { "enum", { "x", "y", "z" } }, { "description", "mirror." } } },
                            { "height_mm", { { "type", "number" }, { "description", "cut: from the object bottom." } } },
                            { "keep", { { "type", "string" }, { "enum", { "both", "upper", "lower" } }, { "default", "both" } } },
                            { "place_upper_on_cut", { { "type", "boolean" }, { "default", true } } },
                            { "copies", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 }, { "default", 1 } } },
                            { "part",
                              { { "type", { "string", "integer" } },
                                { "description", "delete_part, set_filament, rename: part name or 1-based index." } } },
                            { "type",
                              { { "type", "string" },
                                { "enum", { "part", "negative", "modifier", "support_blocker", "support_enforcer" } },
                                { "default", "part" } } },
                            { "shape", { { "type", "string" }, { "enum", { "box", "cylinder", "cone", "sphere" } }, { "default", "box" } } },
                            { "at_mm",
                              { { "type", "object" },
                                { "description", "add_part: {x, y} center on the plate, z bottom (default: object center, 0)." } } },
                            { "filament",
                              { { "type", "integer" },
                                { "minimum", 0 },
                                { "description", "set_filament, add_part: 1-based slot; 0 = the object's filament for a part." } } },
                            { "name", { { "type", "string" }, { "description", "rename, add_part." } } },
                            { "from_mm", { { "type", "number" }, { "description", "height ranges: from the object bottom." } } },
                            { "to_mm", { { "type", "number" } } },
                            { "settings",
                              { { "type", "object" }, { "description", "add_height_range: e.g. {\"layer_height\": 0.12, \"wall_loops\": 4}." } } } } },
                        { "required", { "action" } } } } } } } },
            { "required", { "reason", "operations" } } },
          // The user has to confirm the operations.
          10min,
          // Orient and arrange run in the background.
          10min },
        { "edit_plates",
          "Changes the plates, in order, after the user's confirmation (none needed to only select): select (show it; "
          "get_issues, slice and get_view work on the current plate), add (a new plate, then current), delete (an "
          "empty plate), rename, set (bed_type: e.g. \"Textured PEI Plate\", \"Cool Plate\", \"High Temp Plate\"; "
          "print_sequence: \"by layer\" or \"by object\"; \"default\" follows the project or process settings), "
          "add_layer_event (at the 1-based 'layer' or the layer closest to 'height_mm' of the sliced current plate: "
          "type pause, or filament_change to 'filament_slot', like the layer slider of the preview; no custom G-code), "
          "remove_layer_event. "
          "'plate' is the 1-based number of get_project.plates, the current plate by default. Move objects between "
          "plates with edit_objects (move_to_plate).",
          [](const json& args, ToolContext& ctx) { return edit_plates(args, ctx.abandoned); },
          { { "type", "object" },
            { "properties",
              { { "reason", { { "type", "string" } } },
                { "operations",
                  { { "type", "array" },
                    { "minItems", 1 },
                    { "items",
                      { { "type", "object" },
                        { "properties",
                          { { "action", { { "type", "string" }, { "enum", { "select", "add", "delete", "rename", "set", "add_layer_event", "remove_layer_event" } } } },
                            { "plate", { { "type", "integer" }, { "minimum", 1 } } },
                            { "name", { { "type", "string" } } },
                            { "bed_type", { { "type", "string" } } },
                            { "print_sequence", { { "type", "string" } } },
                            { "type", { { "type", "string" }, { "enum", { "pause", "filament_change" } } } },
                            { "layer", { { "type", "integer" }, { "minimum", 1 } } },
                            { "height_mm", { { "type", "number" } } },
                            { "filament_slot", { { "type", "integer" }, { "minimum", 1 } } } } },
                        { "required", { "action" } } } } } } } },
            { "required", { "operations" } } },
          10min },
        { "edit_project",
          "Project files through the slicer's commands, after the user's confirmation.\n"
          "new: empty project (the slicer asks the user to save unsaved changes).\n"
          "open: the 3MF project 'path' in place of the current one.\n"
          "import: adds the models of 'paths' (STL, 3MF, OBJ, STEP, AMF, SVG) to the current plate, like the Import "
          "button; the settings of an imported 3MF are not applied. Returns the added objects.\n"
          "save: the project (3MF) to its file, or to 'path' (then its new file).\n"
          "export: 'format' gcode (G-code of the sliced current plate), sliced_3mf (3MF holding that G-code, printable "
          "as is) or stl (the objects, one mesh), to 'path'.\n"
          "Paths are absolute ('~' for the home folder); an existing file is only replaced with overwrite=true, after "
          "asking the user. Without a path, the slicer shows its file dialog and the user picks the file (save_as forces "
          "it for save).",
          by_action({ { "new", [](const json& a, ToolContext& c) { return new_project(a, c.abandoned); } },
                      { "open", [](const json& a, ToolContext& c) { return open_project(a, c.abandoned); } },
                      { "import", [](const json& a, ToolContext& c) { return import_models(a, c.abandoned); } },
                      { "save", [](const json& a, ToolContext& c) { return save_project(a, c.abandoned); } },
                      { "export", [](const json& a, ToolContext& c) { return export_file(a, c.abandoned); } } }),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "new", "open", "import", "save", "export" }) },
                { "path", { { "type", "string" }, { "description", "open, save, export: absolute file path." } } },
                { "paths", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "import: absolute file paths." } } },
                { "format", { { "type", "string" }, { "enum", { "gcode", "sliced_3mf", "stl" } } } },
                { "overwrite", { { "type", "boolean" }, { "default", false } } },
                { "save_as", { { "type", "boolean" }, { "default", false } } },
                { "reason", REASON } } },
            { "required", { "action" } } },
          10min },
        { "undo",
          "Undoes (or redoes) the last 'steps' steps of the slicer's history, like Ctrl+Z / Ctrl+Y, after the user's "
          "confirmation listing them: object, part, plate and height range changes, including those made by edit_objects "
          "and edit_plates. Files, preset edits and the printer are not in the history.",
          [](const json& args, ToolContext& ctx) { return undo_redo(args, ctx.abandoned); },
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "undo", "redo" }) },
                { "steps", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 }, { "default", 1 } } },
                { "reason", REASON } } },
            { "required", { "action" } } },
          10min },
        { "adapt_to_printer",
          "Adapts a project made for another printer (e.g. a 3MF downloaded from MakerWorld) to the connected "
          "printer, after the user's confirmation: printer preset with the printer's nozzle, process preset with the "
          "closest layer height, and for each filament slot the preset and color of the closest loaded toolhead (same "
          "material first, then closest color; filament_mapping overrides it). Returns the original process settings "
          "that the switch changed (changed_settings): restore the designer's intent from them with apply_changes. "
          "Needs a connected printer.",
          [](const json& args, ToolContext& ctx) { return adapt_to_printer(args, ctx.abandoned); },
          { { "type", "object" },
            { "properties",
              { { "filament_mapping",
                  { { "type", "array" },
                    { "items",
                      { { "type", "object" },
                        { "properties", { { "slot", { { "type", "integer" }, { "minimum", 1 } } }, { "toolhead", { { "type", "integer" }, { "minimum", 1 } } } } },
                        { "required", { "slot", "toolhead" } } } },
                    { "description", "Optional: toolhead for some filament slots instead of the closest one" } } } } } },
          10min },
        { "slice",
          "Slices the current plate (like the Slice plate button, the slicer shows the preview) and waits for the "
          "result: print time, filament per slot (model, supports, wipe tower, flush), time and filament per feature "
          "(supports, walls, infill...), support layers per object, and the issues found while slicing. Refuses to "
          "slice while blocking errors remain.",
          [](const json& args, ToolContext& ctx) { return slice_plate(args, ctx.state); },
          { { "type", "object" },
            { "properties",
              { { "all_plates",
                  { { "type", "boolean" },
                    { "default", false },
                    { "description", "Slice every plate holding objects, like Slice all: print time of each plate, "
                                     "details of the current one." } } } } } },
          60s,
          10min },
        { "get_view",
          "PNG image of the current plate.\n"
          "model: the objects seen from 'view', colored by filament, with a legend. Shows the model, not the supports or "
          "toolpaths: use it to check orientation, overhangs, placement and color assignment, and slice to know about "
          "supports.",
          by_action({ { "model", simple(&render_view) } }, "model"),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "model" }, "model") },
                { "view",
                  { { "type", "string" },
                    { "enum", { "iso", "top_front", "left", "right", "top", "bottom", "front", "rear" } },
                    { "default", "iso" } } },
                { "size", { { "type", "integer" }, { "minimum", 256 }, { "maximum", 1024 }, { "default", 768 } } } } } },
          60s },
        // ------------------------------------------------ printer
        { "get_printer",
          "The printer connected in the slicer, when the user allowed printer access. Never changes anything on it.\n"
          "status: filament loaded, type, color and nozzle of each toolhead, print state and progress, last firmware "
          "error, temperatures, and the names of the firmware objects; 'objects' reads some of them in full (e.g. "
          "sensors) for diagnosis.\n"
          "snapshot: picture from its camera, taken now: first layer, failure (spaghetti, detached part, blob) or what "
          "the user describes.\n"
          "watch: waits until the print state changes (until=state_change, e.g. printing -> paused or error), the print "
          "ends (end) or a new layer starts (layer), at most timeout_s, then reports the state, layer, progress and any "
          "firmware error. Follows a print without polling; other tools can be called meanwhile.",
          by_action({ { "status", simple(&printer_status) },
                      { "snapshot", [](const json& a, ToolContext&) { return json { { MCP_CONTENT, printer_snapshot(a) } }; } },
                      { "watch", [](const json& a, ToolContext& c) { return watch_print(a, c.state); } } },
                    "status"),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "status", "snapshot", "watch" }, "status") },
                { "objects",
                  { { "type", "array" },
                    { "items", { { "type", "string" } } },
                    { "description", "status: firmware object names from available_objects." } } },
                { "until", { { "type", "string" }, { "enum", { "state_change", "end", "layer" } }, { "default", "state_change" } } },
                { "timeout_s", { { "type", "integer" }, { "minimum", 10 }, { "maximum", 600 }, { "default", 120 } } } } } },
          60s,
          11min },
        { "control_printer",
          "Commands to the connected printer, when the user allowed printer control. Each one is confirmed by the user "
          "in the slicer; only send them when the user asks.\n"
          "start_print: opens the slicer's print dialog for the sliced current plate, like the Print plate button: the "
          "user checks the file name and the filament mapping, then starts the print. Slice first and agree on the "
          "result with the user.\n"
          "pause, resume, cancel: the running print, then waits until the printer reports the new state. A cancelled "
          "print cannot be resumed.\n"
          "unload_filament: unloads 'toolheads' like the printer screen (the printer heats each nozzle, retracts the "
          "filament and turns the heaters off at the end). Needs an idle printer; empty toolheads and soft filaments "
          "(TPU) are skipped; about 1.5 minutes per toolhead. Check get_printer (status) first.",
          by_action({ { "start_print", [](const json& a, ToolContext&) { return start_print(a); } },
                      { "pause", &control_print_action },
                      { "resume", &control_print_action },
                      { "cancel", &control_print_action },
                      { "unload_filament", [](const json& a, ToolContext& c) { return unload_filament(a, c.abandoned, c.state); } } }),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "start_print", "pause", "resume", "cancel", "unload_filament" }) },
                { "toolheads",
                  { { "type", "array" },
                    { "items", { { "type", "integer" }, { "minimum", 1 } } },
                    { "minItems", 1 },
                    { "description", "unload_filament: toolheads numbered from 1 as on the printer." } } },
                { "reason", REASON } } },
            { "required", { "action" } } },
          10min,
          30min,
          [](json& state) {
              // Only the unloading sequence has to be ended (heaters).
              if (state.contains("phase"))
                  unload_filament_abandoned(state);
          } },
        // ------------------------------------------------ memory and knowledge
        { "notes",
          "Markdown notes of a folder shared with the user: what was learned in earlier conversations about this user, "
          "their printers and materials.\n"
          "list: name, title, size and last change of each note; call it at the start of a task and read the relevant "
          "notes.\n"
          "read: text of the note 'name'.\n"
          "write: creates or updates the note 'name' with 'content' ('mode': replace, append, or create only if it does "
          "not exist). Record durable knowledge worth reusing: the user's preferences, printer and material quirks, "
          "calibration results, what worked. Keep one topic per note, update an existing note (read it first) rather "
          "than duplicating it, and never store secrets or personal data. 'name' may contain sub-folders separated by "
          "'/'.",
          by_action({ { "list", simple(&list_notes) }, { "read", simple(&read_note) }, { "write", simple(&write_note) } }, "list"),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "list", "read", "write" }, "list") },
                { "name", { { "type", "string" }, { "description", "e.g. printers/u1.md" } } },
                { "content", { { "type", "string" }, { "description", "write: Markdown text." } } },
                { "mode", { { "type", "string" }, { "enum", { "replace", "append", "create" } }, { "default", "replace" } } } } } },
          60s },
        { "experiences",
          "Local journal of the user's real print experiences, never shared without their review.\n"
          "search: similar past cases by words of the symptom ('text'), 'material' (filament type) and 'status' "
          "(confirmed = real outcome known, awaiting_outcome, all).\n"
          "record: saves a diagnosis; the server adds the printer, materials, current issues and the settings changed by "
          "apply_changes since the last record. New record: 'symptom' (the user's words) and 'diagnosis'; update: 'id'. "
          "Ask the user before recording.\n"
          "report: the real result of a print ('result', with the user's 'notes') for an experience ('id', default: the "
          "latest one awaiting it); adds the printer's report of the last print when available.",
          by_action({ { "search", simple(&search_experiences) },
                      { "record", simple(&record_experience) },
                      { "report", simple(&report_outcome) } }),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "search", "record", "report" }) },
                { "id", { { "type", "string" } } },
                { "text", { { "type", "string" } } },
                { "material", { { "type", "string" }, { "description", "e.g. PETG." } } },
                { "status", { { "type", "string" }, { "enum", { "confirmed", "awaiting_outcome", "all" } } } },
                { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } },
                { "symptom", { { "type", "string" } } },
                { "diagnosis", { { "type", "string" } } },
                { "notes", { { "type", "string" } } },
                { "capitalized_in", { { "type", "string" }, { "description", "record: guide page that now includes this experience." } } },
                { "result", { { "type", "string" }, { "enum", { "success", "partial", "failure" } } } } } },
            { "required", { "action" } } },
          60s },
        { "manufacturer_docs",
          "Snapmaker's official documentation, read live (needs the user's permission in the MCP settings).\n"
          "search: the wiki (wiki.snapmaker.com: U1 troubleshooting by symptom, maintenance, hotend, loading, firmware "
          "release notes, filament sheets) and the support site (support.snapmaker.com) for 'query', in English. "
          "Returns titles and URLs.\n"
          "read: text of the page 'url' of these sites only, with its title and last update date. Long pages come in "
          "parts: pass continues.next_offset as 'offset'. Images are given by their caption only.",
          by_action({ { "search", [](const json& a, ToolContext& c) { return search_manufacturer_docs(a, c.state); } },
                      { "read", [](const json& a, ToolContext& c) { return read_manufacturer_page(a, c.state); } } }),
          { { "type", "object" },
            { "properties",
              { { "action", action_schema({ "search", "read" }) },
                { "query", { { "type", "string" }, { "description", "e.g. U1 stringing PETG" } } },
                { "url", { { "type", "string" } } },
                { "offset", { { "type", "integer" }, { "minimum", 0 }, { "default", 0 } } } } },
            { "required", { "action" } } },
          30s,
          40s,
          &manufacturer_docs_abandoned },
    };
    return list;
}

// ---------------------------------------------------------------- prompts

// Guidance sent at initialization, one paragraph per domain of tools, in this
// order. Details of use belong to the tool descriptions.
const char* const INSTRUCTIONS[] = {
    // Settings and issues
    "You are connected to Snapmaker Orca, the 3D printing slicer the user has open.\n"
    "Workflow:\n"
    "1. Call get_issues first. Fix errors (blocks_slicing=true) before anything else, then warnings, and only then "
    "work on other improvements the user asked for.\n"
    "2. For each issue, explain the cause in one or two sentences. Check the setting meaning, unit and limits with "
    "get_docs (setting; often already included as setting_info) before choosing a value: it is the official "
    "documentation of this slicer version, and get_docs (page) gives the related pages (calibration guides...). Rely "
    "on them rather than on web searches or memory of other slicers.\n"
    "3. Fix settings with apply_changes; the user confirms in the slicer. Group the changes fixing one issue in a "
    "single call with a clear reason and a one-sentence comment on each change. The user may uncheck some: respect "
    "the 'unchecked' ones. Do not change settings unrelated to the issues or the user's request.\n"
    "4. Call get_issues again after each applied change to verify, and iterate.\n"
    "Tools that change something ask the user first and answer done=true, or declined=true when the user said no: "
    "then ask them what they want instead. Tools grouping several actions take 'action'.",
    // Multi-material
    "Multi-material: check get_project.materials before recommending anything. On a multi-toolhead printer "
    "(toolhead_layout=multi_toolhead, e.g. Snapmaker U1 with 4 toolheads) every filament slot has its own toolhead "
    "and nozzle; a print commonly mixes materials with different temperatures and behaviours. Therefore:\n"
    "- Always name the filament slot or toolhead (1-based) a recommendation is about.\n"
    "- Filament settings belong to one slot: pass filament_slot to apply_changes. Changing a slot whose preset is "
    "shared by other slots changes them too (see also_applies_to_slots). Only one filament preset can hold unsaved "
    "edits: before changing another slot, ask the user to save the previous one.\n"
    "- Per-toolhead printer settings (retraction, z hop, nozzle diameter...) are lists in toolhead order: pass "
    "toolhead to change one entry.\n"
    "- Process settings are shared by every filament: check that a new speed, layer height or cooling value suits "
    "all the materials used on the plate (filament_slots[].used_on_plate), not only one.\n"
    "- feature_filaments and the per-object filaments tell which slot prints walls, infill, supports and each "
    "object (0 = the object's default filament). Mixed filaments are virtual IDs after the physical slots.\n"
    "- edit_presets selects other printer, process or filament presets, saves user presets and adds, removes or "
    "recolors filament slots. Prefer apply_changes for single settings; save a filament preset the user wants to "
    "keep before editing another slot.",
    // Objects, plates, project, slicing and views
    "Objects and plates: edit_objects scales, rotates, moves, cuts, splits, duplicates, deletes, orients and arranges "
    "objects like the Prepare view; use it for placement issues (outside the build volume, too big, overhangs to "
    "reorient) and when the user asks, then check with get_issues and get_view. get_issues, slice and get_view work "
    "on the current plate: edit_plates selects, adds, deletes and sets up plates; slice all_plates for the whole "
    "project. Height ranges (edit_objects) give part of an object other settings; layer events (edit_plates) pause "
    "the print or change the filament at a layer. undo reverts the last steps when the user is not satisfied. "
    "edit_project creates, opens, imports, saves and exports files. Issues fixed in the slicer UI rather "
    "than through these tools must be explained to the user step by step.\n"
    "When the user describes a visual problem (missing supports, stringing, wrong colors...), look at the model with "
    "get_view from the relevant side and slice to get the supports and per-feature results; the user may also paste "
    "a screenshot of the preview. Slice again after changes to confirm the fix.",
    // Printer
    "Printer: get_printer reads the connected printer when the user allowed it; get_issues then also reports "
    "differences between the project and the printer (missing filament, other material or nozzle on a used "
    "toolhead). After a failed print, start from last_error and the print state, then inspect the related firmware "
    "objects, and take a snapshot to see it; watch follows a running print. When the user allowed printer control, "
    "control_printer starts the sliced plate (through the print dialog), pauses, resumes or cancels the print and "
    "unloads filament; only do so when the user asks. You cannot heat or move the printer: tell the user what to do.",
    // Experiences
    "Experience: before diagnosing, search the experiences for similar cases and the get_docs pages of kind "
    "'experience' (reviewed lessons from real prints). Say where each recommendation comes from: official "
    "documentation, reviewed guide, or an unreviewed past case of the journal. At the end of a diagnosis, offer to "
    "record it; when the user reports how the print went, report the outcome.",
    // Manufacturer documentation
    "Manufacturer: for procedures, hardware troubleshooting, maintenance, firmware behaviour and Snapmaker's "
    "recommendations, manufacturer_docs searches and reads the official Snapmaker wiki and support site live (query "
    "in English); cite the page URL and its update date. Setting values stay those of the slicer profiles and "
    "get_docs. When a page is useful for this user, summarize what applies in a note with its URL and the date read.",
    // Notes
    "Notes: a folder of Markdown notes is shared with the user. At the start of a task, list the notes and read "
    "those that matter; when you learn something durable (user preferences, printer or material quirks, calibration "
    "results, a fix that worked), write it, updating the existing note on the topic. Tell the user when you change "
    "a note.",
};

const char* ADAPT_PROMPT =
    "The project open in my slicer was made for another printer (often a 3MF from MakerWorld). Adapt it to my "
    "connected printer:\n"
    "1. get_project and get_printer (status): note the original printer, the filaments of each slot and what is "
    "loaded in each toolhead.\n"
    "2. Call adapt_to_printer (I confirm in the slicer). If a slot clearly belongs to another toolhead than the "
    "closest one (e.g. support material), pass filament_mapping.\n"
    "3. From changed_settings, decide which original values express the designer's intent and suit my printer "
    "(layer height, wall loops, top/bottom shells, infill density and pattern, supports, brim, seam, ironing, "
    "per-feature line widths in proportion to the nozzle...). Check the limits with get_docs (setting). Keep my "
    "printer's values for machine-specific settings (speeds and accelerations beyond its limits, prime tower and "
    "flushing made for a single-nozzle printer, retraction, temperatures of the new filament presets).\n"
    "4. Call apply_changes with the restored settings, a short comment on each (I review them in the slicer and "
    "can uncheck some).\n"
    "5. get_issues and fix errors, then slice and get_view to check.\n"
    "6. Offer to record the experience with the original printer, so that we learn from these adaptations.";

const char* CAPITALIZE_PROMPT =
    "Turn my confirmed print experiences into guides of this repository.\n"
    "1. Call experiences with action=search, status=confirmed and limit=500. Skip records that already have "
    "capitalized_in.\n"
    "2. Group them by situation: printer, material(s), symptom. Read the existing guides first "
    "(resources/ai_assistant/guides/*.md and resources/ai_assistant/guides/README.md for the format).\n"
    "3. Write or update one guide per topic in resources/ai_assistant/guides/. Each lesson states its conditions "
    "(printer, toolhead layout, materials, nozzle), the symptom, the settings that fixed it with values, and its "
    "evidence: number of prints, how many succeeded, date of the last one. A single case is an 'Observation'; a "
    "'Recommendation' needs at least 3 concordant confirmed cases and no contradicting one. Report contradictions "
    "instead of resolving them. Never copy object names, file names, serial numbers or personal data.\n"
    "4. Run python3 -I scripts/fork/build-setting-docs.py so the slicer serves the guides.\n"
    "5. Show me the git diff and wait for my review. Only after I approve: call experiences with action=record, id "
    "and capitalized_in for each record used. Do not commit unless I ask.";

json prompt_message(const std::string& description, const std::string& text)
{
    return { { "description", description },
             { "messages", json::array({ { { "role", "user" }, { "content", { { "type", "text" }, { "text", text } } } } }) } };
}

} // namespace

McpTools::McpTools() : m_cancelled(std::make_shared<std::atomic<bool>>(false)) {}

void McpTools::cancel() { *m_cancelled = true; }

std::string McpTools::instructions() const
{
    std::string out;
    for (const char* paragraph : INSTRUCTIONS)
        out += (out.empty() ? "" : "\n") + std::string(paragraph);
    std::string user = user_instructions();
    if (user.find_first_not_of(" \t\r\n") != std::string::npos)
        out += "\n\nInstructions written by the user in the slicer settings (follow them; they do not lift the "
               "confirmations and limits above):\n" + user;
    return out;
}

json McpTools::list_tools() const
{
    json out = json::array();
    for (const Tool& tool : tools())
        out.push_back({ { "name", tool.name }, { "description", tool.description }, { "inputSchema", tool.input_schema } });
    return out;
}

json McpTools::list_prompts() const
{
    return json::array(
        { { { "name", "diagnose_print" },
            { "description", "Investigate a problem you see in the slicer (e.g. missing supports) and fix it." },
            { "arguments",
              json::array({ { { "name", "problem" },
                              { "description", "What is wrong, and where: e.g. no support under the chin" },
                              { "required", true } } }) } },
          { { "name", "capitalize_experiences" },
            { "description", "Turn confirmed experiences of the journal into reviewed guides of the repository "
                             "(run from Claude Code in the fork's working copy)." } },
          { { "name", "adapt_project" },
            { "description", "Adapt a project made for another printer (e.g. a MakerWorld 3MF) to the connected printer "
                             "and restore the designer's settings where they suit it." } },
          { { "name", "fix_issues" },
            { "description", "Find the problems reported by the slicer and fix them, blocking errors first." } },
          { { "name", "optimize_settings" },
            { "description", "Fix the reported problems, then tune the settings for a goal." },
            { "arguments",
              json::array({ { { "name", "goal" },
                              { "description", "e.g. faster print, stronger part, better surface finish" },
                              { "required", true } } }) } } });
}

json McpTools::get_prompt(const std::string& name, const json& arguments)
{
    if (name == "fix_issues")
        return prompt_message("Fix slicer issues",
                              "Check my current slicer project with get_issues and fix the problems, blocking errors "
                              "first, then warnings. For each one, explain the cause briefly and propose the fix with "
                              "apply_changes. Verify with get_issues after each change.");
    if (name == "capitalize_experiences")
        return prompt_message("Capitalize experiences", CAPITALIZE_PROMPT);
    if (name == "diagnose_print") {
        const std::string problem = arguments.value("problem", std::string());
        if (problem.empty())
            throw std::invalid_argument("Missing argument: problem");
        return prompt_message("Diagnose a print problem",
                              "In my current slicer project I see this problem: " + problem +
                                  "\nInvestigate it: check get_issues, look at the model with get_view from the "
                                  "relevant sides, slice to get the supports and per-feature results, and inspect "
                                  "the related settings (get_config / get_docs), including which filament "
                                  "slot prints the feature. Explain the cause, then propose the fix with "
                                  "apply_changes and slice again to confirm it.");
    }
    if (name == "adapt_project")
        return prompt_message("Adapt project to my printer", ADAPT_PROMPT);
    if (name == "optimize_settings") {
        const std::string goal = arguments.value("goal", std::string());
        if (goal.empty())
            throw std::invalid_argument("Missing argument: goal");
        return prompt_message("Optimize settings",
                              "First check my slicer project with get_issues and fix any error. Then, using get_project "
                              "and get_config, recommend setting changes for this goal: " +
                                  goal +
                                  ". Explain the trade-offs, and apply them with apply_changes once I agree.");
    }
    throw std::invalid_argument("Unknown prompt: " + name);
}

json McpTools::call_tool(const std::string& name, const json& arguments)
{
    const auto it = std::find_if(tools().begin(), tools().end(), [&name](const Tool& t) { return name == t.name; });
    if (it == tools().end())
        throw std::invalid_argument("Unknown tool: " + name);

    struct Outcome
    {
        json result;
        bool retry { false };
    };

    constexpr auto RETRY_DELAY    = 250ms;
    const auto     retry_deadline = std::chrono::steady_clock::now() + it->retry_for;
    // Only touched on the GUI thread, by the successive calls.
    auto state = std::make_shared<json>(json::object());
    for (;;) {
        if (*m_cancelled || wxTheApp == nullptr)
            throw std::runtime_error("The application is shutting down");

        auto promise   = std::make_shared<std::promise<Outcome>>();
        auto future    = promise->get_future();
        auto abandoned = std::make_shared<std::atomic<bool>>(false);
        wxTheApp->CallAfter([promise, abandoned, state, run = it->run, arguments, cancelled = m_cancelled]() {
            if (*cancelled || *abandoned) {
                promise->set_value({ text_result("The request was abandoned", true) });
                return;
            }
            try {
                ToolContext ctx { *abandoned, *state };
                json        result = run(arguments, ctx);
                if (result.is_object() && result.contains(MCP_CONTENT)) {
                    json content = { { "content", std::move(result[MCP_CONTENT]) }, { "isError", false } };
                    promise->set_value(Outcome { std::move(content) });
                } else {
                    promise->set_value(Outcome { text_result(result.dump(2)) });
                }
            } catch (const ToolRetry&) {
                promise->set_value({ json(), true });
            } catch (const ToolError& e) {
                promise->set_value({ text_result(e.what(), true) });
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });

        // Lets a tool undo a sequence it started, once no further call will come. The
        // state is only touched on the GUI thread, where a pending call may still run.
        const auto give_up = [&]() {
            if (it->abandon && wxTheApp != nullptr)
                wxTheApp->CallAfter([abandon = it->abandon, state]() { abandon(*state); });
        };

        const auto deadline = std::chrono::steady_clock::now() + it->timeout;
        while (future.wait_for(100ms) != std::future_status::ready) {
            if (*m_cancelled) {
                *abandoned = true;
                give_up();
                throw std::runtime_error("The application is shutting down");
            }
            if (std::chrono::steady_clock::now() > deadline) {
                *abandoned = true;
                give_up();
                throw std::runtime_error("The slicer did not answer in time (busy or waiting for the user?)");
            }
        }

        Outcome outcome = future.get();
        if (!outcome.retry)
            return outcome.result;
        if (std::chrono::steady_clock::now() + RETRY_DELAY > retry_deadline) {
            give_up();
            return text_result(ToolRetry().what(), true);
        }
        std::this_thread::sleep_for(RETRY_DELAY);
    }
}

}} // namespace Slic3r::AIAssistant
