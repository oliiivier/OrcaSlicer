#include "PrinterAutoConnect.hpp"
#include "McpSettings.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include <boost/log/trivial.hpp>

#include <wx/timer.h>

#include <nlohmann/json.hpp>

#include <memory>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;

namespace {

// The gateway and the printer list are usually ready within seconds.
constexpr int POLL_MS  = 3000;
constexpr int ATTEMPTS = 40; // two minutes

class AutoConnect : public wxEvtHandler
{
public:
    AutoConnect() : m_timer(this)
    {
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); });
        m_timer.Start(POLL_MS);
    }

    void stop() { m_timer.Stop(); }

private:
    void poll()
    {
        if (++m_attempts > ATTEMPTS || wxGetApp().gateway_device_connected()) {
            stop();
            return;
        }
        const std::shared_ptr<Gateway::GatewayService> gateway = wxGetApp().gateway_service();
        if (m_waiting || !gateway || !gateway->is_connected())
            return;

        m_waiting = true;
        gateway->request("query.device.list", json::object(), [this](Gateway::GatewayError error, const json& result) {
            m_waiting = false;
            if (!error && m_timer.IsRunning())
                connect_single(result);
        });
    }

    void connect_single(const json& result)
    {
        const json devices = result.value("devices", json::array());
        const json* only   = nullptr;
        int         online = 0;
        for (const json& device : devices)
            if (device.is_object() && device.value("online", 0) == 1) {
                ++online;
                only = &device;
            }
        if (online != 1) {
            // None yet (keep trying) or several: the user chooses.
            if (online > 1)
                stop();
            return;
        }
        // One attempt: connecting can take a while, and the slicer learns the result
        // from the gateway notifications.
        stop();
        if (wxGetApp().gateway_device_connected() || only->value("connected", false))
            return;
        json params = { { "type", only->value("type", std::string("LAN")) } };
        for (const char* key : { "sn", "ip" })
            if (const std::string value = only->value(key, std::string()); !value.empty())
                params[key] = value;
        BOOST_LOG_TRIVIAL(warning) << "Auto-connecting to the only online printer: " << only->value("name", std::string());
        const auto gateway = wxGetApp().gateway_service();
        if (gateway)
            gateway->request("action.device.connect", params, [](Gateway::GatewayError error, const json&) {
                if (error)
                    BOOST_LOG_TRIVIAL(warning) << "Auto-connect to the printer failed: " << error.message;
            });
    }

    wxTimer m_timer;
    int     m_attempts { 0 };
    bool    m_waiting { false };
};

} // namespace

void start_printer_auto_connect()
{
    static bool started = false;
    if (started || !McpSettings::load().auto_connect_printer || wxGetApp().mainframe == nullptr)
        return;
    started = true;

    // Stopped with the main frame but never freed: a gateway reply may still be on its
    // way to it during shutdown.
    auto* handler = new AutoConnect();
    wxGetApp().mainframe->Bind(wxEVT_DESTROY, [handler](wxWindowDestroyEvent& evt) {
        if (evt.GetEventObject() == wxGetApp().mainframe)
            handler->stop();
        evt.Skip();
    });
}

}} // namespace Slic3r::AIAssistant
