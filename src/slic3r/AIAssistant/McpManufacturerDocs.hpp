#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r { namespace AIAssistant {

// Snapmaker's own documentation, fetched live: the wiki (wiki.snapmaker.com,
// procedures, troubleshooting, firmware notes) and the support site
// (support.snapmaker.com). Only these two hosts are ever contacted, and only
// when the user allowed it in the MCP settings.
// The requests run in the background: the tools answer ToolRetry until they
// end and keep their progress in `state`. GUI thread only.
nlohmann::json search_manufacturer_docs(const nlohmann::json& args, nlohmann::json& state);
nlohmann::json read_manufacturer_page(const nlohmann::json& args, nlohmann::json& state);

// Forgets the requests of an abandoned call.
void manufacturer_docs_abandoned(nlohmann::json& state);

}} // namespace Slic3r::AIAssistant
