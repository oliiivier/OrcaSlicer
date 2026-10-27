#include "AssistantSettings.hpp"
#include "LlmProvider.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace AIAssistant {

static const char* SECTION = "ai_assistant";

AssistantSettings AssistantSettings::load()
{
    AssistantSettings s;
    const AppConfig*  cfg = GUI::wxGetApp().app_config;
    if (cfg == nullptr)
        return s;

    std::string value;
    if (cfg->get(SECTION, "provider", value) && !value.empty())
        s.provider = value;
    cfg->get(SECTION, "endpoint", s.endpoint);
    cfg->get(SECTION, "model", s.model);
    cfg->get(SECTION, "api_key", s.api_key);
    return s;
}

void AssistantSettings::save() const
{
    AppConfig* cfg = GUI::wxGetApp().app_config;
    if (cfg == nullptr)
        return;

    cfg->set(SECTION, "provider", provider);
    cfg->set(SECTION, "endpoint", endpoint);
    cfg->set(SECTION, "model", model);
    cfg->set(SECTION, "api_key", api_key);
}

std::unique_ptr<LlmProvider> create_provider(const AssistantSettings& /* settings */)
{
    // Providers are added in the next step (AnthropicProvider, OpenAICompatibleProvider).
    return nullptr;
}

}} // namespace Slic3r::AIAssistant
