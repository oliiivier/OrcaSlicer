#include "McpPrinter.hpp"
#include "McpMaterials.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/GatewayDevice.hpp"
#include "slic3r/Utils/GatewayProtocol.hpp"
#include "slic3r/Utils/GatewayService.hpp"
#include "slic3r/Utils/SnapLogClient.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;

namespace {

// The gateway runs locally: a slow answer means the printer link is down.
constexpr auto   QUERY_TIMEOUT         = std::chrono::seconds(3);
// get_issues is called often; the loaded filaments rarely change within seconds.
constexpr auto   ISSUES_CACHE_TTL      = std::chrono::seconds(10);
constexpr size_t RAW_OBJECTS_MAX_CHARS = 20000;

const char* ACCESS_DISABLED = "printer access is disabled in the slicer (Help > AI Assistant (MCP server))";

// Result of query.device.objects, or nullopt with the reason in `reason`.
std::optional<json> query_objects(std::string& reason, bool allow_cached)
{
    if (!McpSettings::load().printer_access) {
        reason = ACCESS_DISABLED;
        return std::nullopt;
    }
    const std::shared_ptr<Gateway::GatewayService> gateway = wxGetApp().gateway_service();
    if (!gateway || !gateway->is_connected()) {
        reason = "the slicer is not connected to the Snapmaker Connect service";
        return std::nullopt;
    }
    if (!wxGetApp().gateway_device_connected()) {
        reason = "no printer is connected in the slicer";
        return std::nullopt;
    }

    static json                                  cached;
    static std::chrono::steady_clock::time_point cached_at;
    const auto                                   now = std::chrono::steady_clock::now();
    if (allow_cached && !cached.is_null() && now - cached_at < ISSUES_CACHE_TTL)
        return cached;

    // Read-only query; the callback runs on the gateway thread, so waiting here is safe.
    const Gateway::GatewayService::ApiResult result = gateway->request_sync("query.device.objects", json::object(), QUERY_TIMEOUT);
    if (result.error) {
        reason = "the printer did not answer: " + result.error.message;
        return std::nullopt;
    }
    cached    = result.value;
    cached_at = now;
    return result.value;
}

bool is_sensitive_key(std::string key)
{
    boost::algorithm::to_lower(key);
    static const std::set<std::string> exact = { "sn", "ip", "mac", "key", "host", "user", "uid" };
    static const char* parts[] = { "serial", "token", "password", "passwd", "secret", "ssid", "wifi", "email", "account", "mac_addr",
                                   "ip_addr", "ipv4", "ipv6", "user_id", "userid", "license", "cert" };
    if (exact.count(key))
        return true;
    for (const char* part : parts)
        if (key.find(part) != std::string::npos)
            return true;
    return false;
}

// Removes identifiers and secrets before anything is sent to the model.
void redact(json& value)
{
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end();) {
            if (is_sensitive_key(it.key()))
                it = value.erase(it);
            else
                redact(*it++);
        }
    } else if (value.is_array()) {
        for (json& item : value)
            redact(item);
    } else if (value.is_string()) {
        value = SnapLog::mask_secret_in_value(value.get<std::string>());
    }
}

json pick(const json& object, std::initializer_list<const char*> keys)
{
    json out = json::object();
    for (const char* key : keys)
        if (const auto it = object.find(key); it != object.end() && !it->is_null())
            out[key] = *it;
    return out;
}

json print_state(const json& objects)
{
    json state = json::object();
    if (const auto it = objects.find("print_stats"); it != objects.end() && it->is_object()) {
        state = pick(*it, { "state", "filename", "print_duration", "total_duration", "filament_used", "message" });
        if (const auto info = it->find("info"); info != it->end() && info->is_object())
            state["layer"] = pick(*info, { "current_layer", "total_layer" });
    }
    for (const char* name : { "display_status", "virtual_sdcard" })
        if (const auto it = objects.find(name); it != objects.end() && it->is_object() && it->contains("progress")) {
            state["progress"] = (*it)["progress"];
            break;
        }
    if (const auto it = objects.find("webhooks"); it != objects.end() && it->is_object())
        state["firmware"] = pick(*it, { "state", "state_message" });
    return state;
}

json temperatures(const json& objects)
{
    json temps = json::object();
    for (const auto& [name, object] : objects.items())
        if (object.is_object() && object.contains("temperature") &&
            (name == "heater_bed" || boost::algorithm::starts_with(name, "extruder") ||
             boost::algorithm::starts_with(name, "heater_generic") || boost::algorithm::starts_with(name, "temperature_sensor")))
            temps[name] = pick(object, { "temperature", "target" });
    return temps;
}

// Error reported by the firmware for the last print, if any.
std::string last_error(const json& state)
{
    if (state.value("state", std::string()) == "error" && !state.value("message", std::string()).empty())
        return state["message"].get<std::string>();
    if (const auto fw = state.find("firmware"); fw != state.end()) {
        const std::string fw_state = fw->value("state", std::string());
        if ((fw_state == "error" || fw_state == "shutdown") && !fw->value("state_message", std::string()).empty())
            return (*fw)["state_message"].get<std::string>();
    }
    return {};
}

std::vector<ConnectMachineInfo> toolhead_slots(const json& result)
{
    std::vector<ConnectMachineInfo> slots;
    if (!Gateway::GatewayDevice::parse_machine_slots(result, slots))
        slots.clear();
    return slots;
}

std::string normalized(std::string s)
{
    boost::algorithm::trim(s);
    boost::algorithm::to_upper(s);
    return s;
}

// "#RRGGBB", "RRGGBBAA"... -> "RRGGBB"
std::string rgb(const std::string& color)
{
    std::string hex = normalized(color);
    if (!hex.empty() && hex[0] == '#')
        hex.erase(0, 1);
    return hex.substr(0, 6);
}

std::string slot_value(const DynamicPrintConfig& config, const char* key, size_t idx)
{
    const auto* opt = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key));
    if (opt == nullptr)
        return {};
    const std::vector<std::string> values = opt->vserialize();
    return idx < values.size() ? values[idx] : std::string();
}

// 1-based physical filament slots printed on the current plate.
std::set<int> used_slots(GUI::Plater& plater, size_t physical)
{
    std::set<int> used;
    const Print&  print = plater.fff_print();
    if (!print.empty()) {
        for (unsigned int e : print.extruders())
            used.insert(int(e) + 1);
    } else {
        for (const ModelObject* obj : plater.model().objects)
            for (int id : object_filaments(*obj))
                used.insert(id);
    }
    for (auto it = used.begin(); it != used.end();)
        it = (*it < 1 || size_t(*it) > physical) ? used.erase(it) : std::next(it); // mixed filaments are virtual
    return used;
}

json printer_issue(const char* severity, int slot, const std::string& message)
{
    return { { "severity", severity }, { "blocks_slicing", false }, { "source", "printer" }, { "message", message },
             { "filament_slots", { slot } } };
}

} // namespace

json printer_status(const json& args)
{
    std::string reason;
    const auto  result = query_objects(reason, false);
    if (!result)
        return { { "connected", false }, { "reason", reason } };

    const auto objects_opt = Gateway::parse_device_object_query_result(*result);
    const json objects     = objects_opt ? *objects_opt : json::object();

    json out = { { "connected", true } };

    std::string              model, name;
    std::vector<std::string> nozzles, flows;
    if (Gateway::GatewayDevice::query_machine_info(wxGetApp().gateway_service(), model, nozzles, flows, name))
        out["model"] = model;

    json toolheads = json::array();
    for (const ConnectMachineInfo& slot : toolhead_slots(*result))
        toolheads.push_back({ { "toolhead", slot.index + 1 },
                              { "filament_loaded", slot.filament_info != "NONE" },
                              { "filament", slot.filament_info },
                              { "type", slot.filament_type },
                              { "color", slot.color_info },
                              { "nozzle_diameter", slot.nozzle_info },
                              { "nozzle_flow", slot.nozzle_volume_type } });
    out["toolheads"] = std::move(toolheads);

    json state = print_state(objects);
    if (const std::string error = last_error(state); !error.empty())
        out["last_error"] = error;
    out["print"]        = std::move(state);
    out["temperatures"] = temperatures(objects);

    // Raw firmware objects on request, for diagnosis (sensors, defect detection...).
    const std::vector<std::string> requested = string_list(args, "objects");
    if (!requested.empty()) {
        json raw = json::object();
        for (const std::string& key : requested)
            if (const auto it = objects.find(key); it != objects.end())
                raw[key] = *it;
        redact(raw);
        std::string text = raw.dump();
        if (text.size() > RAW_OBJECTS_MAX_CHARS)
            out["objects_error"] = "too large (" + std::to_string(text.size()) + " characters), request fewer objects";
        else
            out["objects"] = std::move(raw);
    }

    json names = json::array();
    for (const auto& item : objects.items())
        names.push_back(item.key());
    out["available_objects"] = std::move(names);
    redact(out);
    return out;
}

void add_printer_issues(json& issues)
{
    std::string reason;
    const auto  result = query_objects(reason, true);
    if (!result)
        return;
    const std::vector<ConnectMachineInfo> slots = toolhead_slots(*result);
    if (slots.empty())
        return;

    GUI::Plater&             plater = require_plater();
    const PresetBundle&      bundle = *wxGetApp().preset_bundle;
    const DynamicPrintConfig config = bundle.full_config();
    const bool               multi_toolhead = !config.opt_bool("single_extruder_multi_material");

    for (int slot : used_slots(plater, bundle.filament_presets.size())) {
        if (size_t(slot) > slots.size()) {
            issues.push_back(printer_issue("error", slot,
                                           "Filament slot " + std::to_string(slot) + " is used but the printer has only " +
                                               std::to_string(slots.size()) + " toolheads."));
            continue;
        }
        const ConnectMachineInfo& head = slots[size_t(slot) - 1];
        const std::string         tool = "toolhead " + std::to_string(slot);

        if (head.filament_info == "NONE") {
            issues.push_back(printer_issue("error", slot, "No filament is loaded in " + tool + ", used by filament slot " +
                                                              std::to_string(slot) + "."));
            continue;
        }

        const std::string project_type = slot_value(config, "filament_type", size_t(slot - 1));
        if (normalized(project_type) != normalized(head.filament_type)) {
            json issue = printer_issue("warning", slot,
                                       "The project expects " + project_type + " in filament slot " + std::to_string(slot) +
                                           " but " + tool + " holds " + head.filament_info + ".");
            issue["fix_hint"] = "Load the expected filament, or sync the filaments from the printer in the slicer and check "
                                "the settings of that slot.";
            issues.push_back(std::move(issue));
        } else if (const std::string color = slot_value(config, "filament_colour", size_t(slot - 1));
                   !head.color_info.empty() && rgb(color) != rgb(head.color_info)) {
            issues.push_back(printer_issue("warning", slot,
                                           "Filament slot " + std::to_string(slot) + " is " + color + " in the project but " +
                                               tool + " holds color " + head.color_info + "."));
        }

        const std::string project_nozzle = slot_value(config, "nozzle_diameter", multi_toolhead ? size_t(slot - 1) : 0);
        try {
            if (!project_nozzle.empty() && !head.nozzle_info.empty() &&
                std::abs(std::stod(project_nozzle) - std::stod(head.nozzle_info)) > 1e-3)
                issues.push_back(printer_issue("error", slot,
                                               "The printer preset uses a " + project_nozzle + " mm nozzle for " + tool +
                                                   " but a " + head.nozzle_info + " mm nozzle is installed."));
        } catch (const std::exception&) {
            // Unparsable nozzle description: nothing reliable to compare.
        }
    }
}

}} // namespace Slic3r::AIAssistant
