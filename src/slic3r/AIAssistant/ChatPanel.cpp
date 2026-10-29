#include "ChatPanel.hpp"
#include "AIAssistant.hpp"
#include "McpService.hpp"
#include "McpSettings.hpp"
#include "ScreenCapture.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/aui/framemanager.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/webview.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

namespace Slic3r { namespace AIAssistant {

namespace fs = boost::filesystem;
using GUI::from_u8;
using GUI::into_u8;
using GUI::wxGetApp;
using nlohmann::json;

namespace {

const char* SERVER_NAME = "snapmaker-orca";
// Tool result images (get_view, get_printer snapshot) larger than this are not shown in the chat.
constexpr size_t MAX_IMAGE_BASE64 = 4 * 1024 * 1024;
// Screenshots are scaled down to this size: Claude would scale larger ones down anyway.
constexpr int SCREENSHOT_MAX_EDGE = 1600;

const char* DOCKED_KEY = "chat_docked";
const char* MODEL_KEY  = "chat_model";
const char* DEFAULT_MODEL = "default";

// Offered until the CLI gives its own list.
json fallback_models()
{
    return json::array({{{"value", DEFAULT_MODEL}, {"displayName", "Default (recommended)"}},
                        {{"value", "opus"}, {"displayName", "Opus"}},
                        {{"value", "sonnet"}, {"displayName", "Sonnet"}},
                        {{"value", "haiku"}, {"displayName", "Haiku"}}});
}

wxWeakRef<ChatPanel>& instance()
{
    static wxWeakRef<ChatPanel> panel;
    return panel;
}

// The plater lays out its sidebar and 3D view with wxAUI: the docked chat is one more pane.
wxAuiManager* plater_aui()
{
    GUI::Plater* plater = wxGetApp().plater();
    return plater ? wxAuiManager::GetManager(plater) : nullptr;
}

const char* SYSTEM_PROMPT =
    "You are the assistant built into the Snapmaker Orca slicer, in a chat window next to the 3D view: "
    "the user sees the slicer while talking to you. Use the snapmaker-orca tools to inspect and change "
    "the project and the printer; you have no other tools (no shell, no files, no web). "
    "Reply in the language of the user. The chat window is narrow: prefer short paragraphs and lists "
    "to wide tables.";

fs::path assistant_dir() { return fs::path(data_dir()) / "ai_assistant"; }

fs::path chats_file() { return assistant_dir() / "chats.json"; }

// Tab title: the beginning of the first message.
std::string title_from(const std::string& text)
{
    constexpr size_t MAX_CHARS = 40;
    wxString title = from_u8(text);
    title.Replace("\n", " ");
    title.Trim().Trim(false);
    if (title.length() > MAX_CHARS)
        title = title.Left(MAX_CHARS - 1) + wxString::FromUTF8("\u2026");
    return into_u8(title);
}

// What a CLI needs to reach the MCP server: a token of its own and the configuration file that
// gives it, readable by the user only. Both go away with the CLI.
// The CLI gives up on a tool call after 60 s by default: slicing, watching a print or waiting
// for the user to confirm takes longer.
struct McpAccess
{
    McpService::Grant grant;
    fs::path          config;

    ~McpAccess()
    {
        boost::system::error_code ec;
        if (!config.empty())
            fs::remove(config, ec);
    }

    // Returns false when the file could not be written.
    bool write_config(const std::string& url, const std::string& conversation_id)
    {
        const json content = {{"mcpServers",
                               {{SERVER_NAME,
                                 {{"type", "http"},
                                  {"url", url},
                                  {"headers", {{"Authorization", "Bearer " + grant.token()}}},
                                  {"timeout", McpSettings::TOOL_TIMEOUT_MS}}}}}};

        config = assistant_dir() / ("claude_mcp_" + conversation_id + ".json");
        boost::system::error_code ec;
        fs::create_directories(config.parent_path(), ec);
        // Created empty and restricted before the token is written into it.
        { boost::nowide::ofstream touch(config.string(), std::ios::binary | std::ios::trunc); }
        fs::permissions(config, fs::owner_read | fs::owner_write, ec);
        boost::nowide::ofstream out(config.string(), std::ios::binary | std::ios::trunc);
        out << content.dump(2);
        return bool(out);
    }
};

// Configurations left by a crash, or by older builds (claude_mcp.json, with the token of the
// external clients): their tokens are no longer accepted, or must not lie around.
void remove_stale_mcp_configs()
{
    boost::system::error_code ec;
    for (fs::directory_iterator it(assistant_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (boost::algorithm::starts_with(name, "claude_mcp") && it->path().extension() == ".json")
            fs::remove(it->path(), ec);
    }
}

wxString chat_page_url()
{
    fs::path page = fs::path(resources_dir()) / "web" / "ai_assistant" / "chat.html";
    return "file://" + from_u8(page.make_preferred().string());
}

// Texts of the page, translated with the slicer's catalog.
json page_labels()
{
    auto t = [](const wxString& s) { return into_u8(s); };
    return {
        {"title", t(_L("Claude"))},
        {"placeholder", t(_L("Ask about this project, a print problem, a setting... (Enter to send, Shift+Enter for a new line, paste a screenshot)"))},
        {"send", t(_L("Send"))},
        {"stop", t(_L("Stop"))},
        {"new_chat", t(_L("New conversation"))},
        {"settings", t(_L("MCP server settings"))},
        {"welcome", t(_L("Claude can read the project, explain the slicer warnings, propose setting changes (you confirm each one in the slicer) and follow the connected printer."))},
        {"starting", t(_L("Starting Claude Code..."))},
        {"thinking", t(_L("Thinking..."))},
        {"interrupted", t(_L("Stopped."))},
        {"server_off", t(_L("The MCP server of the slicer is off: Claude cannot see the project. Turn it on in the MCP server settings."))},
        {"server_failed", t(_L("Claude Code could not connect to the MCP server of the slicer."))},
        {"not_found", t(_L("Claude Code was not found. Install it (https://claude.com/claude-code), run \"claude\" once in a terminal to log in, then try again."))},
        {"exited", t(_L("Claude Code stopped. Your next message restarts it and continues the conversation."))},
        {"login_hint", t(_L("If you are not logged in, run \"claude\" once in a terminal to log in."))},
        {"remove", t(_L("Remove"))},
        {"capture_view", t(_L("3D view"))},
        {"capture_view_tip", t(_L("Attach a screenshot of the 3D view"))},
        {"capture_window", t(_L("Window"))},
        {"capture_window_tip", t(_L("Attach a screenshot of the whole slicer window"))},
        {"capture_failed", t(_L("The screenshot could not be taken."))},
        {"dock", t(_L("Dock next to the 3D view"))},
        {"new_tab_title", t(_L("New conversation"))},
        {"close_tab", t(_L("Close this conversation"))},
        {"undock", t(_L("Float in its own window"))},
        {"model_tip", t(_L("Choose the model"))},
        {"compacting", t(_L("Compacting the conversation..."))},
        {"compacted", t(_L("Conversation compacted: Claude keeps a summary of it."))},
        {"cleared", t(_L("Conversation cleared: Claude starts from scratch."))},
    };
}

} // namespace

// The floating home of the chat panel. Closing it hides it.
class ChatWindow : public GUI::DPIFrame
{
public:
    explicit ChatWindow(wxWindow* parent)
        : DPIFrame(parent, wxID_ANY, _L("Claude"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        SetSize(FromDIP(wxSize(460, 760)));
        SetMinSize(FromDIP(wxSize(320, 400)));
        // On the right of the main window, where it hides the least of the 3D view.
        if (parent != nullptr) {
            const wxRect r = parent->GetScreenRect();
            SetPosition(wxPoint(std::max(r.GetLeft(), r.GetRight() - GetSize().x - FromDIP(16)), r.GetTop() + FromDIP(80)));
        }
        SetSizer(new wxBoxSizer(wxVERTICAL));
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) {
            if (evt.CanVeto()) {
                evt.Veto();
                Hide();
            } else
                evt.Skip();
        });
    }

    void set_content(wxWindow* content)
    {
        GetSizer()->Clear(false);
        if (content != nullptr)
            GetSizer()->Add(content, 1, wxEXPAND);
        Layout();
    }

protected:
    void on_dpi_changed(const wxRect& /* suggested_rect */) override { Layout(); }
};

void ChatPanel::show()
{
    // Without the server Claude cannot see the project: opening the chat turns it on.
    std::string server_error;
    if (!McpService::is_running()) {
        McpSettings settings = McpSettings::load();
        settings.enabled     = true;
        settings.save();
        if (!McpService::apply(settings, server_error))
            BOOST_LOG_TRIVIAL(error) << "AI chat: could not start the MCP server on port " << settings.port << ": " << server_error;
    }

    wxWeakRef<ChatPanel>& panel = instance();
    if (panel == nullptr) {
        auto* window = new ChatWindow(wxGetApp().mainframe);
        panel        = new ChatPanel(window);
        panel->m_window = window;
        window->set_content(panel);
        if (wxGetApp().app_config->get("ai_assistant", DOCKED_KEY) == "true")
            panel->set_docked(true);
    } else
        panel->sync_server();
    if (!server_error.empty())
        panel->post_status("error", wxString::Format(_L("Could not start the MCP server: %s"), from_u8(server_error)));
    panel->show_to_user(true);
}

void ChatPanel::toggle()
{
    if (instance() != nullptr && instance()->is_shown_to_user())
        instance()->show_to_user(false);
    else
        show();
}

bool ChatPanel::is_shown_to_user() const
{
    if (!m_docked)
        return m_window->IsShown();
    wxAuiManager* mgr = plater_aui();
    return mgr != nullptr && mgr->GetPane(const_cast<ChatPanel*>(this)).IsShown();
}

void ChatPanel::show_to_user(bool show)
{
    if (!m_docked) {
        m_window->Show(show);
        if (show)
            m_window->Raise();
        return;
    }
    if (wxAuiManager* mgr = plater_aui()) {
        mgr->GetPane(this).Show(show);
        mgr->Update();
    }
}

void ChatPanel::set_docked(bool docked)
{
    wxAuiManager* mgr = plater_aui();
    if (docked == m_docked || (docked && mgr == nullptr))
        return;

    if (docked) {
        m_window->set_content(nullptr);
        m_window->Hide();
        Reparent(wxGetApp().plater());
        mgr->AddPane(this, wxAuiPaneInfo()
                               .Name("ai_chat")
                               .Right()
                               .CaptionVisible(false)
                               .CloseButton(false)
                               .Floatable(false)
                               .TopDockable(false)
                               .BottomDockable(false)
                               .LeftDockable(false)
                               .BestSize(FromDIP(wxSize(420, 600)))
                               .MinSize(FromDIP(wxSize(300, 200))));
        mgr->Update();
    } else {
        if (mgr != nullptr) {
            mgr->DetachPane(this);
            mgr->Update();
        }
        Reparent(m_window);
        m_window->set_content(this);
        Show();
        m_window->Show();
        m_window->Raise();
    }
    m_docked = docked;
    wxGetApp().app_config->set("ai_assistant", DOCKED_KEY, docked ? "true" : "false");
    post_layout();
}

struct ChatPanel::Conversation
{
    std::string id;
    std::string title;
    // Claude Code session, continued with --resume.
    std::string session_id;
    // The MCP server URL the CLI was started with.
    std::string server_url;
    bool        busy { false };
    // The page events of the conversation, replayed when it is shown again.
    json history = json::array();
    std::unique_ptr<ClaudeProcess> claude;
};

ChatPanel::ChatPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    m_browser = WebView::CreateWebView(this, chat_page_url());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ChatPanel::on_script_message, this, m_browser->GetId());
    remove_stale_mcp_configs();
    m_model = wxGetApp().app_config->get("ai_assistant", MODEL_KEY);
    if (m_model.empty())
        m_model = DEFAULT_MODEL;
    load();
}

ChatPanel::~ChatPanel()
{
    save();
    // The CLIs stop before the conversations their handlers refer to go away.
    for (auto& conv : m_conversations)
        conv->claude.reset();
}

ChatPanel::Conversation* ChatPanel::find(const std::string& id)
{
    for (auto& conv : m_conversations)
        if (conv->id == id)
            return conv.get();
    return nullptr;
}

ChatPanel::Conversation& ChatPanel::add_conversation()
{
    static unsigned counter = 0;
    auto conv = std::make_unique<Conversation>();
    conv->id  = std::to_string(wxGetUTCTimeMillis().GetValue()) + "-" + std::to_string(++counter);
    m_conversations.push_back(std::move(conv));
    return *m_conversations.back();
}

void ChatPanel::select(Conversation& conv)
{
    if (m_active != nullptr && m_active != &conv && !m_active->busy && m_active->claude)
        m_active->claude->stop(); // restarted with --resume on its next message
    m_active = &conv;
    post_tabs();
    post_replay();
    save();
}

void ChatPanel::close(Conversation& conv)
{
    auto it = std::find_if(m_conversations.begin(), m_conversations.end(), [&conv](const auto& c) { return c.get() == &conv; });
    if (it == m_conversations.end())
        return;
    const bool was_active = m_active == &conv;
    conv.claude.reset();
    it = m_conversations.erase(it);
    if (m_conversations.empty())
        add_conversation();
    if (was_active) {
        m_active = nullptr;
        select(it != m_conversations.end() ? **it : *m_conversations.back());
    } else {
        post_tabs();
        save();
    }
}

void ChatPanel::sync_server()
{
    post_server_state();
    const std::string url = McpService::is_running() ? McpService::url() : std::string();
    for (auto& conv : m_conversations)
        if (conv->claude && conv->claude->is_running() && !conv->busy && conv->server_url != url)
            conv->claude->stop();
}

void ChatPanel::on_script_message(wxWebViewEvent& evt)
{
    const json request = json::parse(into_u8(evt.GetString()), nullptr, false);
    if (!request.is_object())
        return;
    const std::string command = request.value("command", "");

    if (command == "ready") {
        m_page_ready = true;
        post({{"kind", "init"}, {"labels", page_labels()}, {"dark", wxGetApp().dark_mode()}});
        post_server_state();
        post_layout();
        std::vector<json> pending = std::move(m_pending);
        m_pending.clear();
        for (const json& e : pending)
            post(e);
        post_tabs();
        post_models();
        post_replay();
    } else if (command == "send") {
        if (m_active != nullptr)
            send_user_message(*m_active, request);
    } else if (command == "interrupt") {
        if (m_active != nullptr && m_active->busy && m_active->claude)
            m_active->claude->interrupt();
    } else if (command == "new_tab") {
        select(add_conversation());
    } else if (command == "select_tab") {
        if (Conversation* conv = find(request.value("id", "")); conv != nullptr && conv != m_active)
            select(*conv);
    } else if (command == "close_tab") {
        if (Conversation* conv = find(request.value("id", "")))
            close(*conv);
    } else if (command == "open_settings") {
        show_settings();
        sync_server();
    } else if (command == "capture") {
        capture(request.value("target", "") == "window");
    } else if (command == "set_model") {
        set_model(request.value("model", ""));
    } else if (command == "dock") {
        set_docked(request.value("docked", false));
    } else if (command == "resize") {
        begin_resize(request.value("edge", ""));
    } else if (command == "open_url") {
        const std::string url = request.value("url", "");
        if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
            wxLaunchDefaultBrowser(from_u8(url));
    }
}

void ChatPanel::capture(bool whole_window)
{
    wxWeakRef<ChatPanel> self(this);
    auto attach = [self](const wxImage& image) {
        if (self == nullptr)
            return;
        const std::string data = ScreenCapture::to_png_base64(image, SCREENSHOT_MAX_EDGE);
        if (data.empty())
            self->post({{"kind", "capture_failed"}});
        else
            self->post({{"kind", "attach"}, {"media_type", "image/png"}, {"data", data}});
    };
    if (!whole_window) {
        ScreenCapture::capture_view(attach);
        return;
    }
    // The floating chat window covers part of the main window: out of the picture while it is taken.
    const bool hide = !m_docked && m_window->IsShown();
    ScreenCapture::capture_main_window(
        attach, [self, hide]() { if (self && hide) self->m_window->Hide(); },
        [self, hide]() {
            if (self && hide) {
                self->m_window->Show();
                self->m_window->Raise();
            }
        });
}

void ChatPanel::set_model(const std::string& model)
{
    if (model.empty() || model == m_model)
        return;
    m_model = model;
    wxGetApp().app_config->set("ai_assistant", MODEL_KEY, m_model);
    // The CLIs that are not running get it with --model when they start.
    const std::string cli_model = m_model == DEFAULT_MODEL ? std::string() : m_model;
    for (auto& conv : m_conversations)
        if (conv->claude && conv->claude->is_running())
            conv->claude->set_model(cli_model);
    post_models();
}

// Lets the page start resizing the floating window from its edges: the window manager's
// own borders are thin, and the page covers the whole window.
void ChatPanel::begin_resize(const std::string& edge)
{
    if (m_docked)
        return; // the sash of the pane
#if defined(__WXGTK__)
    GdkWindowEdge gdk_edge;
    if (edge == "left")
        gdk_edge = GDK_WINDOW_EDGE_WEST;
    else if (edge == "bottom_left")
        gdk_edge = GDK_WINDOW_EDGE_SOUTH_WEST;
    else if (edge == "bottom_right")
        gdk_edge = GDK_WINDOW_EDGE_SOUTH_EAST;
    else
        return;
    const wxPoint mouse = wxGetMousePosition();
    gtk_window_begin_resize_drag(GTK_WINDOW(m_window->GetHandle()), gdk_edge, 1, mouse.x, mouse.y, GDK_CURRENT_TIME);
#elif defined(__WXMSW__)
    WPARAM side;
    if (edge == "left")
        side = WMSZ_LEFT;
    else if (edge == "bottom_left")
        side = WMSZ_BOTTOMLEFT;
    else if (edge == "bottom_right")
        side = WMSZ_BOTTOMRIGHT;
    else
        return;
    ::ReleaseCapture();
    ::PostMessage(m_window->GetHWND(), WM_SYSCOMMAND, SC_SIZE | side, 0);
#else
    (void) edge; // native resize borders
#endif
}

bool ChatPanel::ensure_started(Conversation& conv)
{
    if (conv.claude && conv.claude->is_running())
        return true;

    const ClaudeProcess::Launch launch = ClaudeProcess::locate(McpSettings::load().claude_path);
    if (!launch.valid()) {
        post_status("error", _L("Claude Code was not found."));
        post({{"kind", "notice"}, {"level", "error"}, {"label", "not_found"}});
        return false;
    }

    ClaudeProcess::Options options;
    options.launch         = launch;
    options.system_prompt  = SYSTEM_PROMPT;
    options.resume_session = conv.session_id;
    if (m_model != DEFAULT_MODEL)
        options.model = m_model;
    // An empty directory: the CLI must not pick up project files or instructions from elsewhere.
    const fs::path cwd = assistant_dir() / "chat";
    boost::system::error_code ec;
    fs::create_directories(cwd, ec);
    options.working_dir = cwd.string();

    conv.server_url.clear();
    if (McpService::is_running()) {
        auto access = std::make_shared<McpAccess>();
        if (access->write_config(McpService::url(), conv.id)) {
            options.mcp_config    = access->config.string();
            options.allowed_tools = std::string("mcp__") + SERVER_NAME;
            options.resources     = access;
            conv.server_url       = McpService::url();
        }
    }

    Conversation* c = &conv;
    if (!conv.claude)
        conv.claude = std::make_unique<ClaudeProcess>([this, c](const json& msg) { on_claude_message(*c, msg); },
                                                      [this, c](int code, const std::string& err) { on_claude_exit(*c, code, err); });
    std::string error;
    if (!conv.claude->start(options, error)) {
        BOOST_LOG_TRIVIAL(error) << "AI chat: " << error;
        post_status("error", from_u8(error));
        return false;
    }
    if (!m_models_asked)
        m_models_asked = conv.claude->initialize();
    return true;
}

void ChatPanel::send_user_message(Conversation& conv, const json& request)
{
    if (conv.busy || !ensure_started(conv))
        return;

    const std::string text   = request.value("text", "");
    const json        images = request.contains("images") && request["images"].is_array() ? request["images"] : json::array();
    json content = json::array();
    for (const json& image : images)
        content.push_back({{"type", "image"},
                           {"source",
                            {{"type", "base64"},
                             {"media_type", image.value("media_type", "image/png")},
                             {"data", image.value("data", "")}}}});
    if (!text.empty())
        content.push_back({{"type", "text"}, {"text", text}});
    if (content.empty())
        return;

    if (!conv.claude->send({{"type", "user"}, {"message", {{"role", "user"}, {"content", content}}}})) {
        post_status("error", _L("Could not send the message to Claude Code."));
        return;
    }
    // The page already shows it.
    post_to(conv, {{"kind", "user"}, {"text", text}, {"images", images}}, false);
    // CLI commands (/compact, /context...) do not name the conversation.
    const bool command = !text.empty() && text.front() == '/';
    if (conv.title.empty() && !text.empty() && !command) {
        conv.title = title_from(text);
        post_tabs();
    }
    // The CLI starts a new session, which its next init message gives: the page starts over too.
    if (command && boost::algorithm::trim_copy(text) == "/clear") {
        conv.history = json::array();
        conv.title.clear();
        post_tabs();
        post_replay();
        post_to(conv, {{"kind", "info"}, {"label", "cleared"}});
    }
    conv.busy = true;
    post_busy(conv);
    save();
}

// Turns the CLI messages into the few events the page draws. Tool results can be large
// (images in base64): only what is shown is passed to the page.
void ChatPanel::on_claude_message(Conversation& conv, const json& msg)
{
    const std::string type = msg.value("type", "");
    // Messages of subagents: not shown.
    if (msg.contains("parent_tool_use_id") && !msg["parent_tool_use_id"].is_null())
        return;

    if (type == "control_response") {
        // The answer to initialize(): the models this account can use.
        const json& response = msg.contains("response") ? msg["response"] : json();
        if (response.is_object() && response.value("subtype", "") == "success" && response.contains("response") &&
            response["response"].is_object() && response["response"].contains("models") &&
            response["response"]["models"].is_array()) {
            json models = json::array();
            for (const json& m : response["response"]["models"])
                if (m.is_object() && m.contains("value") && m["value"].is_string())
                    models.push_back({{"value", m["value"]},
                                      {"displayName", m.value("displayName", m["value"].get<std::string>())},
                                      {"description", m.value("description", "")}});
            if (!models.empty() && models != m_models) {
                m_models = std::move(models);
                post_models();
                save();
            }
        }
    } else if (type == "system" && msg.value("subtype", "") == "init") {
        conv.session_id = msg.value("session_id", "");
        bool connected = false;
        if (msg.contains("mcp_servers"))
            for (const json& server : msg["mcp_servers"])
                if (server.value("name", "") == SERVER_NAME)
                    connected = server.value("status", "") == "connected";
        if (!conv.server_url.empty() && !connected && &conv == m_active)
            post({{"kind", "notice"}, {"level", "warning"}, {"label", "server_failed"}});
    } else if (type == "system" && msg.value("subtype", "") == "status" && msg.value("status", json()) == "compacting") {
        post_to(conv, {{"kind", "info"}, {"label", "compacting"}});
    } else if (type == "system" && msg.value("subtype", "") == "compact_boundary") {
        post_to(conv, {{"kind", "info"}, {"label", "compacted"}});
    } else if (type == "user" && msg.contains("message") && msg["message"].contains("content") &&
               msg["message"]["content"].is_string()) {
        // Output of a CLI command (/context, /cost...); "Compacted" is already shown above.
        const std::string content = msg["message"]["content"].get<std::string>();
        const std::string open = "<local-command-stdout>", close = "</local-command-stdout>";
        const size_t      from = content.find(open);
        const size_t      to   = from == std::string::npos ? std::string::npos : content.find(close, from);
        if (to != std::string::npos) {
            const std::string out = boost::algorithm::trim_copy(content.substr(from + open.size(), to - from - open.size()));
            if (!out.empty() && out != "Compacted")
                post_to(conv, {{"kind", "info"}, {"text", out}});
        }
    } else if (type == "stream_event" && msg.contains("event")) {
        const json&       event = msg["event"];
        const std::string etype = event.value("type", "");
        if (etype == "content_block_start" && event.contains("content_block")) {
            const json&       block = event["content_block"];
            const std::string btype = block.value("type", "");
            if (btype == "text")
                post_to(conv, {{"kind", "text_start"}});
            else if (btype == "tool_use")
                post_to(conv, {{"kind", "tool_start"}, {"id", block.value("id", "")}, {"name", block.value("name", "")}});
            else if (btype == "thinking" && &conv == m_active)
                post({{"kind", "thinking"}});
        } else if (etype == "content_block_delta" && event.contains("delta")) {
            const json& delta = event["delta"];
            if (delta.value("type", "") == "text_delta")
                post_to(conv, {{"kind", "text_delta"}, {"text", delta.value("text", "")}});
        } else if (etype == "content_block_stop") {
            post_to(conv, {{"kind", "block_stop"}});
        }
    } else if (type == "assistant" && msg.contains("message") && msg["message"].contains("content") &&
               msg["message"]["content"].is_array()) {
        // The complete tool calls: the action of a tool grouping several is shown next to its name.
        for (const json& block : msg["message"]["content"])
            if (block.value("type", "") == "tool_use" && block.contains("input") && block["input"].is_object() &&
                block["input"].contains("action") && block["input"]["action"].is_string())
                post_to(conv, {{"kind", "tool_action"}, {"id", block.value("id", "")}, {"action", block["input"]["action"]}});
    } else if (type == "user" && msg.contains("message") && msg["message"].contains("content") &&
               msg["message"]["content"].is_array()) {
        for (const json& block : msg["message"]["content"]) {
            if (block.value("type", "") != "tool_result")
                continue;
            json event = {{"kind", "tool_result"},
                          {"id", block.value("tool_use_id", "")},
                          {"is_error", block.value("is_error", false)},
                          {"images", json::array()}};
            std::string text;
            if (block.contains("content") && block["content"].is_array()) {
                for (const json& part : block["content"]) {
                    const std::string ptype = part.value("type", "");
                    if (ptype == "text" && text.empty())
                        text = part.value("text", "");
                    else if (ptype == "image" && part.contains("source")) {
                        const std::string data = part["source"].value("data", "");
                        if (!data.empty() && data.size() <= MAX_IMAGE_BASE64)
                            event["images"].push_back(
                                {{"media_type", part["source"].value("media_type", "image/png")}, {"data", data}});
                    }
                }
            } else if (block.contains("content") && block["content"].is_string())
                text = block["content"].get<std::string>();
            if (event["is_error"].get<bool>())
                event["text"] = text.substr(0, 600);
            post_to(conv, event);
        }
    } else if (type == "result") {
        conv.busy             = false;
        const bool is_error   = msg.value("is_error", false);
        const std::string sub = msg.value("subtype", "");
        json event            = {{"kind", "turn_end"}, {"is_error", is_error}, {"subtype", sub}};
        if (is_error && msg.contains("result") && msg["result"].is_string())
            event["text"] = msg["result"].get<std::string>();
        post_to(conv, event);
        post_busy(conv);
        save();
    }
}

void ChatPanel::on_claude_exit(Conversation& conv, int exit_code, const std::string& stderr_tail)
{
    const bool was_busy = conv.busy;
    conv.busy           = false;
    // The last non-empty line of stderr usually tells why.
    std::string reason = stderr_tail;
    while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r'))
        reason.pop_back();
    if (const size_t nl = reason.find_last_of('\n'); nl != std::string::npos)
        reason = reason.substr(nl + 1);
    if (exit_code != 0 || was_busy)
        BOOST_LOG_TRIVIAL(warning) << "AI chat: claude exited with code " << exit_code << ": " << stderr_tail;
    post_to(conv, {{"kind", "exited"}, {"code", exit_code}, {"text", reason}});
    post_busy(conv);
    save();
}

void ChatPanel::post(const json& event)
{
    if (!m_page_ready || m_browser == nullptr) {
        m_pending.push_back(event);
        return;
    }
    // ASCII-only JSON is a valid JavaScript literal whatever the content.
    const std::string js = "window.chatApp && window.chatApp.receive(" +
                           event.dump(-1, ' ', true, json::error_handler_t::replace) + ");";
    WebView::RunScript(m_browser, from_u8(js));
}

void ChatPanel::post_to(Conversation& conv, const json& event, bool show)
{
    json& history = conv.history;
    // Streamed text is kept as one event per block.
    if (event["kind"] == "text_delta" && !history.empty() && history.back()["kind"] == "text_delta")
        history.back()["text"] = history.back()["text"].get<std::string>() + event["text"].get<std::string>();
    else
        history.push_back(event);
    if (show && &conv == m_active && m_page_ready)
        post(event);
}

void ChatPanel::post_status(const std::string& state, const wxString& text)
{
    post({{"kind", "status"}, {"state", state}, {"text", into_u8(text)}});
}

void ChatPanel::post_busy(Conversation& conv)
{
    post_tabs();
    if (&conv == m_active)
        post_status(conv.busy ? "busy" : "ready");
}

void ChatPanel::post_layout() { post({{"kind", "layout"}, {"docked", m_docked}}); }

void ChatPanel::post_server_state() { post({{"kind", "server"}, {"running", McpService::is_running()}}); }

void ChatPanel::post_tabs()
{
    if (!m_page_ready)
        return;
    json tabs = json::array();
    for (const auto& conv : m_conversations)
        tabs.push_back({{"id", conv->id}, {"title", conv->title}, {"busy", conv->busy}});
    post({{"kind", "tabs"}, {"tabs", tabs}, {"active", m_active ? m_active->id : std::string()}});
}

void ChatPanel::post_models()
{
    post({{"kind", "models"}, {"models", m_models.empty() ? fallback_models() : m_models}, {"current", m_model}});
}

void ChatPanel::post_replay()
{
    if (!m_page_ready || m_active == nullptr)
        return;
    post({{"kind", "replay"}, {"events", m_active->history}, {"busy", m_active->busy}});
}

void ChatPanel::load()
{
    json data;
    try {
        boost::nowide::ifstream in(chats_file().string(), std::ios::binary);
        if (in)
            data = json::parse(in);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "AI chat: could not read " << chats_file().string() << ": " << e.what();
    }
    if (data.is_object() && data.contains("conversations") && data["conversations"].is_array())
        for (const json& c : data["conversations"]) {
            Conversation& conv = add_conversation();
            conv.id            = c.value("id", conv.id);
            conv.title         = c.value("title", "");
            conv.session_id    = c.value("session_id", "");
            if (c.contains("history") && c["history"].is_array())
                conv.history = c["history"];
        }
    if (data.is_object() && data.contains("models") && data["models"].is_array())
        m_models = data["models"];
    if (m_conversations.empty())
        add_conversation();
    m_active = data.is_object() ? find(data.value("active", "")) : nullptr;
    if (m_active == nullptr)
        m_active = m_conversations.back().get();
}

void ChatPanel::save() const
{
    json conversations = json::array();
    for (const auto& conv : m_conversations)
        conversations.push_back(
            {{"id", conv->id}, {"title", conv->title}, {"session_id", conv->session_id}, {"history", conv->history}});
    const json data = {{"active", m_active ? m_active->id : std::string()},
                       {"conversations", conversations},
                       {"models", m_models.is_array() ? m_models : json::array()}};

    boost::system::error_code ec;
    fs::create_directories(assistant_dir(), ec);
    // Written next to the file then renamed: a crash never leaves it half written.
    const fs::path tmp = chats_file().string() + ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << data.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    fs::permissions(tmp, fs::owner_read | fs::owner_write, ec);
    fs::rename(tmp, chats_file(), ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "AI chat: could not save the conversations: " << ec.message();
}

}} // namespace Slic3r::AIAssistant
