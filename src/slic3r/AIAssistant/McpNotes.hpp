#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace AIAssistant {

// Instructions written by the user in the MCP settings, added to the server
// instructions. Stored in a Markdown file of the data directory so that it can
// also be edited outside the slicer. Any thread.
std::string user_instructions();
std::string user_instructions_path();
void        save_user_instructions(const std::string& text);

// Folder of Markdown notes shared by the user and the assistant: the setting,
// or the default folder of the data directory. GUI thread only.
std::string notes_dir();
std::string default_notes_dir();

// MCP tools on the notes folder. GUI thread only.
nlohmann::json list_notes(const nlohmann::json& args);
nlohmann::json read_note(const nlohmann::json& args);
nlohmann::json write_note(const nlohmann::json& args);

}} // namespace Slic3r::AIAssistant
