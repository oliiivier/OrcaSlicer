#include "McpExperiences.hpp"
#include "McpIssues.hpp"
#include "McpMaterials.hpp"
#include "McpPrinter.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;
namespace fs = boost::filesystem;

namespace {

const char* RESULTS[] = { "success", "partial", "failure" };

// Setting changes applied since the last record, attached to the next one.
json& pending_changes()
{
    static json changes = json::array();
    return changes;
}

fs::path journal_dir() { return fs::path(data_dir()) / "ai_assistant" / "experiences"; }

std::string now_iso()
{
    const std::time_t t = std::time(nullptr);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

std::string new_id()
{
    const std::time_t t = std::time(nullptr);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::gmtime(&t));
    return std::string(buf) + "-" + McpSettings::generate_token().substr(0, 6);
}

fs::path record_path(const std::string& id)
{
    // Ids are generated here; refuse anything that could escape the journal directory.
    if (id.empty() || id.find_first_not_of("0123456789abcdef-") != std::string::npos)
        throw ToolError("invalid experience id '" + id + "'");
    return journal_dir() / (id + ".json");
}

json load_record(const fs::path& path)
{
    fs::ifstream in(path);
    return json::parse(in);
}

void save_record(const json& record)
{
    fs::create_directories(journal_dir());
    const fs::path path = record_path(record.at("id").get<std::string>());
    const fs::path tmp  = path.string() + ".tmp";
    {
        fs::ofstream out(tmp);
        out << record.dump(2);
        if (!out)
            throw std::runtime_error("cannot write " + tmp.string());
    }
    if (const std::error_code ec = rename_file(tmp.string(), path.string()))
        throw std::runtime_error("cannot write " + path.string() + ": " + ec.message());
}

std::vector<json> all_records()
{
    std::vector<json> records;
    if (!fs::exists(journal_dir()))
        return records;
    for (const fs::directory_entry& entry : fs::directory_iterator(journal_dir())) {
        if (entry.path().extension() != ".json")
            continue;
        try {
            records.push_back(load_record(entry.path()));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "MCP: skipping experience " << entry.path().string() << ": " << e.what();
        }
    }
    // Most recent first.
    std::sort(records.begin(), records.end(),
              [](const json& a, const json& b) { return a.value("created", "") > b.value("created", ""); });
    return records;
}

// Facts about the setup, captured by the server rather than written by the model.
// Object and file names are left out: they may be personal and say nothing general.
json capture_context()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    json                context = { { "printer_preset", bundle.printers.get_selected_preset_name() },
                                    { "process_preset", bundle.prints.get_selected_preset_name() },
                                    { "materials", describe_materials() } };
    try {
        json issues = json::array();
        for (const json& issue : collect_issues()["issues"]) {
            json summary = { { "severity", issue["severity"] }, { "source", issue["source"] }, { "message", issue["message"] } };
            if (issue.contains("setting"))
                summary["setting"] = issue["setting"];
            issues.push_back(std::move(summary));
        }
        context["issues"] = std::move(issues);
    } catch (const ToolRetry&) {
        context["issues"] = "unknown (the slicer was updating)";
    }
    return context;
}

std::string text_arg(const json& args, const char* name)
{
    if (!args.contains(name) || args[name].is_null())
        return {};
    if (!args[name].is_string())
        throw ToolError(std::string("'") + name + "' must be a string");
    return args[name].get<std::string>();
}

std::string status_of(const json& record) { return record.contains("outcome") ? "confirmed" : "awaiting_outcome"; }

json summary_of(const json& record)
{
    const json context = record.value("context", json::object());
    json       types   = json::array();
    for (const json& slot : context.value("materials", json::object()).value("filament_slots", json::array()))
        if (slot.value("used_on_plate", true))
            types.push_back(slot.value("type", std::string()));
    json s = { { "id", record.value("id", std::string()) },
               { "created", record.value("created", std::string()) },
               { "status", status_of(record) },
               { "symptom", record.value("symptom", std::string()) },
               { "diagnosis", record.value("diagnosis", std::string()) },
               { "materials", std::move(types) },
               { "printer_preset", context.value("printer_preset", std::string()) },
               { "changes", record.value("changes", json::array()) } };
    if (record.contains("outcome"))
        s["outcome"] = record["outcome"];
    if (record.contains("capitalized_in"))
        s["capitalized_in"] = record["capitalized_in"];
    return s;
}

std::vector<std::string> words(const std::string& text)
{
    std::vector<std::string> out;
    std::string              lower = boost::algorithm::to_lower_copy(text);
    // Split on ASCII punctuation and spaces only, so that accented words stay whole.
    boost::algorithm::split(out, lower, [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u < 0x80 && !std::isalnum(u);
    }, boost::algorithm::token_compress_on);
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& w) { return w.size() < 3; }), out.end());
    return out;
}

} // namespace

void log_applied_changes(const json& changes, const std::string& reason)
{
    const std::string time = now_iso();
    for (json change : changes) {
        change["reason"] = reason;
        change["time"]   = time;
        pending_changes().push_back(std::move(change));
    }
}

json record_experience(const json& args)
{
    require_plater();
    const std::string id = text_arg(args, "id");

    json record;
    if (id.empty()) {
        const std::string symptom = text_arg(args, "symptom");
        if (symptom.empty())
            throw ToolError("a new experience needs the 'symptom'");
        record = { { "id", new_id() }, { "created", now_iso() }, { "symptom", symptom }, { "context", capture_context() },
                   { "changes", pending_changes() } };
        pending_changes() = json::array();
    } else {
        const fs::path path = record_path(id);
        if (!fs::exists(path))
            throw ToolError("no experience '" + id + "'");
        record = load_record(path);
        // Changes applied since the record was opened belong to it.
        if (!record.contains("changes") || !record["changes"].is_array())
            record["changes"] = json::array();
        for (const json& change : pending_changes())
            record["changes"].push_back(change);
        pending_changes() = json::array();
    }

    for (const char* field : { "diagnosis", "notes", "capitalized_in" })
        if (const std::string value = text_arg(args, field); !value.empty())
            record[field] = value;
    record["updated"] = now_iso();
    save_record(record);

    return { { "id", record["id"] },
             { "status", status_of(record) },
             { "changes_attached", record["changes"].size() },
             { "message", "Saved in the local journal. After the user has printed, record the real result with report_outcome." } };
}

json report_outcome(const json& args)
{
    require_plater();
    std::string id = text_arg(args, "id");
    if (id.empty()) {
        for (const json& record : all_records())
            if (!record.contains("outcome")) {
                id = record.value("id", std::string());
                break;
            }
        if (id.empty())
            throw ToolError("no experience is awaiting an outcome");
    }

    const std::string result = text_arg(args, "result");
    if (std::find(std::begin(RESULTS), std::end(RESULTS), result) == std::end(RESULTS))
        throw ToolError("'result' must be success, partial or failure");

    const fs::path path = record_path(id);
    if (!fs::exists(path))
        throw ToolError("no experience '" + id + "'");
    json record = load_record(path);

    json outcome = { { "result", result }, { "time", now_iso() }, { "source", "user" } };
    if (const std::string notes = text_arg(args, "notes"); !notes.empty())
        outcome["notes"] = notes;

    // Evidence from the printer when the user allowed it (state, error, duration of the last print).
    const json printer = printer_status(json::object());
    if (printer.value("connected", false) && printer.contains("print")) {
        json print = printer["print"];
        print.erase("filename");
        outcome["printer_report"] = std::move(print);
        if (printer.contains("last_error"))
            outcome["printer_report"]["last_error"] = printer["last_error"];
    }

    record["outcome"] = std::move(outcome);
    record["updated"] = now_iso();
    save_record(record);
    return { { "id", id }, { "status", "confirmed" }, { "outcome", record["outcome"] } };
}

json search_experiences(const json& args)
{
    const std::vector<std::string> query    = words(text_arg(args, "text"));
    const std::string              material = boost::algorithm::to_upper_copy(text_arg(args, "material"));
    const std::string              status   = text_arg(args, "status");
    const int                      limit    = std::clamp(args.value("limit", 20), 1, 500);

    struct Hit
    {
        int  score;
        json summary;
    };
    std::vector<Hit> hits;
    for (const json& record : all_records()) {
        if (!status.empty() && status != "all" && status_of(record) != status)
            continue;
        json summary = summary_of(record);
        if (!material.empty()) {
            bool found = false;
            for (const json& type : summary["materials"])
                found = found || boost::algorithm::to_upper_copy(type.get<std::string>()) == material;
            if (!found)
                continue;
        }
        int score = 0;
        if (!query.empty()) {
            const std::string haystack = boost::algorithm::to_lower_copy(
                record.value("symptom", std::string()) + " " + record.value("diagnosis", std::string()) + " " +
                record.value("notes", std::string()) + " " + record.value("changes", json::array()).dump());
            for (const std::string& w : query)
                score += boost::algorithm::contains(haystack, w) ? 1 : 0;
            if (score == 0)
                continue;
        }
        hits.push_back({ score, std::move(summary) });
    }
    // Best match first; all_records() already sorted by date for equal scores.
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });

    json out = json::array();
    for (size_t i = 0; i < hits.size() && int(i) < limit; ++i)
        out.push_back(std::move(hits[i].summary));
    return { { "count", hits.size() },
             { "experiences", std::move(out) },
             { "note", "Local journal of this user's prints. Only 'confirmed' records have a real outcome; none of them "
                       "has been reviewed: cite them as past cases, not as rules." } };
}

}} // namespace Slic3r::AIAssistant
