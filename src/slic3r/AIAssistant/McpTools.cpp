#include "McpTools.hpp"
#include "McpChanges.hpp"
#include "McpDocs.hpp"
#include "McpIssues.hpp"
#include "McpMaterials.hpp"
#include "McpRender.hpp"
#include "McpSlicing.hpp"
#include "McpToolUtils.hpp"

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
    for (const ModelObject* obj : p.model().objects) {
        json overrides = json::object();
        for (const std::string& key : obj->config.keys())
            overrides[key] = obj->config.opt_serialize(key);

        json o = { { "name", obj->name },
                   { "instances", obj->instances.size() },
                   { "parts", obj->volumes.size() },
                   { "filaments", object_filaments(*obj) },
                   { "setting_overrides", std::move(overrides) },
                   { "height_range_overrides", obj->layer_config_ranges.size() } };
        if (!obj->instances.empty()) {
            const Vec3d size = obj->instance_bounding_box(0).size();
            o["size_mm"]     = { size.x(), size.y(), size.z() };
        }
        objects.push_back(std::move(o));
    }

    json out = { { "project_name", GUI::into_u8(p.get_project_name()) },
                 { "presets",
                   { { "printer", bundle.printers.get_selected_preset_name() },
                     { "process", bundle.prints.get_selected_preset_name() } } },
                 { "materials", describe_materials() },
                 { "objects", std::move(objects) } };
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

// Adapts a read-only tool, which has nothing to roll back when the client stops waiting.
ToolFn read_only(json (*fn)(const json&))
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
};

const std::vector<Tool>& tools()
{
    static const json string_array = { { "type", "array" }, { "items", { { "type", "string" } } } };
    static const std::vector<Tool> list = {
        { "get_issues",
          "Errors and warnings currently reported by the slicer for the current plate, errors first: objects outside "
          "the build volume, invalid settings (with the setting key and its limits), incompatible filaments, slicing "
          "warnings, toolpath collisions. Call it first and after every change.",
          read_only(&get_issues),
          { { "type", "object" }, { "properties", json::object() } },
          60s },
        { "get_project",
          "Summary of the open project: printer toolheads, filament slots (preset, type, color, temperature, toolhead, "
          "used on the plate), mixed filaments, filament assigned to walls/infill/supports, and for each object its "
          "size, filaments and per-object setting overrides.",
          read_only(&get_project),
          { { "type", "object" }, { "properties", json::object() } },
          60s },
        { "get_config",
          "Current values of the active print, filament and printer settings, including unsaved edits. Filament "
          "settings are arrays with one value per filament slot, per-toolhead printer settings arrays with one value "
          "per toolhead. Without arguments returns every setting except G-code templates and other long texts.",
          read_only(&get_config),
          { { "type", "object" },
            { "properties",
              { { "keys", string_array },
                { "search", { { "type", "string" }, { "description", "Substring of a setting key or label." } } } } } },
          60s },
        { "describe_setting",
          "Meaning of settings: label, tooltip, unit, min/max, allowed values, default and current value, and the "
          "section of the official documentation describing the setting (bundled with this slicer version).",
          read_only(&describe_setting),
          { { "type", "object" }, { "properties", { { "keys", string_array } } }, { "required", { "keys" } } },
          60s },
        { "apply_changes",
          "Proposes setting changes to the user, who confirms or declines them in the slicer. Applied to the edited "
          "(unsaved) presets, or as per-object overrides when 'object' is given. Filament settings need "
          "'filament_slot' (one slot per call; the change applies to that slot's filament preset, so to every slot "
          "using the same preset). Per-toolhead printer settings take 'toolhead' to change a single toolhead, "
          "otherwise the value lists every toolhead. Invalid changes are all rejected with the reason.",
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
        { "read_documentation",
          "Official user documentation bundled with this slicer version (settings, calibration guides, multi-material, "
          "printer features). Without arguments lists the pages; with 'page' (and optionally 'section', an anchor from "
          "a documentation link) returns the text. Use it instead of searching the web.",
          read_only(&read_documentation),
          { { "type", "object" },
            { "properties",
              { { "page", { { "type", "string" } } },
                { "section", { { "type", "string" }, { "description", "Section anchor, e.g. scarf-joint-seam." } } } } } },
          60s },
        { "slice",
          "Slices the current plate (like the Slice plate button, the slicer shows the preview) and waits for the "
          "result: print time, filament per slot (model, supports, wipe tower, flush), time and filament per feature "
          "(supports, walls, infill...), support layers per object, and the issues found while slicing. Refuses to "
          "slice while blocking errors remain.",
          [](const json&, ToolContext& ctx) { return slice_plate(ctx.state); },
          { { "type", "object" }, { "properties", json::object() } },
          60s,
          10min },
        { "render_view",
          "PNG image of the objects of the current plate seen from a viewpoint, colored by filament, with a legend. "
          "Shows the model, not the supports or toolpaths: use it to check orientation, overhangs, placement and "
          "color assignment, and slice to know about supports.",
          read_only(&render_view),
          { { "type", "object" },
            { "properties",
              { { "view",
                  { { "type", "string" },
                    { "enum", { "iso", "top_front", "left", "right", "top", "bottom", "front", "rear" } },
                    { "default", "iso" } } },
                { "size", { { "type", "integer" }, { "minimum", 256 }, { "maximum", 1024 }, { "default", 768 } } } } } },
          60s },
    };
    return list;
}

// ---------------------------------------------------------------- prompts

const char* INSTRUCTIONS =
    "You are connected to Snapmaker Orca, the 3D printing slicer the user has open.\n"
    "Workflow:\n"
    "1. Call get_issues first. Fix errors (blocks_slicing=true) before anything else, then warnings, and only then "
    "work on other improvements the user asked for.\n"
    "2. For each issue, explain the cause in one or two sentences. Check the setting meaning, unit and limits with "
    "describe_setting (often already included as setting_info) before choosing a value; its documentation field is "
    "the official documentation of this slicer version, and read_documentation gives the related pages "
    "(calibration guides...). Rely on them rather than on web searches or memory of other slicers.\n"
    "3. Fix settings with apply_changes; the user confirms in the slicer. Group the changes fixing one issue in a "
    "single call with a clear reason. Do not change settings unrelated to the issues or the user's request.\n"
    "4. Call get_issues again after each applied change to verify, and iterate.\n"
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
    "Issues fixed in the slicer UI rather than in settings (object placement, filament assignment to slots, plate "
    "type) must be explained to the user step by step.\n"
    "When the user describes a visual problem (missing supports, stringing, wrong colors...), use render_view to "
    "look at the model from the relevant side and slice to get the supports and per-feature results; the user may "
    "also paste a screenshot of the preview. Slice again after changes to confirm the fix.";

json prompt_message(const std::string& description, const std::string& text)
{
    return { { "description", description },
             { "messages", json::array({ { { "role", "user" }, { "content", { { "type", "text" }, { "text", text } } } } }) } };
}

} // namespace

McpTools::McpTools() : m_cancelled(std::make_shared<std::atomic<bool>>(false)) {}

void McpTools::cancel() { *m_cancelled = true; }

std::string McpTools::instructions() const { return INSTRUCTIONS; }

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
    if (name == "diagnose_print") {
        const std::string problem = arguments.value("problem", std::string());
        if (problem.empty())
            throw std::invalid_argument("Missing argument: problem");
        return prompt_message("Diagnose a print problem",
                              "In my current slicer project I see this problem: " + problem +
                                  "\nInvestigate it: check get_issues, look at the model with render_view from the "
                                  "relevant sides, slice to get the supports and per-feature results, and inspect "
                                  "the related settings (get_config / describe_setting), including which filament "
                                  "slot prints the feature. Explain the cause, then propose the fix with "
                                  "apply_changes and slice again to confirm it.");
    }
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

        const auto deadline = std::chrono::steady_clock::now() + it->timeout;
        while (future.wait_for(100ms) != std::future_status::ready) {
            if (*m_cancelled) {
                *abandoned = true;
                throw std::runtime_error("The application is shutting down");
            }
            if (std::chrono::steady_clock::now() > deadline) {
                *abandoned = true;
                throw std::runtime_error("The slicer did not answer in time (busy or waiting for the user?)");
            }
        }

        Outcome outcome = future.get();
        if (!outcome.retry)
            return outcome.result;
        if (std::chrono::steady_clock::now() + RETRY_DELAY > retry_deadline)
            return text_result(ToolRetry().what(), true);
        std::this_thread::sleep_for(RETRY_DELAY);
    }
}

}} // namespace Slic3r::AIAssistant
