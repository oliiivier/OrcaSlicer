#pragma once

#include <nlohmann/json.hpp>

#include <wx/event.h>
#include <wx/timer.h>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r { namespace AIAssistant {

// Runs the Claude Code CLI in stream-json mode ("claude -p --input-format stream-json
// --output-format stream-json"): one JSON message per line on stdin and stdout.
// Lives on the GUI thread; stdout is polled with a timer, the handlers are called
// from the event loop.
class ClaudeProcess : public wxEvtHandler
{
public:
    // How to run the CLI.
    struct Launch
    {
        std::string program;
        // Arguments before the CLI options: the script of an npm install run with node.
        std::vector<std::string> args;
        // PATH for the CLI, empty to inherit the slicer's. A CLI installed with npm needs
        // node on its PATH, which a slicer started from the desktop may not have.
        std::string path_env;

        bool valid() const { return !program.empty(); }
    };

    struct Options
    {
        Launch      launch;
        std::string working_dir;
        // Path of an MCP configuration file, empty for none.
        std::string mcp_config;
        // Tools Claude may use without asking; everything else is denied.
        std::string allowed_tools;
        std::string system_prompt;
        // Session to continue, empty for a new conversation.
        std::string resume_session;
    };

    using MessageHandler = std::function<void(const nlohmann::json& msg)>;
    // Exit code and the last lines written on stderr.
    using ExitHandler = std::function<void(int exit_code, const std::string& stderr_tail)>;

    ClaudeProcess(MessageHandler on_message, ExitHandler on_exit);
    ~ClaudeProcess() override;

    ClaudeProcess(const ClaudeProcess&) = delete;
    ClaudeProcess& operator=(const ClaudeProcess&) = delete;

    // Returns false and fills `error` when the process could not be launched.
    bool start(const Options& options, std::string& error);
    // Kills the process; the exit handler is not called.
    void stop();
    bool is_running() const { return m_process != nullptr; }

    // Writes one message on the process stdin.
    bool send(const nlohmann::json& msg);
    // Asks Claude to stop the current turn; the process keeps running.
    bool interrupt();

    // Finds the CLI: `configured_path` when given, else on the PATH, in the usual install
    // locations, then (macOS, Linux) on the PATH of the user's login shell. Invalid if not found.
    static Launch locate(const std::string& configured_path);

private:
    class Process;

    void poll();
    void read_available();
    void on_terminate(int exit_code);

    MessageHandler m_on_message;
    ExitHandler    m_on_exit;
    Process*       m_process { nullptr };
    long           m_pid { 0 };
    wxTimer        m_timer;
    std::string    m_stdout;
    std::string    m_stderr;
    unsigned       m_request_id { 0 };
};

}} // namespace Slic3r::AIAssistant
