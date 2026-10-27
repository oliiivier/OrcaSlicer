#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace AIAssistant {

// Local journal of real print experiences, one JSON file per record in
// <data_dir>/ai_assistant/experiences/. Never part of the repository: records
// become curated guides only through the capitalize_experiences prompt and a
// review by the user. GUI thread only.
//
// A record holds facts captured by the server (printer, materials, issues,
// setting changes applied by apply_changes), the symptom and diagnosis given by
// the model, and the real outcome reported after printing.

// Called by apply_changes: keeps the applied changes until the next record.
void log_applied_changes(const nlohmann::json& changes, const std::string& reason);

nlohmann::json record_experience(const nlohmann::json& args);
nlohmann::json report_outcome(const nlohmann::json& args);
nlohmann::json search_experiences(const nlohmann::json& args);

}} // namespace Slic3r::AIAssistant
