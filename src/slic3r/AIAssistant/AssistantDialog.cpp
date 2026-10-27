#include "AssistantDialog.hpp"
#include "AssistantSettings.hpp"
#include "LlmProvider.hpp"

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <wx/sizer.h>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::wxGetApp;

AssistantDialog::AssistantDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("AI Assistant"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
{
    const wxString url = from_u8(
        (boost::filesystem::path(resources_dir()) / "web/ai_assistant/index.html").make_preferred().string());

    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "AI assistant: could not create web view";
        return;
    }

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &AssistantDialog::on_script_message, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &AssistantDialog::on_error, this, m_browser->GetId());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);
    SetSize(FromDIP(wxSize(480, 720)));
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

void AssistantDialog::on_script_message(wxWebViewEvent& evt)
{
    const std::string payload = evt.GetString().ToUTF8().data();
    try {
        handle_command(json::parse(payload));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "AI assistant: invalid message from web view: " << e.what();
    }
}

void AssistantDialog::on_error(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(error) << "AI assistant: web view error " << evt.GetInt() << " on " << evt.GetURL().ToUTF8().data();
}

void AssistantDialog::handle_command(const json& msg)
{
    const std::string cmd = msg.value("cmd", "");

    if (cmd == "ready") {
        send_settings();
    } else if (cmd == "save_settings") {
        AssistantSettings s = AssistantSettings::load();
        s.provider          = msg.value("provider", s.provider);
        s.endpoint          = msg.value("endpoint", s.endpoint);
        s.model             = msg.value("model", s.model);
        // An empty key from the UI means "keep the stored one": the key is never sent back to JS.
        const std::string api_key = msg.value("api_key", "");
        if (!api_key.empty())
            s.api_key = api_key;
        s.save();
        send_settings();
    } else if (cmd == "chat") {
        auto provider = create_provider(AssistantSettings::load());
        if (!provider) {
            post_to_web({{"cmd", "chat_error"}, {"message", _u8L("No LLM provider is available yet.")}});
            return;
        }
        // Wiring to the provider, project context and config changes comes in the next step.
    } else {
        BOOST_LOG_TRIVIAL(warning) << "AI assistant: unknown command '" << cmd << "'";
    }
}

void AssistantDialog::send_settings()
{
    const AssistantSettings s = AssistantSettings::load();
    post_to_web({{"cmd", "settings"},
                 {"provider", s.provider},
                 {"endpoint", s.endpoint},
                 {"model", s.model},
                 {"has_api_key", !s.api_key.empty()}});
}

void AssistantDialog::post_to_web(const json& msg)
{
    if (m_browser == nullptr)
        return;
    // ensure_ascii keeps the payload a valid JS literal whatever the content.
    const std::string script = "window.aiAssistant && window.aiAssistant.onMessage(" + msg.dump(-1, ' ', true) + ");";
    WebView::RunScript(m_browser, from_u8(script));
}

}} // namespace Slic3r::AIAssistant
