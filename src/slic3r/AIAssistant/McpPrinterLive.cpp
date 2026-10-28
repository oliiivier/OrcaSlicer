#include "McpPrinterLive.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/GatewayService.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>


#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;

namespace {

using namespace std::chrono_literals;

constexpr auto QUERY_TIMEOUT = 3s;
constexpr auto WATCH_POLL       = 2s;

const char* ACCESS_DISABLED = "printer access is disabled in the slicer (Help > AI Assistant (MCP server))";

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::shared_ptr<Gateway::GatewayService> connected_gateway(std::string& reason)
{
    if (!McpSettings::load().printer_access) {
        reason = ACCESS_DISABLED;
        return nullptr;
    }
    std::shared_ptr<Gateway::GatewayService> gateway = wxGetApp().gateway_service();
    if (!gateway || !gateway->is_connected()) {
        reason = "the slicer is not connected to the Snapmaker Connect service";
        return nullptr;
    }
    if (!wxGetApp().gateway_device_connected()) {
        reason = "no printer is connected in the slicer";
        return nullptr;
    }
    return gateway;
}

std::shared_ptr<Gateway::GatewayService> require_gateway()
{
    std::string reason;
    auto        gateway = connected_gateway(reason);
    if (!gateway)
        throw ToolError("Cannot reach the printer: " + reason);
    return gateway;
}

// ------------------------------------------------------------ camera

struct HttpResult
{
    unsigned    status { 0 };
    std::string body;
    std::string error;
};

// The printer is on the local network: a short timeout keeps the GUI responsive.
HttpResult http_get(const std::string& url)
{
    HttpResult r;
    Http::get(url)
        .timeout_connect(2)
        .timeout_max(5)
        .size_limit(10 * 1024 * 1024)
        .on_complete([&r](std::string body, unsigned status) {
            r.status = status;
            r.body   = std::move(body);
        })
        .on_error([&r](std::string, std::string error, unsigned status) {
            r.status = status;
            r.error  = error.empty() ? "HTTP " + std::to_string(status) : error;
        })
        .perform_sync();
    return r;
}

// The camera sends a compact JPEG (about 150 KB in 1920x1080): it is passed on
// as is. Re-encoding is avoided on purpose: on some Linux builds the
// JPEG library loaded at run time differs from the one compiled against, and
// libjpeg then exits the application.
bool is_jpeg(const std::string& data) { return data.size() > 3 && uint8_t(data[0]) == 0xFF && uint8_t(data[1]) == 0xD8; }

std::string local_time()
{
    const std::time_t t = std::time(nullptr);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

// ------------------------------------------------------------ print watching

const json WATCHED_OBJECTS = { { "print_stats", { "state", "filename", "print_duration", "info", "message" } },
                               { "display_status", { "progress" } },
                               { "webhooks", { "state", "state_message" } } };

json print_summary(const json& objects)
{
    json        out   = json::object();
    const json& stats = objects.value("print_stats", json::object());
    out["state"]      = stats.value("state", std::string());
    if (const std::string file = stats.value("filename", std::string()); !file.empty())
        out["filename"] = file;
    if (stats.contains("print_duration"))
        out["print_duration_s"] = std::lround(stats.value("print_duration", 0.0));
    if (const std::string message = stats.value("message", std::string()); !message.empty())
        out["message"] = message;
    const json info = stats.value("info", json::object());
    if (info.is_object()) {
        if (info.contains("current_layer") && !info["current_layer"].is_null())
            out["layer"] = info["current_layer"];
        if (info.contains("total_layer") && !info["total_layer"].is_null())
            out["total_layers"] = info["total_layer"];
    }
    const json display = objects.value("display_status", json::object());
    if (display.is_object() && display.contains("progress"))
        out["progress_percent"] = std::lround(display.value("progress", 0.0) * 100.);
    const json webhooks = objects.value("webhooks", json::object());
    if (webhooks.is_object()) {
        const std::string fw = webhooks.value("state", std::string());
        if (fw == "error" || fw == "shutdown")
            out["firmware_error"] = webhooks.value("state_message", fw);
    }
    return out;
}

bool printing(const std::string& state) { return state == "printing" || state == "paused"; }

} // namespace

json live_printer_objects(const json& objects, std::string& reason)
{
    const auto gateway = connected_gateway(reason);
    if (!gateway)
        return nullptr;
    const auto result = gateway->request_sync("printer.objects.query", { { "objects", objects } }, QUERY_TIMEOUT);
    if (result.error) {
        reason = "the printer did not answer: " + result.error.message;
        return nullptr;
    }
    const auto status = result.value.find("status");
    if (status == result.value.end() || !status->is_object()) {
        reason = "unexpected answer from the printer";
        return nullptr;
    }
    return *status;
}

json printer_snapshot(const json& /* args */)
{
    const auto gateway = require_gateway();
    // Moonraker serves the camera on the local network, at the printer address.
    const auto        device = gateway->request_sync("query.device.current", json::object(), QUERY_TIMEOUT);
    const std::string ip     = device.error ? std::string() : device.value.value("ip", std::string());
    if (ip.empty())
        throw ToolError("the printer address is unknown: " + (device.error ? device.error.message : std::string("not reported")));

    // The webcam declared to Moonraker takes a picture on request; the camera
    // monitor of the Snapmaker app is not used, its stream lingers for minutes.
    std::string path   = "/webcam/snapshot.jpg";
    const auto  webcams = gateway->request_sync("server.webcams.list", json::object(), QUERY_TIMEOUT);
    if (!webcams.error)
        for (const json& cam : webcams.value.value("webcams", json::array()))
            if (cam.is_object() && cam.value("enabled", true) && !cam.value("snapshot_url", std::string()).empty()) {
                path = cam["snapshot_url"].get<std::string>();
                break;
            }
    const std::string url = boost::algorithm::starts_with(path, "http") ? path : "http://" + ip + (path[0] == '/' ? "" : "/") + path;

    const HttpResult picture = http_get(url);
    if (picture.status != 200)
        throw ToolError("the printer camera did not answer: " + picture.error);
    if (!is_jpeg(picture.body))
        throw ToolError("the camera sent something that is not a JPEG picture");
    return json::array({ { { "type", "image" }, { "data", base64(picture.body.data(), picture.body.size()) }, { "mimeType", "image/jpeg" } },
                         { { "type", "text" }, { "text", "Printer camera, taken at " + local_time() + " (local time)." } } });
}

json watch_print(const json& args, json& state)
{
    const std::string until = args.value("until", std::string("state_change"));
    if (until != "state_change" && until != "end" && until != "layer")
        throw ToolError("'until' must be state_change, end or layer");

    const std::int64_t now = now_ms();
    std::string        reason;
    if (!state.contains("start")) {
        const int  timeout = std::clamp(args.value("timeout_s", 120), 10, 600);
        const json objects = live_printer_objects(WATCHED_OBJECTS, reason);
        if (objects.is_null())
            throw ToolError("Cannot reach the printer: " + reason);
        const json start = print_summary(objects);
        if (start.contains("firmware_error"))
            return { { "event", "firmware_error" }, { "print", start }, { "waited_s", 0 } };
        if (until != "state_change" && !printing(start.value("state", std::string())))
            return { { "event", "not_printing" }, { "print", start }, { "waited_s", 0 } };
        state["start"]       = start;
        state["started_ms"]  = now;
        state["deadline_ms"] = now + int64_t(timeout) * 1000;
        throw ToolRetry();
    }

    if (now < state.value("next_poll_ms", std::int64_t(0)))
        throw ToolRetry();
    state["next_poll_ms"] = now + std::chrono::duration_cast<std::chrono::milliseconds>(WATCH_POLL).count();

    const json& start   = state["start"];
    const auto  waited  = (now - state["started_ms"].get<std::int64_t>()) / 1000;
    const json  objects = live_printer_objects(WATCHED_OBJECTS, reason);
    if (objects.is_null()) {
        // Transient losses are retried; the deadline still ends the wait.
        if (now > state["deadline_ms"].get<std::int64_t>())
            return { { "event", "timeout" }, { "print", start }, { "waited_s", waited }, { "warning", reason } };
        throw ToolRetry();
    }
    const json current = print_summary(objects);

    std::string event;
    const std::string from = start.value("state", std::string()), to = current.value("state", std::string());
    if (current.contains("firmware_error"))
        event = "firmware_error";
    else if (until == "end" ? (printing(from) && !printing(to)) : from != to)
        event = until == "end" ? "finished" : "state_changed";
    else if (until == "layer" && current.value("layer", json()) != start.value("layer", json()))
        event = "new_layer";
    else if (now > state["deadline_ms"].get<std::int64_t>())
        event = "timeout";
    if (event.empty())
        throw ToolRetry();

    json out = { { "event", event }, { "print", current }, { "waited_s", waited } };
    if (from != to)
        out["previous_state"] = from;
    if (start.contains("progress_percent") && current.contains("progress_percent"))
        out["progress_gained_percent"] = current["progress_percent"].get<long>() - start["progress_percent"].get<long>();
    return out;
}

}} // namespace Slic3r::AIAssistant
