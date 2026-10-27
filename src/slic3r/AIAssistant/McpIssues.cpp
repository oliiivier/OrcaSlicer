#include "McpIssues.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::Plater;
using GUI::wxGetApp;

namespace {

json make_issue(const char* severity, bool blocks_slicing, const char* source, const std::string& message)
{
    return { { "severity", severity }, { "blocks_slicing", blocks_slicing }, { "source", source }, { "message", message } };
}

void attach_setting(json& issue, const std::string& key)
{
    if (key.empty())
        return;
    issue["setting"] = key;
    if (const ConfigOptionDef* def = print_config_def.get(key)) {
        json summary = setting_summary(*def);
        const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
        if (const ConfigOption* opt = config.option(key))
            summary["current"] = option_value(*opt);
        issue["setting_info"] = std::move(summary);
    }
}

void attach_object(json& issue, const ObjectBase* object)
{
    const auto* po = dynamic_cast<const PrintObjectBase*>(object);
    const auto* mo = po ? po->model_object() : dynamic_cast<const ModelObject*>(object);
    if (mo != nullptr)
        issue["object"] = mo->name;
}

json from_validation(const StringObjectException& e, const char* severity, bool blocks_slicing)
{
    json issue = make_issue(severity, blocks_slicing, "validation", e.string);
    attach_setting(issue, e.opt_key);
    attach_object(issue, e.object);
    // Context of the message, e.g. the 1-based filament/extruder number for filament/bed mismatches.
    if (!e.params.empty())
        issue["details"] = e.params;
    return issue;
}

std::string slots_text(const std::vector<int>& slots)
{
    std::string out;
    for (int slot : slots)
        out += (out.empty() ? "" : ", ") + std::to_string(slot);
    return out;
}

void add_plate_blockers(Plater& plater, int plate, json& issues)
{
    const Plater::FilamentTempMixingState mixing_state = plater.get_filament_temp_mixing_state(plate);
    if (mixing_state != Plater::FilamentTempMixingState::Compatible) {
        const bool                       blocked = mixing_state == Plater::FilamentTempMixingState::BlockedError;
        Plater::FilamentTempMixingDetail mixing;
        plater.check_filament_temp_mixing(plate, mixing);
        json issue = make_issue(blocked ? "error" : "warning", blocked, "filament_temperature_mixing",
                                "High-temperature filaments (slots " + slots_text(mixing.high_temp_slots_1based) +
                                    ") and low-temperature filaments (slots " + slots_text(mixing.low_temp_slots_1based) +
                                    ") are used on the same plate.");
        issue["filament_slots"] = { { "high_temperature", mixing.high_temp_slots_1based },
                                    { "low_temperature", mixing.low_temp_slots_1based } };
        issues.push_back(std::move(issue));
    }

    Plater::FlowRatioZeroDetail flow;
    if (!plater.check_flow_ratio_zero(plate, flow)) {
        json issue = make_issue("error", true, "flow_ratio_zero",
                                "The filaments in slots " + slots_text(flow.offender_slots_1based) + " have a flow ratio of 0.");
        attach_setting(issue, "filament_flow_ratio");
        issue["filament_slots"] = flow.offender_slots_1based;
        issues.push_back(std::move(issue));
    }

    const Plater::ColdPlateCompatResult cold = plater.get_cold_plate_compat_state(plate);
    if (cold.state == Plater::ColdPlateCompatState::BlockedError) {
        json issue = make_issue("error", true, "cold_plate",
                                "The filaments in slots " + slots_text(cold.unsupported_slots_1_based) +
                                    " have no bed temperature for the selected cold plate.");
        attach_setting(issue, "supertack_plate_temp");
        issue["filament_slots"] = cold.unsupported_slots_1_based;
        issues.push_back(std::move(issue));
    } else if (cold.state == Plater::ColdPlateCompatState::SeriousWarning) {
        json issue = make_issue("warning", false, "cold_plate",
                                "TPU (slots " + slots_text(cold.tpu_slots_1_based) + ") is used on the cold plate.");
        issue["filament_slots"] = cold.tpu_slots_1_based;
        issues.push_back(std::move(issue));
    }
}

void add_warnings(const PrintStateBase::StateWithWarnings& state, const std::string& object, json& issues)
{
    for (const PrintStateBase::Warning& w : state.warnings) {
        if (!w.current)
            continue;
        json issue = make_issue("warning", false, "slicing", w.message);
        if (w.level == PrintStateBase::WarningLevel::CRITICAL)
            issue["critical"] = true;
        if (!object.empty())
            issue["object"] = object;
        issues.push_back(std::move(issue));
    }
}

void add_slicing_results(const Print& print, json& issues)
{
    for (int step = 0; step < int(psCount); ++step)
        add_warnings(print.step_state_with_warnings(static_cast<PrintStep>(step)), {}, issues);
    for (const PrintObject* po : print.objects()) {
        const std::string name = po->model_object() ? po->model_object()->name : std::string();
        for (int step = 0; step < int(posCount); ++step)
            add_warnings(po->step_state_with_warnings(static_cast<PrintObjectStep>(step)), name, issues);
    }

    if (const auto conflict = print.get_conflict_result())
        issues.push_back(make_issue("warning", false, "path_conflict",
                                    "G-code paths of objects '" + conflict->_objName1 + "' and '" + conflict->_objName2 +
                                        "' collide (e.g. with the nozzle or the wipe tower)."));
}

} // namespace

json collect_issues()
{
    Plater& plater = require_plater();
    if (plater.is_background_process_update_scheduled())
        throw ToolRetry();

    const int plate  = plater.get_partplate_list().get_curr_plate_index();
    json      issues = json::array();

    if (GLCanvas3D* canvas = plater.get_view3D_canvas3D();
        canvas != nullptr && canvas->check_volumes_outside_state() == ModelInstancePVS_Partly_Outside)
        issues.push_back(make_issue("error", true, "build_volume",
                                    "An object is laid over the boundary of the plate or exceeds the height limit."));

    Print& print = plater.fff_print();
    if (!print.empty()) {
        // Same call as BackgroundSlicingProcess::validate().
        print.is_BBL_printer() = wxGetApp().preset_bundle->is_bbl_vendor();
        StringObjectException warning;
        StringObjectException error = print.validate(&warning);
        plater.post_process_string_object_exception(error);
        if (!error.string.empty())
            issues.push_back(from_validation(error, "error", true));
        if (!warning.string.empty())
            issues.push_back(from_validation(warning, "warning", false));
    }

    add_plate_blockers(plater, plate, issues);
    add_slicing_results(print, issues);

    std::stable_sort(issues.begin(), issues.end(), [](const json& a, const json& b) {
        return a["severity"] == "error" && b["severity"] != "error";
    });

    const size_t errors = size_t(
        std::count_if(issues.begin(), issues.end(), [](const json& i) { return i["severity"] == "error"; }));
    return { { "plate", plate + 1 },
             { "summary", std::to_string(errors) + " error(s), " + std::to_string(issues.size() - errors) + " warning(s)" },
             { "issues", std::move(issues) } };
}

}} // namespace Slic3r::AIAssistant
