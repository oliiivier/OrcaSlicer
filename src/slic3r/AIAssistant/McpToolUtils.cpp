#include "McpToolUtils.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/beast/core/detail/base64.hpp>

#include <climits>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;

namespace {

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

} // namespace

GUI::Plater& require_plater()
{
    GUI::Plater* p = wxGetApp().plater();
    if (p == nullptr || wxGetApp().preset_bundle == nullptr || wxGetApp().is_gcode_viewer())
        throw ToolError("No project is open (the application may be in G-code viewer mode)");
    if (p->printer_technology() != ptFFF)
        throw ToolError("Only FFF printers are supported");
    return *p;
}

int find_object(const std::string& name)
{
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    int                    found   = -1;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i]->name != name)
            continue;
        if (found >= 0)
            throw ToolError("Several objects are named '" + name + "', rename them in the slicer first");
        found = int(i);
    }
    if (found < 0)
        throw ToolError("No object named '" + name + "'");
    return found;
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

json setting_summary(const ConfigOptionDef& def)
{
    json d = { { "label", def.full_label.empty() ? def.label : def.full_label },
               { "category", def.category },
               { "tooltip", def.tooltip },
               { "type", type_name(def.type) },
               { "per_extruder", !def.is_scalar() },
               { "mode", mode_name(def.mode) } };
    if (!def.sidetext.empty())
        d["unit"] = def.sidetext;
    if (def.min != INT_MIN)
        d["min"] = def.min;
    if (def.max != INT_MAX)
        d["max"] = def.max;
    if (!def.enum_values.empty()) {
        json values = json::array();
        for (size_t i = 0; i < def.enum_values.size(); ++i)
            values.push_back({ { "value", def.enum_values[i] },
                               { "label", i < def.enum_labels.size() ? def.enum_labels[i] : def.enum_values[i] } });
        d["allowed_values"] = std::move(values);
    }
    if (def.default_value)
        d["default"] = option_value(*def.default_value);
    return d;
}

json option_value(const ConfigOption& opt)
{
    if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(&opt))
        return vec->vserialize();
    return opt.serialize();
}

std::string base64(const void* data, size_t size)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(size), '\0');
    out.resize(b64::encode(out.data(), data, size));
    return out;
}

}} // namespace Slic3r::AIAssistant
