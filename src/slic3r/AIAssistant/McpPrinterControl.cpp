#include "McpPrinterControl.hpp"
#include "McpPrinterLive.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::wxGetApp;

namespace {

using namespace std::chrono_literals;

// The feed module state changes within seconds.
constexpr auto POLL_INTERVAL = 1s;
// Heating then unloading one toolhead takes about a minute and a half.
constexpr auto TOOLHEAD_TIMEOUT = 6min;
// The printer screen continues any unload by itself once the nozzle is hot;
// the tool only sends that step when nobody did after this delay.
constexpr auto SCREEN_TAKEOVER_DELAY = 15s;

const char* CONTROL_DISABLED = "printer control is disabled in the slicer (Help > AI Assistant (MCP server), \"Allow unloading "
                               "filament\"). Ask the user to enable it, or to unload the filament from the printer screen.";

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t ms(std::chrono::milliseconds d) { return d.count(); }

// ------------------------------------------------------------ printer access

// Current values of the objects used here, read from the printer itself: the
// device view of the gateway is refreshed too rarely to follow an unload.
json live_objects(std::string& reason)
{
    const McpSettings settings = McpSettings::load();
    if (!settings.printer_access || !settings.printer_control) {
        reason = CONTROL_DISABLED;
        return nullptr;
    }
    return live_printer_objects({ { "filament_feed left", nullptr },
                                  { "filament_feed right", nullptr },
                                  { "machine_state_manager", nullptr },
                                  { "print_stats", { "state" } },
                                  { "print_task_config", { "filament_soft", "filament_vendor", "filament_type", "filament_sub_type" } } },
                                reason);
}

// Replies to the G-code scripts sent without waiting, by script number.
struct Reply
{
    bool        done { false };
    std::string error;
};

std::map<int, Reply>& replies()
{
    static std::map<int, Reply> map;
    return map;
}

// Moonraker may answer printer.gcode.script only after minutes (or never, for
// the feed steps): send it asynchronously and look at the reply later. The
// gateway delivers the reply on the GUI thread.
int send_script(const std::string& script)
{
    static int last_id = 0;
    const int  id      = ++last_id;
    replies()[id];
    BOOST_LOG_TRIVIAL(info) << "MCP: printer.gcode.script " << script;

    const std::shared_ptr<Gateway::GatewayService> gateway = wxGetApp().gateway_service();
    if (!gateway) {
        replies()[id] = { true, "the slicer is not connected to the Snapmaker Connect service" };
        return id;
    }
    gateway->request("printer.gcode.script", { { "script", script } }, [id](Gateway::GatewayError error, const json&) {
        Reply& reply = replies()[id];
        reply.done   = true;
        if (error)
            reply.error = error.message.empty() ? "the printer refused the command" : error.message;
    });
    return id;
}

// Error returned by the firmware for a script, if its reply came. A reply that
// timed out in the gateway says nothing about the command.
std::string script_error(int id)
{
    const auto it = replies().find(id);
    if (it == replies().end() || !it->second.done)
        return {};
    std::string error = it->second.error;
    replies().erase(it);
    return boost::algorithm::istarts_with(error, "timeout") ? std::string() : error;
}

bool script_done(int id)
{
    const auto it = replies().find(id);
    return it == replies().end() || it->second.done;
}

std::string batch(const std::string& action, int extruder = -1, const char* stage = nullptr)
{
    std::string script = "AUTO_FEEDING_BATCH ACTION=" + action;
    if (stage != nullptr)
        script += std::string(" STAGE=") + stage + " EXTRUDER=" + std::to_string(extruder) + " UNLOAD=1";
    return script;
}

// ------------------------------------------------------------ printer objects

// Feed channel of a 0-based extruder ("filament_feed left" holds extruder0 and extruder1...).
const json* feed_channel(const json& objects, int extruder)
{
    const std::string key = "extruder" + std::to_string(extruder);
    for (const auto& [name, object] : objects.items())
        if (boost::algorithm::starts_with(name, "filament_feed") && object.is_object())
            if (const auto it = object.find(key); it != object.end() && it->is_object())
                return &*it;
    return nullptr;
}

int toolhead_count(const json& objects)
{
    int count = 0;
    while (feed_channel(objects, count) != nullptr)
        ++count;
    return count;
}

std::string print_state(const json& objects)
{
    const auto it = objects.find("print_stats");
    return it != objects.end() && it->is_object() ? it->value("state", std::string()) : std::string();
}

bool machine_idle(const json& objects)
{
    const auto it = objects.find("machine_state_manager");
    return it == objects.end() || !it->is_object() || it->value("main_state", 0) == 0;
}

json task_value(const json& objects, const char* key, int extruder)
{
    const auto config = objects.find("print_task_config");
    if (config == objects.end() || !config->is_object())
        return nullptr;
    const auto values = config->find(key);
    return values != config->end() && values->is_array() && size_t(extruder) < values->size() ? (*values)[size_t(extruder)] : json();
}

std::string filament_name(const json& objects, int extruder)
{
    std::string name;
    for (const char* key : { "filament_vendor", "filament_type", "filament_sub_type" }) {
        const json value = task_value(objects, key, extruder);
        if (value.is_string() && !value.get<std::string>().empty() && value.get<std::string>() != "NONE")
            name += (name.empty() ? "" : " ") + value.get<std::string>();
    }
    return name.empty() ? "unknown filament" : name;
}

std::string channel_error(const json& channel)
{
    const std::string error = channel.value("channel_error", std::string("ok"));
    if (error == "ok")
        return {};
    return "feed module error: " + error + " (" + channel.value("channel_error_state", std::string()) + ")";
}

// ------------------------------------------------------------ sequence

bool confirm(const json& objects, const std::vector<int>& extruders, const std::string& reason)
{
    std::string text = reason.empty() ? std::string() : reason + "\n\n";
    for (int e : extruders)
        text += "- " + _u8L("Toolhead") + " " + std::to_string(e + 1) + ": " + filament_name(objects, e) + "\n";
    GUI::MessageDialog dlg(wxGetApp().mainframe,
                           from_u8(text) + "\n" +
                               _L("The AI assistant asks to unload the filament of these toolheads. The printer heats each "
                                  "nozzle and moves. Unload now?"),
                           _L("AI Assistant"), wxYES_NO | wxICON_QUESTION);
    return dlg.ShowModal() == wxID_YES;
}

int current_extruder(const json& state) { return state["extruders"][state["next"].get<size_t>()].get<int>(); }

void record(json& state, const char* status, const std::string& detail = {})
{
    json       entry    = { { "toolhead", current_extruder(state) + 1 }, { "status", status } };
    const bool unloaded = std::string(status) == "unloaded";
    if (!detail.empty())
        entry[unloaded ? "detail" : "reason"] = detail;
    if (unloaded)
        entry["seconds"] = (now_ms() - state["step_started_ms"].get<std::int64_t>()) / 1000;
    state["results"].push_back(std::move(entry));
}

// Records the failure of the current toolhead, skips the others and ends the sequence.
void fail(json& state, const std::string& error)
{
    record(state, "failed", error);
    for (size_t i = state["next"].get<size_t>() + 1; i < state["extruders"].size(); ++i)
        state["results"].push_back(
            { { "toolhead", state["extruders"][i].get<int>() + 1 }, { "status", "skipped" }, { "reason", "not attempted after the failure" } });
    state["failed"] = true;
    state["phase"]  = "stop";
}

// Checks the request and asks the user; returns the result when nothing has to be sent.
json begin(const json& args, const std::atomic<bool>& abandoned, json& state)
{
    if (!args.contains("toolheads") || !args["toolheads"].is_array() || args["toolheads"].empty())
        throw ToolError("'toolheads' must list the toolheads to unload, e.g. [1, 3]");

    std::string reason;
    const json  objects = live_objects(reason);
    if (objects.is_null())
        throw ToolError("Cannot unload: " + reason);
    const int count = toolhead_count(objects);
    if (count == 0)
        throw ToolError("This printer does not report filament feed modules: unloading is not supported");

    std::set<int> requested;
    for (const json& t : args["toolheads"]) {
        if (!t.is_number_integer() || t.get<int>() < 1 || t.get<int>() > count)
            throw ToolError("toolheads are numbered from 1 to " + std::to_string(count));
        requested.insert(t.get<int>() - 1);
    }

    const std::string print = print_state(objects);
    if (print == "printing" || print == "paused" || !machine_idle(objects))
        throw ToolError("The printer is busy (print state: " + (print.empty() ? std::string("unknown") : print) +
                        "). Unloading is only possible when the printer is idle.");

    json             results = json::array();
    std::vector<int> to_unload;
    for (int e : requested) {
        const json& channel = *feed_channel(objects, e);
        if (!channel.value("filament_detected", false))
            results.push_back({ { "toolhead", e + 1 }, { "status", "skipped" }, { "reason", "no filament loaded" } });
        else if (task_value(objects, "filament_soft", e) == true)
            // As the firmware, which never unloads soft filaments automatically.
            results.push_back({ { "toolhead", e + 1 },
                                { "status", "skipped" },
                                { "reason", "soft filament (e.g. TPU): unload it from the printer screen" } });
        else if (const std::string error = channel_error(channel); !error.empty())
            results.push_back({ { "toolhead", e + 1 }, { "status", "skipped" }, { "reason", error } });
        else
            to_unload.push_back(e);
    }
    if (to_unload.empty())
        return { { "unloaded", false }, { "toolheads", std::move(results) }, { "message", "Nothing to unload" } };

    if (!confirm(objects, to_unload, args.value("reason", std::string())))
        return { { "unloaded", false }, { "message", "The user declined. Ask them what they want instead." } };
    if (abandoned)
        return { { "unloaded", false }, { "message", "The request timed out before the user answered" } };

    state["extruders"]  = to_unload;
    state["next"]       = 0;
    state["results"]    = std::move(results);
    state["started_ms"] = now_ms();
    // Remembers the heater targets, restored (or turned off) by ACTION=STOP.
    state["script"] = send_script(batch("START"));
    state["phase"]  = "start";
    throw ToolRetry();
}

// Waits for the unload of the current toolhead. The firmware goes through
// unload_homing, unload_picking, unload_heating, unload_heat_finish (where the
// printer screen sends STAGE=doing), unload_doing, then ends idle with the
// filament out of the toolhead: unload_finish, preload_finish (still in the
// feed module) or wait_insert (pulled out).
void follow_unload(json& state, std::int64_t now)
{
    if (const std::string error = script_error(state.value("script", 0)); !error.empty()) {
        fail(state, error);
        return;
    }
    if (now > state["deadline_ms"].get<std::int64_t>()) {
        fail(state, "the unload did not finish in time");
        return;
    }

    std::string reason;
    const json  objects = live_objects(reason);
    if (objects.is_null())
        return; // transient: the deadline above ends the wait
    const int   extruder = current_extruder(state);
    const json* channel  = feed_channel(objects, extruder);
    if (channel == nullptr) {
        fail(state, "the printer no longer reports this toolhead");
        return;
    }
    if (const std::string error = channel_error(*channel); !error.empty()) {
        fail(state, error);
        return;
    }

    const std::string channel_state = channel->value("channel_state", std::string());
    if (boost::algorithm::starts_with(channel_state, "unload_") && channel_state != "unload_finish")
        state["seen_unloading"] = true;

    if (channel_state == "unload_heat_finish") {
        if (!state.contains("heat_finish_ms"))
            state["heat_finish_ms"] = now;
        else if (!state.value("sent_doing", false) && now - state["heat_finish_ms"].get<std::int64_t>() > ms(SCREEN_TAKEOVER_DELAY)) {
            state["script"]     = send_script(batch("DOING", extruder, "doing"));
            state["sent_doing"] = true;
        }
        return;
    }

    if (!machine_idle(objects) || !state.value("seen_unloading", false))
        return;
    if (channel_state == "unload_finish" || boost::algorithm::starts_with(channel_state, "preload") || channel_state == "wait_insert") {
        record(state, "unloaded",
               channel_state == "wait_insert" ? std::string() :
                                                "the filament is out of the toolhead but still in the feed module: pull it out "
                                                "to change the spool");
        const size_t next = state["next"].get<size_t>() + 1;
        state["next"]     = next;
        state["phase"]    = next < state["extruders"].size() ? "prepare" : "stop";
    } else if (channel_state == "load_finish") {
        fail(state, "the printer went back to idle with the filament still loaded");
    }
}

json advance(json& state)
{
    const std::int64_t now = now_ms();
    if (now < state.value("next_poll_ms", std::int64_t(0)))
        throw ToolRetry();
    state["next_poll_ms"] = now + ms(POLL_INTERVAL);

    const std::string phase = state["phase"].get<std::string>();
    if (phase == "start") {
        const int id = state["script"].get<int>();
        if (!script_done(id))
            throw ToolRetry();
        if (const std::string error = script_error(id); !error.empty())
            fail(state, error);
        else
            state["phase"] = "prepare";
    } else if (phase == "prepare") {
        const int extruder       = current_extruder(state);
        state["step_started_ms"] = now;
        state["deadline_ms"]     = now + ms(TOOLHEAD_TIMEOUT);
        state.erase("heat_finish_ms");
        state.erase("sent_doing");
        state.erase("seen_unloading");
        // Heats the nozzle itself (preheat, toolhead change, unload temperature).
        state["script"] = send_script(batch("DOING", extruder, "prepare"));
        state["phase"]  = "unloading";
    } else if (phase == "unloading") {
        follow_unload(state, now);
    } else if (phase == "stop") {
        state["script"] = send_script(batch("STOP"));
        state["phase"]  = "stopping";
    } else if (phase == "stopping") {
        // The heaters are off once STOP has run (no print is running).
        const int id = state["script"].get<int>();
        if (!script_done(id))
            throw ToolRetry();
        json out = { { "unloaded", !state.value("failed", false) },
                     { "toolheads", state["results"] },
                     { "seconds", (now - state["started_ms"].get<std::int64_t>()) / 1000 } };
        if (const std::string error = script_error(id); !error.empty())
            out["stop_error"] = error;
        state["phase"] = "done";
        return out;
    }
    throw ToolRetry();
}

} // namespace

json unload_filament(const json& args, const std::atomic<bool>& abandoned, json& state)
{
    require_plater();
    if (!state.contains("phase"))
        return begin(args, abandoned, state);
    return advance(state);
}

void unload_filament_abandoned(json& state)
{
    const std::string phase = state.value("phase", std::string());
    if (phase.empty() || phase == "stopping" || phase == "done")
        return;
    BOOST_LOG_TRIVIAL(warning) << "MCP: unload_filament abandoned in phase " << phase << ", ending the sequence";
    send_script(batch("STOP"));
    state["phase"] = "done";
}

}} // namespace Slic3r::AIAssistant
