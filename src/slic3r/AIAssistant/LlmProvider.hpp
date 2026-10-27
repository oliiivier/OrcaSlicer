#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace AIAssistant {

struct AssistantSettings;

struct ChatMessage
{
    enum class Role { System, User, Assistant };
    Role        role;
    std::string content;
};

// A setting change proposed by the model. Never applied without user confirmation.
struct ConfigChange
{
    std::string key;       // PrintConfigDef option key, e.g. "layer_height"
    std::string value;     // serialized value, as accepted by ConfigBase::set_deserialize()
    std::string rationale; // why the model recommends it
};

struct LlmReply
{
    std::string               text;
    std::vector<ConfigChange> changes;
};

// Common interface for all LLM backends (Anthropic, OpenAI-compatible, ...).
// Implementations must invoke the callbacks exactly once, possibly from a worker thread.
class LlmProvider
{
public:
    using OnReply = std::function<void(LlmReply)>;
    using OnError = std::function<void(std::string /* message */)>;

    virtual ~LlmProvider() = default;

    virtual std::string name() const = 0;
    virtual void        send(const std::vector<ChatMessage>& messages, OnReply on_reply, OnError on_error) = 0;
};

// Returns nullptr when the configured provider is unknown or not implemented yet.
std::unique_ptr<LlmProvider> create_provider(const AssistantSettings& settings);

}} // namespace Slic3r::AIAssistant
