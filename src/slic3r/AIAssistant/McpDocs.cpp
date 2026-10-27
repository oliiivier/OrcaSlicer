#include "McpDocs.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;

namespace {

// Keeps tool results readable; read_documentation can return the rest.
constexpr size_t SETTING_DOC_MAX_CHARS = 4000;
constexpr size_t PAGE_MAX_CHARS        = 30000;

const json& docs()
{
    static const json data = []() {
        const boost::filesystem::path path = boost::filesystem::path(resources_dir()) / "ai_assistant" / "setting_docs.json";
        try {
            boost::filesystem::ifstream in(path);
            return json::parse(in);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "MCP: cannot read " << path.string() << ": " << e.what();
            return json { { "pages", json::object() }, { "settings", json::object() } };
        }
    }();
    return data;
}

const json& find_page(const std::string& name)
{
    const json& pages = docs()["pages"];
    const auto  it    = pages.find(name);
    if (it == pages.end())
        throw ToolError("No documentation page '" + name + "', call read_documentation without arguments to list them");
    return *it;
}

// Markdown of the section with the given anchor and its subsections; the whole
// page when the anchor is empty.
std::string section_text(const json& page, const std::string& anchor)
{
    std::string text;
    int         level = -1; // level of the requested section once found
    for (const json& s : page["sections"]) {
        const int s_level = s["level"].get<int>();
        if (level < 0) {
            if (!anchor.empty() && s["anchor"] != anchor)
                continue;
            level = anchor.empty() ? 0 : s_level;
        } else if (!anchor.empty() && s_level <= level) {
            break;
        }
        if (s_level > 0)
            text += std::string(size_t(s_level), '#') + " " + s["title"].get<std::string>() + "\n\n";
        text += s["text"].get<std::string>() + "\n\n";
    }
    if (level < 0)
        throw ToolError("No section '" + anchor + "' in this page");
    return text;
}

std::string truncate(std::string text, size_t max_chars, bool& truncated)
{
    truncated = text.size() > max_chars;
    if (truncated) {
        text.resize(max_chars);
        // Do not cut a UTF-8 sequence in the middle.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
            text.pop_back();
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80))
            text.pop_back();
    }
    return text;
}

} // namespace

json setting_documentation(const std::string& key)
{
    const json& settings = docs()["settings"];
    const auto  it       = settings.find(key);
    if (it == settings.end())
        return nullptr;

    const std::string target = it->get<std::string>();
    const size_t      hash   = target.find('#');
    const std::string page   = target.substr(0, hash);
    const std::string anchor = hash == std::string::npos ? std::string() : target.substr(hash + 1);

    bool truncated = false;
    json out       = { { "page", page },
                       { "text", truncate(section_text(find_page(page), anchor), SETTING_DOC_MAX_CHARS, truncated) } };
    if (!anchor.empty())
        out["section"] = anchor;
    if (truncated)
        out["truncated"] = "use read_documentation for the full text";
    return out;
}

std::string no_documentation_note(bool has_tooltip)
{
    return has_tooltip ? "none: no documentation page, the 'tooltip' field is the only description" :
                         "none: no documentation page and no tooltip, only the label, type, limits and allowed values";
}

json read_documentation(const json& args)
{
    const std::string page_name = args.value("page", std::string());
    if (page_name.empty()) {
        json pages = json::array();
        for (const auto& [name, page] : docs()["pages"].items())
            pages.push_back({ { "page", name }, { "title", page["title"] }, { "path", page["path"] } });
        return { { "source", "Snapmaker Orca / OrcaSlicer user documentation, bundled with this version" },
                 { "pages", std::move(pages) } };
    }

    const json&       page   = find_page(page_name);
    const std::string anchor = args.value("section", std::string());
    bool              truncated = false;
    json out = { { "page", page_name }, { "title", page["title"] }, { "text", truncate(section_text(page, anchor), PAGE_MAX_CHARS, truncated) } };
    if (!anchor.empty())
        out["section"] = anchor;
    if (truncated) {
        json sections = json::array();
        for (const json& s : page["sections"])
            if (s["level"].get<int>() > 0)
                sections.push_back(s["anchor"]);
        out["truncated"]         = "read one section at a time";
        out["available_sections"] = std::move(sections);
    }
    return out;
}

}} // namespace Slic3r::AIAssistant
