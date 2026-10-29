#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace AIAssistant {

// Setting documentation bundled in resources/ai_assistant/setting_docs.json,
// generated from doc/ by scripts/fork/build-setting-docs.py. Read once, lazily.

// Documentation of a setting: { page, section, text }, or a null json when the
// setting has no documentation page.
nlohmann::json setting_documentation(const std::string& key);

// What the model can rely on for a setting without documentation page.
std::string no_documentation_note(bool has_tooltip);

// get_docs (page): without a page, lists the pages; with a page (and
// optionally a section anchor), returns its text.
nlohmann::json read_documentation(const nlohmann::json& args);

}} // namespace Slic3r::AIAssistant
