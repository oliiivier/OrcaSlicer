#include "ClaudeProcess.hpp"

#include <boost/log/trivial.hpp>

#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/process.h>
#include <wx/stream.h>
#include <wx/utils.h>

namespace Slic3r { namespace AIAssistant {

namespace {

constexpr int    POLL_MS          = 30;
constexpr size_t STDERR_TAIL_SIZE = 4096;
// A stdin write blocked longer than this means the CLI stopped reading.
constexpr long WRITE_TIMEOUT_MS = 5000;

void read_stream(wxInputStream* in, std::string& out)
{
    char buf[8192];
    while (in != nullptr && in->CanRead()) {
        in->Read(buf, sizeof(buf));
        const size_t n = in->LastRead();
        if (n == 0)
            break;
        out.append(buf, n);
    }
}

} // namespace

// Reports its termination to the owner, which may have let go of it (stop()).
class ClaudeProcess::Process : public wxProcess
{
public:
    explicit Process(ClaudeProcess* owner) : m_owner(owner) { Redirect(); }

    void OnTerminate(int /* pid */, int status) override
    {
        if (m_owner != nullptr)
            m_owner->on_terminate(status);
        delete this;
    }

    ClaudeProcess* m_owner;
};

ClaudeProcess::ClaudeProcess(MessageHandler on_message, ExitHandler on_exit)
    : m_on_message(std::move(on_message)), m_on_exit(std::move(on_exit)), m_timer(this)
{
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); });
}

ClaudeProcess::~ClaudeProcess() { stop(); }

bool ClaudeProcess::start(const Options& options, std::string& error)
{
    stop();

    std::vector<std::wstring> args = {
        wxString::FromUTF8(options.executable).ToStdWstring(),
        L"-p",
        L"--input-format", L"stream-json",
        L"--output-format", L"stream-json",
        L"--verbose",
        L"--include-partial-messages",
        // No built-in tools (shell, files, web): only the slicer's MCP tools.
        L"--tools=",
        L"--strict-mcp-config",
        L"--permission-mode", L"dontAsk",
    };
    if (!options.mcp_config.empty()) {
        args.push_back(L"--mcp-config");
        args.push_back(wxString::FromUTF8(options.mcp_config).ToStdWstring());
    }
    if (!options.allowed_tools.empty()) {
        args.push_back(L"--allowedTools");
        args.push_back(wxString::FromUTF8(options.allowed_tools).ToStdWstring());
    }
    if (!options.system_prompt.empty()) {
        args.push_back(L"--append-system-prompt");
        args.push_back(wxString::FromUTF8(options.system_prompt).ToStdWstring());
    }
    if (!options.resume_session.empty()) {
        args.push_back(L"--resume");
        args.push_back(wxString::FromUTF8(options.resume_session).ToStdWstring());
    }

    std::vector<const wchar_t*> argv;
    for (const std::wstring& a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    wxExecuteEnv env; // empty env: the child inherits ours
    env.cwd = wxString::FromUTF8(options.working_dir);

    auto* process = new Process(this);
    m_pid         = ::wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE | wxEXEC_MAKE_GROUP_LEADER, process, &env);
    if (m_pid <= 0) {
        // wxExecute deletes the process object itself when the launch fails.
        m_pid = 0;
        error = "could not run " + options.executable;
        return false;
    }
    m_process = process;
    m_stdout.clear();
    m_stderr.clear();
    m_timer.Start(POLL_MS);
    BOOST_LOG_TRIVIAL(info) << "AI chat: started " << options.executable << " (pid " << m_pid << ")";
    return true;
}

void ClaudeProcess::stop()
{
    m_timer.Stop();
    if (m_process == nullptr)
        return;
    // The process object deletes itself once the child is gone.
    m_process->m_owner = nullptr;
    m_process          = nullptr;
    wxProcess::Kill(m_pid, wxSIGTERM, wxKILL_CHILDREN);
    m_pid = 0;
}

bool ClaudeProcess::send(const nlohmann::json& msg)
{
    wxOutputStream* out = m_process ? m_process->GetOutputStream() : nullptr;
    if (out == nullptr)
        return false;

    const std::string line  = msg.dump() + "\n";
    size_t            done  = 0;
    const wxLongLong  start = wxGetLocalTimeMillis();
    // The pipe may accept only part of a large message (pasted image) at a time.
    while (done < line.size()) {
        out->Write(line.data() + done, line.size() - done);
        const size_t n = out->LastWrite();
        done += n;
        if (n == 0) {
            if (out->GetLastError() != wxSTREAM_NO_ERROR && out->GetLastError() != wxSTREAM_EOF)
                return false;
            if (wxGetLocalTimeMillis() - start > WRITE_TIMEOUT_MS)
                return false;
            wxMilliSleep(1);
        }
    }
    return true;
}

bool ClaudeProcess::interrupt()
{
    return send({{"type", "control_request"},
                 {"request_id", "interrupt_" + std::to_string(++m_request_id)},
                 {"request", {{"subtype", "interrupt"}}}});
}

void ClaudeProcess::read_available()
{
    if (m_process == nullptr)
        return;
    read_stream(m_process->GetInputStream(), m_stdout);
    read_stream(m_process->GetErrorStream(), m_stderr);
    if (m_stderr.size() > STDERR_TAIL_SIZE)
        m_stderr.erase(0, m_stderr.size() - STDERR_TAIL_SIZE);
}

void ClaudeProcess::poll()
{
    read_available();

    size_t begin = 0;
    for (size_t end = m_stdout.find('\n'); end != std::string::npos; end = m_stdout.find('\n', begin)) {
        const std::string line = m_stdout.substr(begin, end - begin);
        begin                  = end + 1;
        if (line.find_first_not_of(" \r\t") == std::string::npos)
            continue;
        nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
        if (msg.is_discarded()) {
            BOOST_LOG_TRIVIAL(warning) << "AI chat: not a JSON line from claude: " << line.substr(0, 200);
            continue;
        }
        // The handler may stop or restart the process.
        const long pid = m_pid;
        m_on_message(msg);
        if (m_pid != pid)
            return;
    }
    m_stdout.erase(0, begin);
}

void ClaudeProcess::on_terminate(int exit_code)
{
    // Messages written just before exiting are still in the pipes.
    const long pid = m_pid;
    poll();
    if (m_pid != pid)
        return; // a handler replaced the process
    m_timer.Stop();
    m_process = nullptr;
    m_pid     = 0;
    BOOST_LOG_TRIVIAL(info) << "AI chat: claude exited with code " << exit_code;
    m_on_exit(exit_code, m_stderr);
}

std::string ClaudeProcess::find_executable()
{
#ifdef _WIN32
    const wxString name = "claude.exe";
#else
    const wxString name = "claude";
#endif
    wxPathList path;
    path.AddEnvList("PATH");
    wxString found = path.FindAbsoluteValidPath(name);
    if (!found.empty())
        return found.ToUTF8().data();

    const wxString home = wxGetHomeDir();
    const std::vector<wxString> dirs = {
        home + "/.local/bin",
        home + "/.claude/local",
#ifdef __APPLE__
        "/opt/homebrew/bin",
        "/usr/local/bin",
#endif
    };
    for (const wxString& dir : dirs) {
        wxFileName file(dir, name);
        if (file.IsFileExecutable())
            return file.GetFullPath().ToUTF8().data();
    }
    return {};
}

}} // namespace Slic3r::AIAssistant
