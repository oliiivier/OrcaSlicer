#include "McpNotes.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <algorithm>
#include <ctime>
#include <iterator>
#include <sstream>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// Larger instructions would crowd out the rest of the assistant's context.
constexpr size_t MAX_INSTRUCTIONS = 32 * 1024;
constexpr size_t MAX_WRITE        = 200 * 1024;
constexpr size_t MAX_NOTE         = 1024 * 1024;
constexpr size_t MAX_NAME         = 200;

fs::path assistant_dir() { return fs::path(data_dir()) / "ai_assistant"; }

std::string read_file(const fs::path& path)
{
    fs::ifstream       in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes through a temporary file so that a failure never leaves half a note.
void write_file(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp";
    {
        fs::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out)
            throw ToolError("Could not write " + path.string());
    }
    fs::rename(tmp, path);
}

// Note name relative to the notes folder, '/' between sub-folders. Refuses
// anything that could leave the folder or name another kind of file.
fs::path note_path(const std::string& raw_name)
{
    std::string name = raw_name;
    if (!boost::algorithm::iends_with(name, ".md"))
        name += ".md";
    if (raw_name.empty() || name.size() > MAX_NAME)
        throw ToolError("'name' must be a note name of at most " + std::to_string(MAX_NAME) + " characters");

    std::vector<std::string> parts;
    boost::algorithm::split(parts, name, [](char c) { return c == '/'; });
    fs::path rel;
    for (const std::string& part : parts) {
        const bool bad_char = std::any_of(part.begin(), part.end(), [](unsigned char c) {
            return c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        });
        if (part.empty() || part.front() == '.' || bad_char)
            throw ToolError("Invalid note name '" + raw_name + "': use sub-folders separated by '/', without '..' or special characters");
        rel /= part;
    }
    return fs::path(notes_dir()) / rel;
}

std::string modified_iso(const fs::path& path)
{
    const std::time_t t = fs::last_write_time(path);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// First heading, or else first non-empty line, so that the list tells what each note is about.
std::string title_of(const fs::path& path)
{
    fs::ifstream in(path);
    std::string  line;
    for (int i = 0; i < 20 && std::getline(in, line); ++i) {
        const size_t start = line.find_first_not_of("# \t\r");
        if (start == std::string::npos)
            continue;
        std::string title = line.substr(start);
        if (!title.empty() && title.back() == '\r')
            title.pop_back();
        return title.size() > 100 ? title.substr(0, 100) + "..." : title;
    }
    return {};
}

} // namespace

std::string user_instructions_path() { return (assistant_dir() / "instructions.md").string(); }

std::string user_instructions()
{
    boost::system::error_code ec;
    const fs::path            path = user_instructions_path();
    if (!fs::is_regular_file(path, ec))
        return {};
    std::string text = read_file(path);
    if (text.size() > MAX_INSTRUCTIONS)
        text = text.substr(0, MAX_INSTRUCTIONS) + "\n[truncated]";
    return text;
}

void save_user_instructions(const std::string& text) { write_file(user_instructions_path(), text); }

std::string default_notes_dir() { return (assistant_dir() / "notes").string(); }

std::string notes_dir()
{
    const std::string dir = McpSettings::load().notes_dir;
    return dir.empty() ? default_notes_dir() : dir;
}

json list_notes(const json& /* args */)
{
    const fs::path            root = notes_dir();
    boost::system::error_code ec;
    json                      notes = json::array();
    if (fs::is_directory(root, ec)) {
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec)
                break;
            const fs::path& path = it->path();
            if (path.filename().string().front() == '.') {
                if (fs::is_directory(path, ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (!fs::is_regular_file(path, ec) || !boost::algorithm::iends_with(path.string(), ".md"))
                continue;
            notes.push_back({ { "name", path.lexically_relative(root).generic_string() },
                              { "title", title_of(path) },
                              { "size", fs::file_size(path, ec) },
                              { "modified", modified_iso(path) } });
        }
    }
    std::sort(notes.begin(), notes.end(), [](const json& a, const json& b) { return a["name"] < b["name"]; });
    return { { "folder", root.string() }, { "writable", McpSettings::load().notes_writable }, { "notes", std::move(notes) } };
}

json read_note(const json& args)
{
    const fs::path            path = note_path(args.value("name", std::string()));
    boost::system::error_code ec;
    if (!fs::is_regular_file(path, ec))
        throw ToolError("No note named '" + args.value("name", std::string()) + "' (see list_notes)");
    if (fs::file_size(path, ec) > MAX_NOTE)
        throw ToolError("This note is too large to be read (over 1 MB)");
    return { { "name", path.lexically_relative(notes_dir()).generic_string() },
             { "modified", modified_iso(path) },
             { "content", read_file(path) } };
}

json write_note(const json& args)
{
    if (!McpSettings::load().notes_writable)
        throw ToolError("The user did not allow the assistant to edit notes (AI assistant settings)");
    const fs::path path = note_path(args.value("name", std::string()));
    if (!args.contains("content") || !args["content"].is_string())
        throw ToolError("'content' must be the Markdown text of the note");
    const std::string content = args["content"].get<std::string>();
    if (content.size() > MAX_WRITE)
        throw ToolError("'content' is too long (at most 200 KB per call)");

    const std::string         mode = args.value("mode", std::string("replace"));
    boost::system::error_code ec;
    const bool                existed = fs::is_regular_file(path, ec);
    std::string               text;
    if (mode == "append") {
        text = existed ? read_file(path) : std::string();
        if (!text.empty() && text.back() != '\n')
            text += '\n';
        text += content;
    } else if (mode == "replace") {
        text = content;
    } else if (mode == "create") {
        if (existed)
            throw ToolError("A note with this name already exists: read it, then replace or append");
        text = content;
    } else
        throw ToolError("'mode' must be replace, append or create");
    if (text.size() > MAX_NOTE)
        throw ToolError("The note would exceed 1 MB: split it into several notes");

    write_file(path, text);
    return { { "name", path.lexically_relative(notes_dir()).generic_string() },
             { "created", !existed },
             { "size", text.size() } };
}

}} // namespace Slic3r::AIAssistant
