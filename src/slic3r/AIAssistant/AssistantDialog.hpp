#pragma once

#include <wx/dialog.h>
#include <wx/webview.h>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Modeless window hosting the assistant web UI (resources/web/ai_assistant/).
//
// JS -> C++: window.wx.postMessage(JSON.stringify({ cmd: "...", ... }))
// C++ -> JS: window.aiAssistant.onMessage({ cmd: "...", ... })
class AssistantDialog : public wxDialog
{
public:
    explicit AssistantDialog(wxWindow* parent);
    ~AssistantDialog() override = default;

private:
    void on_script_message(wxWebViewEvent& evt);
    void on_error(wxWebViewEvent& evt);

    void handle_command(const nlohmann::json& msg);
    void post_to_web(const nlohmann::json& msg);
    void send_settings();

    wxWebView* m_browser { nullptr };
};

}} // namespace Slic3r::AIAssistant
