#include "McpSlicing.hpp"
#include "McpIssues.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <cmath>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;

namespace {

// Polls without any slicing activity before concluding that slicing did not
// produce a result (each poll is ~250 ms apart).
constexpr int IDLE_POLLS_BEFORE_FAILURE = 8;

double round2(double v) { return std::round(v * 100.) / 100.; }

json slice_results(GUI::Plater& plater)
{
    GUI::PartPlate*             plate  = plater.get_partplate_list().get_curr_plate();
    const GCodeProcessorResult* result = plate->get_slice_result();
    const Print&                print  = plater.fff_print();

    json out = { { "sliced", true }, { "plate", plater.get_partplate_list().get_curr_plate_index() + 1 } };
    if (result != nullptr) {
        using Stats            = PrintEstimatedStatistics;
        const Stats&      st   = result->print_statistics;
        const Stats::Mode& mode = st.modes[size_t(Stats::ETimeMode::Normal)];
        out["print_time_s"]    = std::lround(mode.time);

        json filaments = json::array();
        for (const auto& [extruder, volume] : st.total_volumes_per_extruder) {
            auto value = [extruder = extruder](const std::map<size_t, double>& m) {
                const auto it = m.find(extruder);
                return it == m.end() ? 0. : round2(it->second);
            };
            filaments.push_back({ { "filament_slot", extruder + 1 },
                                  { "total_mm3", round2(volume) },
                                  { "model_mm3", value(st.model_volumes_per_extruder) },
                                  { "support_mm3", value(st.support_volumes_per_extruder) },
                                  { "wipe_tower_mm3", value(st.wipe_tower_volumes_per_extruder) },
                                  { "flush_mm3", value(st.flush_per_filament) } });
        }
        out["filaments"] = std::move(filaments);

        json roles = json::object();
        for (const auto& [role, time] : mode.roles_times)
            if (time > 0.f)
                roles[ExtrusionEntity::role_to_string(role)]["time_s"] = std::lround(time);
        for (const auto& [role, used] : st.used_filaments_per_role)
            roles[ExtrusionEntity::role_to_string(role)]["filament_g"] = round2(used.second);
        out["by_feature"] = std::move(roles);

        if (result->toolpath_outside)
            out["toolpath_outside_build_volume"] = true;
    }

    json objects = json::array();
    for (const PrintObject* po : print.objects())
        objects.push_back({ { "object", po->model_object() ? po->model_object()->name : std::string() },
                            { "support_enabled", po->config().enable_support.value },
                            { "support_layers", po->support_layer_count() },
                            { "layers", po->layer_count() } });
    out["objects"] = std::move(objects);

    out["issues"] = collect_issues();
    return out;
}

} // namespace

json slice_plate(json& state)
{
    GUI::Plater& plater = require_plater();

    if (!state.value("started", false)) {
        // Slicing would be refused (or a blocking dialog shown): report why instead.
        json issues = collect_issues();
        bool blocked = false;
        for (const json& issue : issues["issues"])
            blocked = blocked || issue.value("blocks_slicing", false);
        if (blocked)
            return { { "sliced", false }, { "message", "Fix the blocking errors first" }, { "issues", std::move(issues) } };

        // Same entry point as the "Slice plate" button (switches to the preview).
        wxPostEvent(&plater, GUI::SimpleEvent(GUI::EVT_GLTOOLBAR_SLICE_PLATE));
        state["started"] = true;
        throw ToolRetry();
    }

    if (plater.is_background_process_slicing() || plater.is_background_process_update_scheduled()) {
        state["idle_polls"] = 0;
        throw ToolRetry();
    }
    if (plater.get_partplate_list().get_curr_plate()->is_slice_result_valid())
        return slice_results(plater);

    const int idle = state.value("idle_polls", 0) + 1;
    state["idle_polls"] = idle;
    if (idle < IDLE_POLLS_BEFORE_FAILURE)
        throw ToolRetry();
    return { { "sliced", false }, { "message", "Slicing did not produce a result" }, { "issues", collect_issues() } };
}

}} // namespace Slic3r::AIAssistant
