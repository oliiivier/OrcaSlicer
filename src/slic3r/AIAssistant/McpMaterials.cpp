#include "McpMaterials.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::wxGetApp;

namespace {

std::string slot_value(const DynamicPrintConfig& config, const char* key, size_t idx)
{
    const auto* opt = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key));
    if (opt == nullptr)
        return {};
    const std::vector<std::string> values = opt->vserialize();
    return idx < values.size() ? values[idx] : std::string();
}

json describe_mixed(const MixedFilament& mf, unsigned int id)
{
    static const char* modes[] = { "layer_cycle", "same_layer_pointillisme", "simple" };
    json components = json::array();
    if (!mf.gradient_component_ids.empty()) {
        for (unsigned int c : MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids))
            components.push_back(c);
    } else {
        components = { mf.component_a, mf.component_b };
    }
    json out = { { "id", id },
                 { "components", std::move(components) },
                 { "mix_percent_of_second", mf.mix_b_percent },
                 { "distribution", mf.distribution_mode >= 0 && mf.distribution_mode < 3 ? modes[mf.distribution_mode] : "unknown" },
                 { "display_color", mf.display_color } };
    if (!mf.manual_pattern.empty())
        out["pattern"] = mf.manual_pattern;
    if (mf.gradient_enabled)
        out["gradient"] = { { "start", mf.gradient_start }, { "end", mf.gradient_end } };
    return out;
}

} // namespace

std::vector<int> object_filaments(const ModelObject& object)
{
    std::set<int> ids;
    for (const ModelVolume* volume : object.volumes)
        for (int id : volume->get_extruders())
            ids.insert(id);
    return { ids.begin(), ids.end() };
}

json describe_materials()
{
    GUI::Plater&              plater = require_plater();
    const PresetBundle&       bundle = *wxGetApp().preset_bundle;
    const DynamicPrintConfig  config = bundle.full_config();

    const auto*  nozzles   = config.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t toolheads = nozzles ? nozzles->values.size() : 1;
    const bool   semm      = config.opt_bool("single_extruder_multi_material");
    const char*  layout    = toolheads > 1 ? (semm ? "single_toolhead_multi_material" : "multi_toolhead") : "single_toolhead";

    const ConfigOption* model = config.option("printer_model");
    json printer = { { "model", model ? config.opt_string("printer_model") : std::string() },
                     { "toolhead_layout", layout },
                     { "toolheads", toolheads },
                     { "nozzle_diameters", nozzles ? json(nozzles->values) : json::array() } };
    if (toolheads > 1 && !semm)
        printer["note"] = "One independent toolhead per filament slot: slot N prints with toolhead N, filament "
                          "changes are tool changes (no purge of the nozzle). Per-toolhead printer settings are "
                          "lists in toolhead order.";

    // Filament slots used by the current plate, known once the print has been applied.
    std::set<unsigned int> used;
    const Print&           print = plater.fff_print();
    if (!print.empty())
        for (unsigned int e : print.extruders())
            used.insert(e + 1);

    const size_t physical = bundle.filament_presets.size();
    json         slots    = json::array();
    for (size_t i = 0; i < physical; ++i) {
        json slot = { { "slot", i + 1 },
                      { "preset", bundle.filament_presets[i] },
                      { "type", slot_value(config, "filament_type", i) },
                      { "color", slot_value(config, "filament_colour", i) },
                      { "nozzle_temperature", slot_value(config, "nozzle_temperature", i) } };
        if (toolheads > 1 && !semm)
            slot["toolhead"] = i + 1;
        if (!print.empty())
            slot["used_on_plate"] = used.count(unsigned(i + 1)) > 0;
        slots.push_back(std::move(slot));
    }

    json mixed = json::array();
    const MixedFilamentManager& mixed_mgr = bundle.mixed_filaments;
    for (unsigned int id = unsigned(physical) + 1; id <= unsigned(physical + mixed_mgr.enabled_count()); ++id)
        if (const MixedFilament* mf = mixed_mgr.mixed_filament_from_id(id, physical))
            mixed.push_back(describe_mixed(*mf, id));

    // 0 means "default": the filament of the object or part.
    json features = json::object();
    for (const char* key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament", "support_filament",
                             "support_interface_filament", "wipe_tower_filament" })
        if (config.option(key) != nullptr)
            features[key] = config.opt_int(key);

    json out = { { "printer", std::move(printer) },
                 { "filament_slots", std::move(slots) },
                 { "feature_filaments", std::move(features) },
                 { "edited_filament_preset", bundle.filaments.get_edited_preset().name } };
    if (!mixed.empty())
        out["mixed_filaments"] = std::move(mixed);
    return out;
}

}} // namespace Slic3r::AIAssistant
