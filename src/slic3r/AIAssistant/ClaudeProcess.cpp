#include "ClaudeProcess.hpp"

#include <boost/log/trivial.hpp>

#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/process.h>
#include <wx/regex.h>
#include <wx/stream.h>
#include <wx/textfile.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>

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

#ifdef _WIN32
const wxString PATH_SEP = ";";
#else
const wxString PATH_SEP = ":";
#endif

wxString find_in(const wxString& path_list, const wxString& name)
{
    wxStringTokenizer dirs(path_list, PATH_SEP, wxTOKEN_STRTOK);
    while (dirs.HasMoreTokens()) {
        const wxFileName file(dirs.GetNextToken(), name);
        if (wxFileName::IsFileExecutable(file.GetFullPath()))
            return file.GetFullPath();
    }
    return {};
}

#ifdef _WIN32
// npm installs a claude.cmd shim that runs the package's script with node, or its native
// binary. A .cmd needs cmd.exe, which would interpret the arguments: run its target instead.
ClaudeProcess::Launch launch_from_shim(const wxString& shim)
{
    wxTextFile file;
    if (!file.Open(shim))
        return {};
    const wxString dir = wxFileName(shim).GetPath();
    // e.g. "%dp0%\node_modules\@anthropic-ai\claude-code\cli.js" %*
    wxRegEx target("%~?dp0%?\\\\([^\"%]+)", wxRE_ADVANCED);
    for (wxString line = file.GetLastLine();; line = file.GetPrevLine()) {
        if (target.Matches(line)) {
            const wxString path = dir + "\\" + target.GetMatch(line, 1);
            if (!wxFileExists(path))
                break;
            ClaudeProcess::Launch launch;
            if (path.Lower().EndsWith(".exe")) {
                launch.program = path.ToUTF8().data();
                return launch;
            }
            // The shim prefers a node.exe next to it, as here.
            wxString node = wxFileName(dir, "node.exe").GetFullPath();
            if (!wxFileExists(node)) {
                wxString path_list;
                wxGetEnv("PATH", &path_list);
                node = find_in(path_list, "node.exe");
            }
            if (node.empty())
                break;
            launch.program = node.ToUTF8().data();
            launch.args    = {path.ToUTF8().data()};
            return launch;
        }
        if (file.GetCurrentLine() == 0)
            break;
    }
    BOOST_LOG_TRIVIAL(warning) << "AI chat: could not read the npm shim " << shim.ToUTF8().data();
    return {};
}

ClaudeProcess::Launch launch_from(const wxString& path)
{
    if (path.Lower().EndsWith(".cmd"))
        return launch_from_shim(path);
    ClaudeProcess::Launch launch;
    launch.program = path.ToUTF8().data();
    return launch;
}
#else
// The PATH of the user's interactive login shell, which reads the files that set it up
// (.zprofile, .bashrc, config.fish, nvm...). Asked once; empty if the shell does not answer.
std::string login_shell_path()
{
    static bool        asked = false;
    static std::string path;
    if (asked)
        return path;
    asked = true;

    const char* shell = std::getenv("SHELL");
    const std::string command = std::string("'") + (shell && *shell ? shell : "/bin/sh") +
                                "' -l -i -c 'echo __ORCA_ENV__; /usr/bin/env' </dev/null 2>/dev/null";
    // A shell may hang on its startup files: it is waited for a few seconds only.
    auto result = std::make_shared<std::promise<std::string>>();
    std::future<std::string> output = result->get_future();
    std::thread([command, result]() {
        std::string out;
        if (FILE* pipe = ::popen(command.c_str(), "r")) {
            char   buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0)
                out.append(buf, n);
            ::pclose(pipe);
        }
        result->set_value(out);
    }).detach();
    if (output.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        BOOST_LOG_TRIVIAL(warning) << "AI chat: no answer from the login shell " << command;
        return path;
    }
    const std::string out    = output.get();
    const size_t      marker = out.find("__ORCA_ENV__\n");
    if (marker == std::string::npos)
        return path;
    for (size_t begin = marker; begin < out.size();) {
        size_t end = out.find('\n', begin);
        if (end == std::string::npos)
            end = out.size();
        if (out.compare(begin, 5, "PATH=") == 0) {
            path = out.substr(begin + 5, end - begin - 5);
            break;
        }
        begin = end + 1;
    }
    return path;
}
#endif

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

    if (!options.launch.valid()) {
        error = "no Claude Code executable";
        return false;
    }
    std::vector<std::wstring> args = {wxString::FromUTF8(options.launch.program).ToStdWstring()};
    for (const std::string& a : options.launch.args)
        args.push_back(wxString::FromUTF8(a).ToStdWstring());
    args.insert(args.end(), {
        L"-p",
        L"--input-format", L"stream-json",
        L"--output-format", L"stream-json",
        L"--verbose",
        L"--include-partial-messages",
        // No built-in tools (shell, files, web): only the slicer's MCP tools.
        L"--tools=",
        L"--strict-mcp-config",
        L"--permission-mode", L"dontAsk",
    });
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
    if (!options.launch.path_env.empty()) {
        wxGetEnvMap(&env.env);
        env.env["PATH"] = wxString::FromUTF8(options.launch.path_env);
    }

    auto* process = new Process(this);
    m_pid         = ::wxExecute(argv.data(), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE | wxEXEC_MAKE_GROUP_LEADER, process, &env);
    if (m_pid <= 0) {
        // wxExecute deletes the process object itself when the launch fails.
        m_pid = 0;
        error = "could not run " + options.launch.program;
        return false;
    }
    m_process = process;
    m_stdout.clear();
    m_stderr.clear();
    m_timer.Start(POLL_MS);
    BOOST_LOG_TRIVIAL(info) << "AI chat: started " << options.launch.program << " (pid " << m_pid << ")";
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

ClaudeProcess::Launch ClaudeProcess::locate(const std::string& configured_path)
{
    wxString path_list;
    wxGetEnv("PATH", &path_list);
    const wxString home = wxGetHomeDir();

#ifdef _WIN32
    if (!configured_path.empty())
        return wxFileExists(wxString::FromUTF8(configured_path)) ? launch_from(wxString::FromUTF8(configured_path)) : Launch();

    // Native installer, then npm.
    for (const wxString& name : {wxString("claude.exe"), wxString("claude.cmd")})
        if (const wxString found = find_in(path_list, name); !found.empty())
            if (Launch launch = launch_from(found); launch.valid())
                return launch;
    wxString appdata;
    wxGetEnv("APPDATA", &appdata);
    for (const wxString& candidate : {home + "\\.local\\bin\\claude.exe", appdata + "\\npm\\claude.cmd"})
        if (wxFileExists(candidate))
            if (Launch launch = launch_from(candidate); launch.valid())
                return launch;
    return {};
#else
    Launch launch;
    if (!configured_path.empty()) {
        if (!wxFileName::IsFileExecutable(wxString::FromUTF8(configured_path)))
            return {};
        launch.program = configured_path;
    } else if (const wxString found = find_in(path_list, "claude"); !found.empty()) {
        launch.program = found.ToUTF8().data();
        return launch; // node, if needed, is on the same PATH
    } else {
        std::vector<wxString> dirs = {home + "/.local/bin", home + "/.claude/local"};
#ifdef __APPLE__
        dirs.insert(dirs.end(), {"/opt/homebrew/bin", "/usr/local/bin"});
#endif
        for (const wxString& dir : dirs)
            if (const wxString found = find_in(dir, "claude"); !found.empty()) {
                launch.program = found.ToUTF8().data();
                break;
            }
    }

    // Started from the desktop, the slicer may not have the PATH set up by the shell
    // (nvm, npm prefix...): look there too, and give it to the CLI for its node.
    const std::string shell_path = login_shell_path();
    if (launch.program.empty() && !shell_path.empty())
        if (const wxString found = find_in(wxString::FromUTF8(shell_path), "claude"); !found.empty())
            launch.program = found.ToUTF8().data();
    if (!launch.program.empty())
        launch.path_env = shell_path;
    return launch;
#endif
}

}} // namespace Slic3r::AIAssistant
