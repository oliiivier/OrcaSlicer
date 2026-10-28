#include "McpSettings.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <openssl/rand.h>

#include <stdexcept>

namespace Slic3r { namespace AIAssistant {

static const char* SECTION = "ai_assistant";

// AppConfig stores booleans as "true"/"false"; "1"/"0" were written by older builds.
static bool is_true(const std::string& value) { return value == "true" || value == "1"; }

McpSettings McpSettings::load()
{
    McpSettings s;
    AppConfig*  cfg = GUI::wxGetApp().app_config;
    if (cfg == nullptr)
        return s;

    std::string value;
    if (cfg->get(SECTION, "mcp_enabled", value))
        s.enabled = is_true(value);
    if (cfg->get(SECTION, "mcp_port", value) && !value.empty()) {
        try {
            const int port = std::stoi(value);
            if (port > 1024 && port < 65536)
                s.port = static_cast<unsigned short>(port);
        } catch (const std::exception&) {}
    }
    cfg->get(SECTION, "mcp_token", s.token);
    if (cfg->get(SECTION, "mcp_printer_access", value))
        s.printer_access = is_true(value);
    if (cfg->get(SECTION, "mcp_printer_control", value))
        s.printer_control = is_true(value);
    if (cfg->get(SECTION, "auto_connect_printer", value) && !value.empty())
        s.auto_connect_printer = is_true(value);

    if (s.token.empty()) {
        s.token = generate_token();
        cfg->set(SECTION, "mcp_token", s.token);
    }
    return s;
}

void McpSettings::save() const
{
    AppConfig* cfg = GUI::wxGetApp().app_config;
    if (cfg == nullptr)
        return;

    cfg->set(SECTION, "mcp_enabled", enabled);
    cfg->set(SECTION, "mcp_port", std::to_string(port));
    cfg->set(SECTION, "mcp_token", token);
    cfg->set(SECTION, "mcp_printer_access", printer_access);
    cfg->set(SECTION, "mcp_printer_control", printer_control);
    cfg->set(SECTION, "auto_connect_printer", auto_connect_printer);
}

std::string McpSettings::generate_token()
{
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("Could not generate a random MCP token");

    static const char hex[] = "0123456789abcdef";
    std::string       token;
    token.reserve(2 * sizeof(buf));
    for (unsigned char c : buf) {
        token += hex[c >> 4];
        token += hex[c & 0xf];
    }
    return token;
}

}} // namespace Slic3r::AIAssistant
