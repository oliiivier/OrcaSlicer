#include "McpTools.hpp"

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
#include <climits>
#include <future>
#include <stdexcept>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;

namespace {

// Raised by a tool for a problem the model should see and can fix (bad key...).
struct ToolError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

constexpr auto GUI_CALL_TIMEOUT = std::chrono::seconds(60);

json text_result(const std::string& text, bool is_error = false)
{
    return { { "content", json::array({ { { "type", "text" }, { "text", text } } }) }, { "isError", is_error } };
}

const char* type_name(ConfigOptionType type)
{
    switch (static_cast<ConfigOptionType>(int(type) & ~int(coVectorType))) {
    case coFloat: return "float";
    case coInt: return "int";
    case coString: return "string";
    case coPercent: return "percent";
    case coFloatOrPercent: return "float_or_percent";
    case coPoint: return "point";
    case coPoint3: return "point3";
    case coBool: return "bool";
    case coEnum: return "enum";
    default: return "unknown";
    }
}

const char* mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comSimple: return "simple";
    case comAdvanced: return "advanced";
    default: return "develop";
    }
}

std::vector<std::string> string_list(const json& args, const char* name)
{
    std::vector<std::string> out;
    if (!args.contains(name))
        return out;
    const json& list = args[name];
    if (!list.is_array())
        throw ToolError(std::string("'") + name + "' must be an array of strings");
    for (const json& item : list) {
        if (!item.is_string())
            throw ToolError(std::string("'") + name + "' must be an array of strings");
        out.push_back(item.get<std::string>());
    }
    return out;
}

GUI::Plater& plater()
{
    GUI::Plater* p = wxGetApp().plater();
    if (p == nullptr || wxGetApp().preset_bundle == nullptr)
        throw ToolError("No project is open (the application may be in G-code viewer mode)");
    return *p;
}

// ---------------------------------------------------------------- tools

json get_project(const json& /* args */)
{
    GUI::Plater&        p      = plater();
    const PresetBundle& bundle = *wxGetApp().preset_bundle;

    json objects = json::array();
    for (const ModelObject* obj : p.model().objects) {
        json overrides = json::object();
        for (const std::string& key : obj->config.keys())
            overrides[key] = obj->config.opt_serialize(key);

        json o = { { "name", obj->name },
                   { "instances", obj->instances.size() },
                   { "parts", obj->volumes.size() },
                   { "setting_overrides", std::move(overrides) },
                   { "height_range_overrides", obj->layer_config_ranges.size() } };
        if (!obj->instances.empty()) {
            const Vec3d size = obj->instance_bounding_box(0).size();
            o["size_mm"]     = { size.x(), size.y(), size.z() };
        }
        objects.push_back(std::move(o));
    }

    return { { "project_name", GUI::into_u8(p.get_project_name()) },
             { "presets",
               { { "printer", bundle.printers.get_selected_preset_name() },
                 { "process", bundle.prints.get_selected_preset_name() },
                 { "filaments", bundle.filament_presets } } },
             { "objects", std::move(objects) } };
}

json get_config(const json& args)
{
    plater();
    const std::vector<std::string> keys   = string_list(args, "keys");
    const std::string              search = boost::algorithm::to_lower_copy(args.value("search", std::string()));
    const DynamicPrintConfig       config = wxGetApp().preset_bundle->full_config();

    json values = json::object();
    if (!keys.empty()) {
        json unknown = json::array();
        for (const std::string& key : keys) {
            if (config.option(key) != nullptr)
                values[key] = config.opt_serialize(key);
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
        values[key] = config.opt_serialize(key);
    }
    return { { "values", std::move(values) } };
}

json describe_setting(const json& args)
{
    plater();
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
        json d = { { "label", def->full_label.empty() ? def->label : def->full_label },
                   { "category", def->category },
                   { "tooltip", def->tooltip },
                   { "type", type_name(def->type) },
                   { "per_extruder", !def->is_scalar() },
                   { "mode", mode_name(def->mode) } };
        if (!def->sidetext.empty())
            d["unit"] = def->sidetext;
        if (def->min != INT_MIN)
            d["min"] = def->min;
        if (def->max != INT_MAX)
            d["max"] = def->max;
        if (!def->enum_values.empty()) {
            json values = json::array();
            for (size_t i = 0; i < def->enum_values.size(); ++i)
                values.push_back({ { "value", def->enum_values[i] },
                                   { "label", i < def->enum_labels.size() ? def->enum_labels[i] : def->enum_values[i] } });
            d["allowed_values"] = std::move(values);
        }
        if (def->default_value)
            d["default"] = def->default_value->serialize();
        if (config.option(key) != nullptr)
            d["current"] = config.opt_serialize(key);
        out[key] = std::move(d);
    }
    return out;
}

// ---------------------------------------------------------------- registry

struct Tool
{
    const char* name;
    const char* description;
    json (*run)(const json& args);
    json input_schema;
};

const std::vector<Tool>& tools()
{
    static const json string_array = { { "type", "array" }, { "items", { { "type", "string" } } } };
    static const std::vector<Tool> list = {
        { "get_project",
          "Summary of the open project: selected printer, process and filament presets, and for each object its size, "
          "instance count and per-object setting overrides.",
          &get_project,
          { { "type", "object" }, { "properties", json::object() } } },
        { "get_config",
          "Current values of the active print, filament and printer settings, including unsaved edits. Without "
          "arguments returns every setting except G-code templates and other long texts.",
          &get_config,
          { { "type", "object" },
            { "properties",
              { { "keys", string_array },
                { "search", { { "type", "string" }, { "description", "Substring of a setting key or label." } } } } } } },
        { "describe_setting",
          "Meaning of settings: label, tooltip, unit, min/max, allowed values, default and current value.",
          &describe_setting,
          { { "type", "object" }, { "properties", { { "keys", string_array } } }, { "required", { "keys" } } } },
    };
    return list;
}

} // namespace

McpTools::McpTools() : m_cancelled(std::make_shared<std::atomic<bool>>(false)) {}

void McpTools::cancel() { *m_cancelled = true; }

json McpTools::list_tools() const
{
    json out = json::array();
    for (const Tool& tool : tools())
        out.push_back({ { "name", tool.name }, { "description", tool.description }, { "inputSchema", tool.input_schema } });
    return out;
}

json McpTools::call_tool(const std::string& name, const json& arguments)
{
    const auto it = std::find_if(tools().begin(), tools().end(), [&name](const Tool& t) { return name == t.name; });
    if (it == tools().end())
        throw std::invalid_argument("Unknown tool: " + name);
    if (*m_cancelled || wxTheApp == nullptr)
        throw std::runtime_error("The application is shutting down");

    auto       promise = std::make_shared<std::promise<json>>();
    auto       future  = promise->get_future();
    const auto run     = it->run;
    wxTheApp->CallAfter([promise, run, arguments, cancelled = m_cancelled]() {
        if (*cancelled) {
            promise->set_value(text_result("The application is shutting down", true));
            return;
        }
        try {
            promise->set_value(text_result(run(arguments).dump(2)));
        } catch (const ToolError& e) {
            promise->set_value(text_result(e.what(), true));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + GUI_CALL_TIMEOUT;
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (*m_cancelled)
            throw std::runtime_error("The application is shutting down");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("The slicer did not answer in time (busy or showing a modal dialog?)");
    }
    return future.get();
}

}} // namespace Slic3r::AIAssistant
