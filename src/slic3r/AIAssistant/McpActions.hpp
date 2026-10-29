#pragma once

// Conventions shared by the MCP tools that change something (PLAN.md, section 3):
// one confirmation style, one result shape, the same references to objects,
// parts, plates, filament slots and toolheads. GUI thread only.

#include <nlohmann/json.hpp>

#include <boost/filesystem/path.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

class wxString;

namespace Slic3r {
class ModelObject;
namespace GUI { class PartPlate; }

namespace AIAssistant {

// ------------------------------------------------------------ confirmation

// Asks the user in the slicer: the reason given by the model, the operations as
// a list (may be empty), then the question. Returns true for Yes.
bool confirm(const std::string& reason, const std::vector<std::string>& lines, const wxString& question);

// The answer the test mode gives to every confirmation, without showing it:
// SNAPMAKER_AI_TEST_CONFIRM=yes|no, read only in Debug builds compiled with
// SNAPMAKER_AI_TEST_HOOKS (see scripts/fork/mcp-test.py). Always empty otherwise.
std::optional<bool> test_confirm_answer();

// Shows that the test mode is on (a red notification that stays). Does nothing outside it.
void show_test_mode_banner();

// ------------------------------------------------------------ results

// { done: true } plus the fields of `extra`.
nlohmann::json done(nlohmann::json extra = nlohmann::json::object());
// The user answered No.
nlohmann::json declined();
// The client stopped waiting before the user answered: nothing was changed.
nlohmann::json timed_out();
// A list of operations stopped by an error: `applied` lists the operations done
// before it (the call is done when there are some), `failed` the operation and
// its error, `not_run` the number of operations left.
nlohmann::json operations_failed(const nlohmann::json& applied, const nlohmann::json& op, const std::string& error, size_t not_run);
// Adds the summary of the current issues to a result.
void add_issues_summary(nlohmann::json& out);

// ------------------------------------------------------------ references, all 1-based

// 0-based index of the object given by name or 1-based number.
int object_ref(const nlohmann::json& ref);
// 0-based index of the part of `obj` given by name or 1-based number.
int part_ref(const ModelObject& obj, const nlohmann::json& ref);
// The plate op.plate, or the current plate without it.
GUI::PartPlate& plate_ref(const nlohmann::json& op);
// Filament slot op[key], checked against the slots of the project; 0 only when allowed.
int filament_slot_ref(const nlohmann::json& op, const char* key, bool allow_zero = false);
int filament_slot_count();

// ------------------------------------------------------------ files

// Absolute path given by the model ("~" for the home folder); throws ToolError otherwise.
boost::filesystem::path absolute_path(const nlohmann::json& arg, const char* name = "path");
// An existing file with one of the extensions (lower case, with the dot).
boost::filesystem::path existing_file(const nlohmann::json& arg, const std::set<std::string>& extensions);
// A file to write, with the extension `ext`, in an existing folder; an existing
// file is only accepted with `overwrite`.
boost::filesystem::path output_file(const nlohmann::json& arg, const std::string& ext, bool overwrite);

} // namespace AIAssistant
} // namespace Slic3r
