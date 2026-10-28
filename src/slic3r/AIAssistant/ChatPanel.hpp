#pragma once

#include "ClaudeProcess.hpp"

#include <nlohmann/json.hpp>

#include <wx/panel.h>

#include <memory>
#include <string>
#include <vector>

class wxWebView;
class wxWebViewEvent;

namespace Slic3r { namespace AIAssistant {

class ChatWindow;

// Chat with Claude inside the slicer: a web page (resources/web/ai_assistant/chat.html)
// in front of the Claude Code CLI, which reaches the slicer through the MCP server.
// One instance for the application. It lives either in its own floating window or
// docked on the right of the 3D view, and moves between them without losing the
// conversations. Each tab is a conversation with its own Claude Code session; the
// tabs and what they show are saved in the data directory (ai_assistant/chats.json).
class ChatPanel : public wxPanel
{
public:
    static void show();
    // Shows the chat, or hides it when it is shown.
    static void toggle();

    ~ChatPanel() override;

private:
    struct Conversation;

    explicit ChatPanel(wxWindow* parent);

    // Moves the panel into the main window, or out of it into its own window.
    void set_docked(bool docked);
    bool is_shown_to_user() const;
    void show_to_user(bool show);

    void on_script_message(wxWebViewEvent& evt);
    void on_claude_message(Conversation& conv, const nlohmann::json& msg);
    void on_claude_exit(Conversation& conv, int exit_code, const std::string& stderr_tail);

    Conversation* find(const std::string& id);
    Conversation& add_conversation();
    void          select(Conversation& conv);
    void          close(Conversation& conv);

    // Starts the CLI of the conversation if needed, continuing its session.
    bool ensure_started(Conversation& conv);
    // Stops the idle CLIs started before the MCP server was turned on or off, or moved:
    // they restart with the new server on their next message.
    void sync_server();
    void send_user_message(Conversation& conv, const nlohmann::json& request);
    // Attaches a screenshot of the 3D view or of the whole main window to the next message.
    void capture(bool whole_window);
    void begin_resize(const std::string& edge);

    // Posts an event to the page, or queues it until the page is loaded.
    void post(const nlohmann::json& event);
    // Records an event in the history of the conversation, and shows it if the conversation
    // is the one on screen.
    void post_to(Conversation& conv, const nlohmann::json& event, bool show = true);
    void post_status(const std::string& state, const wxString& text = {});
    void post_busy(Conversation& conv);
    void post_server_state();
    void post_layout();
    void post_tabs();
    // Redraws the conversation on screen from its history.
    void post_replay();

    void load();
    void save() const;

    wxWebView*    m_browser { nullptr };
    ChatWindow*   m_window { nullptr };
    bool          m_docked { false };
    bool          m_page_ready { false };
    std::vector<nlohmann::json> m_pending;
    std::vector<std::unique_ptr<Conversation>> m_conversations;
    Conversation* m_active { nullptr };
};

}} // namespace Slic3r::AIAssistant
