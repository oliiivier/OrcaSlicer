#include "McpManufacturerDocs.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <cctype>
#include <map>
#include <memory>
#include <mutex>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;

namespace {

const std::string WIKI    = "https://wiki.snapmaker.com";
const std::string SUPPORT = "https://support.snapmaker.com";

// A page longer than this is cut: the assistant reads it in parts with 'offset'.
constexpr size_t MAX_TEXT = 30000;

// ------------------------------------------------------------ requests

struct Pending
{
    std::mutex  mutex;
    bool        done { false };
    unsigned    status { 0 };
    std::string body;
    std::string error;
};

struct Response
{
    unsigned    status { 0 };
    std::string body;
    std::string error;
};

// Requests of the calls in progress, by id. GUI thread only; the callbacks
// only touch their own Pending, under its mutex.
std::map<int, std::shared_ptr<Pending>>& registry()
{
    static std::map<int, std::shared_ptr<Pending>> requests;
    return requests;
}

void start(Http http, json& ids)
{
    static int next_id = 0;
    auto       p       = std::make_shared<Pending>();
    registry()[++next_id] = p;
    ids.push_back(next_id);
    http.timeout_connect(5)
        .timeout_max(20)
        .size_limit(8 * 1024 * 1024)
        .header("Accept-Language", "en")
        .on_complete([p](std::string body, unsigned status) {
            std::lock_guard<std::mutex> lock(p->mutex);
            p->status = status;
            p->body   = std::move(body);
            p->done   = true;
        })
        .on_error([p](std::string body, std::string error, unsigned status) {
            std::lock_guard<std::mutex> lock(p->mutex);
            p->status = status;
            p->body   = std::move(body);
            p->error  = error.empty() ? "HTTP " + std::to_string(status) : error;
            p->done   = true;
        })
        .perform();
}

// Responses of the requests started in `state`, once they all ended.
std::vector<Response> collect(json& state)
{
    std::vector<Response> out;
    for (const json& id : state["requests"]) {
        const auto it = registry().find(id.get<int>());
        if (it == registry().end())
            throw ToolError("The request was lost, try again");
        std::lock_guard<std::mutex> lock(it->second->mutex);
        if (!it->second->done)
            throw ToolRetry();
        out.push_back({ it->second->status, it->second->body, it->second->error });
    }
    manufacturer_docs_abandoned(state);
    return out;
}

void require_allowed()
{
    if (!McpSettings::load().manufacturer_docs)
        throw ToolError("The user did not allow searching the Snapmaker documentation (AI assistant settings)");
}

// ------------------------------------------------------------ HTML to text

std::string decode_entities(const std::string& s)
{
    static const std::map<std::string, std::string> named = { { "amp", "&" },   { "lt", "<" },     { "gt", ">" },
                                                              { "quot", "\"" }, { "apos", "'" },   { "nbsp", " " },
                                                              { "#39", "'" },   { "mdash", "—" }, { "ndash", "–" } };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const size_t end = s[i] == '&' ? s.find(';', i) : std::string::npos;
        if (end == std::string::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        const std::string name = s.substr(i + 1, end - i - 1);
        if (const auto it = named.find(name); it != named.end()) {
            out += it->second;
        } else if (name.size() > 1 && name[0] == '#') {
            unsigned long cp = 0;
            try {
                cp = name[1] == 'x' ? std::stoul(name.substr(2), nullptr, 16) : std::stoul(name.substr(1));
            } catch (...) {
                out += s[i];
                continue;
            }
            // UTF-8 encoding of the code point.
            if (cp < 0x80)
                out += char(cp);
            else if (cp < 0x800)
                out += { char(0xC0 | (cp >> 6)), char(0x80 | (cp & 0x3F)) };
            else if (cp < 0x10000)
                out += { char(0xE0 | (cp >> 12)), char(0x80 | ((cp >> 6) & 0x3F)), char(0x80 | (cp & 0x3F)) };
            else
                out += { char(0xF0 | (cp >> 18)), char(0x80 | ((cp >> 12) & 0x3F)), char(0x80 | ((cp >> 6) & 0x3F)),
                         char(0x80 | (cp & 0x3F)) };
        } else {
            out += s[i];
            continue;
        }
        i = end;
    }
    return out;
}

std::string attribute(const std::string& tag, const char* name)
{
    const std::string key = std::string(" ") + name + "=\"";
    const size_t      pos = tag.find(key);
    if (pos == std::string::npos)
        return {};
    const size_t start = pos + key.size();
    const size_t end   = tag.find('"', start);
    return end == std::string::npos ? std::string() : decode_entities(tag.substr(start, end - start));
}

// Markdown-like text of an HTML fragment: headings, lists, paragraphs, links to
// other pages and image captions are kept, the layout is dropped.
std::string html_to_text(const std::string& html, const std::string& site)
{
    std::string out;
    std::string link; // href of the link being read, appended after its text
    bool        skip = false; // inside <script> or <style>
    for (size_t i = 0; i < html.size();) {
        if (html[i] != '<') {
            const size_t next = html.find('<', i);
            std::string  text = html.substr(i, next == std::string::npos ? std::string::npos : next - i);
            i                 = next == std::string::npos ? html.size() : next;
            if (skip)
                continue;
            for (char& c : text)
                if (c == '\n' || c == '\t' || c == '\r')
                    c = ' ';
            out += decode_entities(text);
            continue;
        }
        const size_t end = html.find('>', i);
        if (end == std::string::npos)
            break;
        const std::string tag = html.substr(i, end - i + 1);
        i                     = end + 1;

        const bool  closing = tag.size() > 1 && tag[1] == '/';
        size_t      n       = closing ? 2 : 1;
        std::string name;
        while (n < tag.size() && std::isalnum(static_cast<unsigned char>(tag[n])))
            name += char(std::tolower(static_cast<unsigned char>(tag[n++])));

        if (name == "script" || name == "style") {
            skip = !closing;
            continue;
        }
        if (skip)
            continue;
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            out += closing ? "\n\n" : "\n\n" + std::string(name[1] - '0', '#') + " ";
        } else if (name == "li" && !closing) {
            out += "\n- ";
        } else if (name == "p" || name == "div" || name == "br" || name == "tr" || name == "figure" || name == "blockquote" ||
                   name == "ul" || name == "ol" || name == "table") {
            out += "\n";
        } else if (name == "td" || name == "th") {
            out += closing ? "" : " | ";
        } else if (name == "img" && !closing) {
            // Without a caption the address tells the assistant that the page shows a picture there.
            const std::string alt = attribute(tag, "alt");
            const std::string src = attribute(tag, "src");
            if (!alt.empty())
                out += "[image: " + alt + "]";
            else if (boost::algorithm::starts_with(src, "http"))
                out += "[image: " + src + "]";
        } else if (name == "iframe" && !closing) {
            // Embedded videos: some articles are only a video.
            std::string src = attribute(tag, "src");
            if (boost::algorithm::starts_with(src, "//"))
                src = "https:" + src;
            if (!src.empty())
                out += "\n[video: " + src + "]\n";
        } else if (name == "a") {
            if (!closing) {
                const std::string href = attribute(tag, "href");
                // Section anchors and enlarged images are not worth a link.
                link = attribute(tag, "class").find("anchor") != std::string::npos ||
                               attribute(tag, "class").find("asset") != std::string::npos || href.empty() || href[0] == '#' ?
                           std::string() :
                       href[0] == '/' ? site + href :
                                        href;
                if (attribute(tag, "class").find("toc-anchor") != std::string::npos) {
                    // Skips the '¶' of the heading anchors.
                    const size_t close = html.find("</a>", i);
                    if (close != std::string::npos)
                        i = close + 4;
                }
            } else if (!link.empty()) {
                if (boost::algorithm::starts_with(link, "http"))
                    out += " (" + link + ")";
                link.clear();
            }
        }
    }

    // Collapses the spaces and blank lines left by the layout.
    std::string clean;
    int         newlines = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c == '\n') {
            while (!clean.empty() && clean.back() == ' ')
                clean.pop_back();
            if (++newlines <= 2)
                clean += c;
        } else if (c == ' ' && (clean.empty() || clean.back() == ' ' || clean.back() == '\n')) {
            continue;
        } else {
            newlines = 0;
            clean += c;
        }
    }
    const size_t first = clean.find_first_not_of('\n');
    return first == std::string::npos ? std::string() : clean.substr(first);
}

json page_text(const std::string& text, const json& args)
{
    const int    requested = args.value("offset", 0);
    const size_t offset    = requested > 0 ? size_t(requested) : 0;
    json         out    = { { "content", offset < text.size() ? text.substr(offset, MAX_TEXT) : std::string() } };
    if (offset + MAX_TEXT < text.size())
        out["continues"] = { { "next_offset", offset + MAX_TEXT }, { "total", text.size() } };
    return out;
}

// ------------------------------------------------------------ sites

Http wiki_search(const std::string& query)
{
    const json body = { { "query",
                          "query($q: String!) { pages { search(query: $q, locale: \"en\") { results { title path "
                          "description } totalHits } } }" },
                        { "variables", { { "q", query } } } };
    Http http = Http::post(WIKI + "/graphql");
    http.header("Content-Type", "application/json").set_post_body(body.dump());
    return http;
}

Http support_search(const std::string& query)
{
    return Http::get(SUPPORT + "/api/v2/help_center/articles/search.json?per_page=10&locale=en-us&query=" + Http::url_encode(query));
}

json wiki_results(const Response& r)
{
    if (!r.error.empty())
        return { { "error", r.error } };
    const json  d       = json::parse(r.body, nullptr, false);
    const json* results = nullptr;
    if (d.is_object() && d.contains("data"))
        results = &d["data"]["pages"]["search"]["results"];
    if (results == nullptr || !results->is_array())
        return { { "error", "unexpected answer from the wiki" } };
    json out = json::array();
    for (const json& p : *results) {
        json entry = { { "title", p.value("title", std::string()) }, { "url", WIKI + "/en/" + p.value("path", std::string()) } };
        if (const std::string desc = p.value("description", std::string()); !desc.empty())
            entry["description"] = desc;
        out.push_back(std::move(entry));
    }
    return out;
}

json support_results(const Response& r)
{
    if (!r.error.empty())
        return { { "error", r.error } };
    const json d = json::parse(r.body, nullptr, false);
    if (!d.is_object() || !d.contains("results") || !d["results"].is_array())
        return { { "error", "unexpected answer from the support site" } };
    json out = json::array();
    for (const json& a : d["results"])
        out.push_back({ { "title", a.value("title", std::string()) },
                        { "url", a.value("html_url", std::string()) },
                        { "updated", a.value("updated_at", std::string()).substr(0, 10) },
                        { "snippet", html_to_text(a.value("snippet", std::string()), SUPPORT) } });
    return out;
}

// Article id of a support URL such as .../hc/en-us/articles/37191276082199-U1-Initial-Setup-Printing.
std::string support_article_id(const std::string& url)
{
    const size_t pos = url.find("/articles/");
    if (pos == std::string::npos)
        return {};
    size_t end = pos + 10;
    while (end < url.size() && std::isdigit(static_cast<unsigned char>(url[end])))
        ++end;
    return url.substr(pos + 10, end - pos - 10);
}

} // namespace

void manufacturer_docs_abandoned(json& state)
{
    if (state.contains("requests"))
        for (const json& id : state["requests"])
            registry().erase(id.get<int>());
    state.erase("requests");
}

json search_manufacturer_docs(const json& args, json& state)
{
    require_allowed();
    const std::string query = args.value("query", std::string());
    if (query.empty())
        throw ToolError("'query' must give the words to search, in English (e.g. U1 stringing PETG)");

    if (!state.contains("requests")) {
        json ids = json::array();
        start(wiki_search(query), ids);
        start(support_search(query), ids);
        state["requests"] = std::move(ids);
        throw ToolRetry();
    }
    const std::vector<Response> r = collect(state);
    return { { "query", query },
             { "wiki", wiki_results(r[0]) },
             { "support", support_results(r[1]) },
             { "note", "Read a page with read_manufacturer_page. Cite its URL and update date to the user." } };
}

json read_manufacturer_page(const json& args, json& state)
{
    require_allowed();
    std::string url = args.value("url", std::string());
    if (boost::algorithm::starts_with(url, "http://"))
        url = "https://" + url.substr(7);
    const bool wiki    = boost::algorithm::starts_with(url, WIKI + "/");
    const bool support = boost::algorithm::starts_with(url, SUPPORT + "/");
    if (!wiki && !support)
        throw ToolError("Only pages of " + WIKI + " and " + SUPPORT + " can be read");
    const std::string article = support ? support_article_id(url) : std::string();
    if (support && article.empty())
        throw ToolError("Give the URL of a support article (.../hc/en-us/articles/<id>-...)");

    if (!state.contains("requests")) {
        json ids = json::array();
        start(Http::get(support ? SUPPORT + "/api/v2/help_center/en-us/articles/" + article + ".json" : url), ids);
        state["requests"] = std::move(ids);
        throw ToolRetry();
    }
    const Response r = collect(state).front();
    if (!r.error.empty())
        throw ToolError("Could not read " + url + ": " + r.error);

    if (support) {
        const json d = json::parse(r.body, nullptr, false);
        if (!d.is_object() || !d.contains("article"))
            throw ToolError("Unexpected answer from the support site");
        const json& a   = d["article"];
        json        out = { { "title", a.value("title", std::string()) },
                            { "url", a.value("html_url", url) },
                            { "updated", a.value("updated_at", std::string()).substr(0, 10) } };
        out.update(page_text(html_to_text(a.value("body", std::string()), SUPPORT), args));
        return out;
    }

    // Wiki.js renders the page content and its metadata in the HTML.
    const size_t page_tag = r.body.find("<page ");
    const std::string meta = page_tag == std::string::npos ? std::string() : r.body.substr(page_tag, r.body.find('>', page_tag) - page_tag);
    const std::string open = "<template slot=\"contents\">";
    const size_t      from = r.body.find(open);
    const size_t      to   = from == std::string::npos ? std::string::npos : r.body.find("</template>", from);
    if (to == std::string::npos)
        throw ToolError("No page content at " + url + " (wrong address?)");
    json out = { { "title", attribute(meta, "title") }, { "url", url }, { "updated", attribute(meta, "updated-at").substr(0, 10) } };
    out.update(page_text(html_to_text(r.body.substr(from + open.size(), to - from - open.size()), WIKI), args));
    return out;
}

}} // namespace Slic3r::AIAssistant
