#pragma once

#include <string>

namespace Slic3r { namespace AIAssistant {

// Assistant preferences, persisted in the [ai_assistant] section of AppConfig
// (the user's config, never inside project files).
struct AssistantSettings
{
    // "anthropic" or "openai_compatible" (OpenAI, Ollama, LM Studio, OpenRouter...)
    std::string provider { "anthropic" };
    std::string endpoint;  // empty = provider default
    std::string model;     // empty = provider default
    std::string api_key;

    static AssistantSettings load();
    void                     save() const;
};

}} // namespace Slic3r::AIAssistant
