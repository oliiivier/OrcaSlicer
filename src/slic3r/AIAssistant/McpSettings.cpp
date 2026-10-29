#include "McpSettings.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/rand.h>

#include <stdexcept>

namespace Slic3r { namespace AIAssistant {

static const char* SECTION = "ai_assistant";

// AppConfig stores booleans as "true"/"false"; "1"/"0" were written by older builds.
static bool is_true(const std::string& value) { return value == "true" || value == "1"; }

namespace fs = boost::filesystem;

static fs::path token_file() { return fs::path(data_dir()) / "ai_assistant" / "mcp_token"; }

static std::string read_token()
{
    std::string              token;
    boost::nowide::ifstream in(token_file().string(), std::ios::binary);
    if (in)
        std::getline(in, token);
    return token;
}

// Empty removes the file.
static void write_token(const std::string& token)
{
    const fs::path            path = token_file();
    boost::system::error_code ec;
    if (token.empty()) {
        fs::remove(path, ec);
        return;
    }
    if (read_token() == token)
        return;
    fs::create_directories(path.parent_path(), ec);
    // Created empty and restricted before the token is written into it.
    { boost::nowide::ofstream touch(path.string(), std::ios::binary | std::ios::trunc); }
    fs::permissions(path, fs::owner_read | fs::owner_write, ec);
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << token;
    if (!out)
        BOOST_LOG_TRIVIAL(error) << "MCP: could not write " << path.string();
}

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
    // Older builds kept the token in AppConfig, readable by every account, and always let
    // external clients in: such a setup keeps working, with the token moved to its own file.
    std::string legacy_token;
    cfg->get(SECTION, "mcp_token", legacy_token);
    if (cfg->get(SECTION, "mcp_external_access", value) && !value.empty())
        s.external_access = is_true(value);
    else
        s.external_access = !legacy_token.empty();
    if (!legacy_token.empty()) {
        if (s.external_access && read_token().empty())
            write_token(legacy_token);
        cfg->erase(SECTION, "mcp_token");
        cfg->set(SECTION, "mcp_external_access", s.external_access);
    }
    if (s.external_access)
        s.token = read_token();
    else
        write_token({});
    if (cfg->get(SECTION, "mcp_printer_access", value))
        s.printer_access = is_true(value);
    if (cfg->get(SECTION, "mcp_printer_control", value))
        s.printer_control = is_true(value);
    if (cfg->get(SECTION, "auto_connect_printer", value) && !value.empty())
        s.auto_connect_printer = is_true(value);
    if (cfg->get(SECTION, "suggest_adaptation", value) && !value.empty())
        s.suggest_adaptation = is_true(value);
    cfg->get(SECTION, "claude_path", s.claude_path);
    cfg->get(SECTION, "notes_dir", s.notes_dir);
    if (cfg->get(SECTION, "notes_writable", value) && !value.empty())
        s.notes_writable = is_true(value);
    if (cfg->get(SECTION, "manufacturer_docs", value) && !value.empty())
        s.manufacturer_docs = is_true(value);

    if (s.external_access && s.token.empty()) {
        s.token = generate_token();
        write_token(s.token);
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
    cfg->set(SECTION, "mcp_external_access", external_access);
    write_token(external_access ? token : std::string());
    cfg->set(SECTION, "mcp_printer_access", printer_access);
    cfg->set(SECTION, "mcp_printer_control", printer_control);
    cfg->set(SECTION, "auto_connect_printer", auto_connect_printer);
    cfg->set(SECTION, "suggest_adaptation", suggest_adaptation);
    cfg->set(SECTION, "claude_path", claude_path);
    cfg->set(SECTION, "notes_dir", notes_dir);
    cfg->set(SECTION, "notes_writable", notes_writable);
    cfg->set(SECTION, "manufacturer_docs", manufacturer_docs);
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
