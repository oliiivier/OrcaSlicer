#include "ProjectAdapt.hpp"
#include "McpActions.hpp"
#include "McpSettings.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/timer.h>

#include <chrono>
#include <cmath>
#include <map>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::into_u8;
using GUI::wxGetApp;

namespace {

// Settings that only name presets or link them: not part of the designer's intent.
const std::set<std::string> NOT_SETTINGS = { "inherits", "print_settings_id", "compatible_printers",
                                             "compatible_printers_condition", "print_compatible_printers", "version",
                                             "from", "name", "setting_id", "filament_settings_id", "printer_settings_id" };

struct Toolhead
{
    int         number; // 1-based, as on the printer
    std::string filament, type, color, nozzle;
};

std::string lower(std::string s) { return boost::algorithm::to_lower_copy(boost::algorithm::trim_copy(s)); }

// Toolheads reported by the connected printer (the slicer keeps them up to date).
std::vector<Toolhead> printer_toolheads()
{
    std::vector<Toolhead> out;
    for (const ConnectMachineInfo& info : wxGetApp().preset_bundle->m_connect_machine_info_list)
        out.push_back({ info.index + 1, info.filament_info, info.filament_type, info.color_info, info.nozzle_info });
    return out;
}

bool loaded(const Toolhead& t) { return !t.filament.empty() && t.filament != "NONE"; }

// Model of the connected printer, e.g. "Snapmaker U1", as in the printer presets.
std::string connected_printer_model()
{
    const auto gateway = wxGetApp().gateway_service();
    if (!gateway || !wxGetApp().gateway_device_connected())
        return {};
    const auto result = gateway->request_sync("query.device.list", json::object(), std::chrono::seconds(3));
    if (result.error)
        return {};
    for (const json& device : result.value.value("devices", json::array()))
        if (device.is_object() && device.value("connected", false))
            return device.value("machineType", std::string());
    return {};
}

std::string config_string(const DynamicPrintConfig& config, const char* key)
{
    const auto* opt = config.option<ConfigOptionString>(key);
    return opt != nullptr ? opt->value : std::string();
}

std::vector<std::string> config_strings(const DynamicPrintConfig& config, const char* key)
{
    const auto* opt = config.option<ConfigOptionStrings>(key);
    return opt != nullptr ? opt->values : std::vector<std::string>();
}

// "#RRGGBB" -> distance in RGB space, 0..441.
double color_distance(const std::string& a, const std::string& b)
{
    auto rgb = [](std::string s, int out[3]) {
        if (!s.empty() && s[0] == '#')
            s.erase(0, 1);
        if (s.size() < 6)
            return false;
        try {
            for (int i = 0; i < 3; ++i)
                out[i] = std::stoi(s.substr(size_t(2 * i), 2), nullptr, 16);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    };
    int ca[3], cb[3];
    if (!rgb(a, ca) || !rgb(b, cb))
        return 442.;
    return std::sqrt(double((ca[0] - cb[0]) * (ca[0] - cb[0]) + (ca[1] - cb[1]) * (ca[1] - cb[1]) + (ca[2] - cb[2]) * (ca[2] - cb[2])));
}

// Index of the closest loaded toolhead (same material first, then closest color), or -1.
int closest_toolhead(const std::vector<Toolhead>& toolheads, const std::string& type, const std::string& color)
{
    int    best       = -1;
    double best_score = 1e9;
    for (size_t i = 0; i < toolheads.size(); ++i) {
        if (!loaded(toolheads[i]))
            continue;
        const double score = (lower(toolheads[i].type) == lower(type) ? 0. : 1000.) + color_distance(toolheads[i].color, color);
        if (score < best_score) {
            best_score = score;
            best       = int(i);
        }
    }
    return best;
}

// Nozzle shared by most toolheads, e.g. "0.4".
std::string common_nozzle(const std::vector<Toolhead>& toolheads)
{
    std::map<std::string, int> count;
    for (const Toolhead& t : toolheads) {
        std::string n = t.nozzle;
        if (n.size() > 2 && lower(n.substr(n.size() - 2)) == "mm")
            n.resize(n.size() - 2);
        boost::algorithm::trim(n);
        if (!n.empty())
            ++count[n];
    }
    std::string best;
    for (const auto& [nozzle, n] : count)
        if (best.empty() || n > count[best])
            best = nozzle;
    return best;
}

const Preset* system_printer_preset(const std::string& model, const std::string& variant)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    for (const Preset& p : bundle.printers)
        if (p.is_system && config_string(p.config, "printer_model") == model && config_string(p.config, "printer_variant") == variant)
            return &p;
    return bundle.get_similar_printer_preset(model, variant);
}

// Process preset of the selected printer with the closest layer height ("Standard" on a tie).
const Preset* closest_process_preset(double layer_height)
{
    const Preset* best      = nullptr;
    double        best_diff = 1e9;
    for (const Preset& p : wxGetApp().preset_bundle->prints) {
        if (!p.is_system || !p.is_compatible || !p.is_visible)
            continue;
        const auto* lh = p.config.option<ConfigOptionFloat>("layer_height");
        if (lh == nullptr)
            continue;
        double diff = std::abs(lh->value - layer_height);
        if (p.name.find("Standard") != std::string::npos)
            diff -= 1e-6;
        if (diff < best_diff) {
            best_diff = diff;
            best      = &p;
        }
    }
    return best;
}

// Filament preset of the selected printer for a loaded filament, as the filament
// sync of the slicer: same name before '@', else "Generic <type>".
const Preset* filament_preset_for(const Toolhead& t)
{
    auto base = [](const std::string& name) { return lower(name.substr(0, name.find('@'))); };
    const PresetBundle& bundle  = *wxGetApp().preset_bundle;
    const Preset*       generic = nullptr;
    for (const Preset& p : bundle.filaments) {
        if (!p.is_system || !p.is_compatible)
            continue;
        if (base(p.name) == lower(t.filament))
            return &p;
        if (generic == nullptr && base(p.name) == lower("Generic " + t.type))
            generic = &p;
    }
    return generic;
}

// Process settings of the project, serialized, to compare before and after the switch.
std::map<std::string, std::string> process_settings()
{
    std::map<std::string, std::string> out;
    const DynamicPrintConfig&          config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    for (const std::string& key : config.keys())
        if (!NOT_SETTINGS.count(key))
            if (const ConfigOption* opt = config.option(key))
                out[key] = opt->serialize();
    return out;
}

// Settings that hold a filament slot number (0 = automatic).
const char* FILAMENT_KEYS[] = { "extruder",        "wall_filament",    "sparse_infill_filament", "solid_infill_filament",
                                "support_filament", "support_interface_filament", "wipe_tower_filament" };

void remap_filament_settings(ModelConfig& config, const std::vector<int>& remap)
{
    for (const char* key : FILAMENT_KEYS)
        if (config.has(key)) {
            const int slot = config.opt_int(key);
            if (slot >= 1 && size_t(slot) < remap.size() && remap[size_t(slot)] > 0)
                config.set(key, remap[size_t(slot)]);
        }
}

struct Plan
{
    std::string                         source_printer, source_model, source_process, target_model;
    const Preset*                       printer { nullptr };
    double                              layer_height { 0.2 };
    std::vector<std::string>            types, colors, presets;
    std::vector<Toolhead>               toolheads;
    std::vector<size_t>                 slot_toolhead; // index in toolheads, per filament slot

    const Toolhead& toolhead_of(size_t slot) const { return toolheads[slot_toolhead[slot]]; }
    std::map<std::string, std::string>  source_settings;
};

Plan make_plan(const json& args)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    Plan          plan;
    plan.toolheads = printer_toolheads();
    if (plan.toolheads.empty() || !wxGetApp().gateway_device_connected())
        throw ToolError("No printer is connected in the slicer: connect it from the Device tab first");
    plan.target_model = connected_printer_model();
    if (plan.target_model.empty())
        throw ToolError("The model of the connected printer is unknown");

    const std::string nozzle = common_nozzle(plan.toolheads);
    plan.printer             = system_printer_preset(plan.target_model, nozzle);
    if (plan.printer == nullptr)
        throw ToolError("No printer preset for " + plan.target_model + " with a " + nozzle + " mm nozzle");

    const DynamicPrintConfig full = bundle.full_config();
    plan.source_printer           = bundle.printers.get_edited_preset().name;
    plan.source_model             = config_string(bundle.printers.get_edited_preset().config, "printer_model");
    plan.source_process           = bundle.prints.get_edited_preset().name;
    if (const auto* lh = full.option<ConfigOptionFloat>("layer_height"))
        plan.layer_height = lh->value;
    plan.types           = config_strings(full, "filament_type");
    plan.colors          = config_strings(full, "filament_colour");
    plan.presets         = bundle.filament_presets;
    plan.source_settings = process_settings();

    const size_t slots = bundle.filament_presets.size();
    plan.types.resize(slots);
    plan.colors.resize(slots);
    for (size_t i = 0; i < slots; ++i) {
        const int t = closest_toolhead(plan.toolheads, plan.types[i], plan.colors[i]);
        if (t < 0)
            throw ToolError("No filament is loaded in the printer");
        plan.slot_toolhead.push_back(size_t(t));
    }

    // The assistant may choose some toolheads itself.
    for (const json& m : args.value("filament_mapping", json::array())) {
        const int slot = m.value("slot", 0), toolhead = m.value("toolhead", 0);
        if (slot < 1 || size_t(slot) > slots)
            throw ToolError("filament_mapping: slot must be between 1 and " + std::to_string(slots));
        const auto it = std::find_if(plan.toolheads.begin(), plan.toolheads.end(), [toolhead](const Toolhead& t) { return t.number == toolhead; });
        if (it == plan.toolheads.end() || !loaded(*it))
            throw ToolError("filament_mapping: toolhead " + std::to_string(toolhead) + " has no filament loaded");
        plan.slot_toolhead[size_t(slot - 1)] = size_t(it - plan.toolheads.begin());
    }
    return plan;
}

bool confirm(const Plan& plan)
{
    std::string text = _u8L("Printer") + ": " + plan.source_printer + " -> " + plan.printer->name + "\n" + _u8L("Process") + ": " +
                       plan.source_process + " -> " +
                       (boost::format(_u8L("the closest profile for a layer height of %1% mm")) %
                        float_to_string_decimal_point(plan.layer_height, 2))
                           .str() +
                       "\n\n" + _u8L("Filaments") + ":\n";
    for (size_t i = 0; i < plan.slot_toolhead.size(); ++i) {
        const Toolhead& t = plan.toolhead_of(i);
        text += "- " + std::to_string(i + 1) + ". " + plan.types[i] + " " + plan.colors[i] + " -> " +
                (boost::format(_u8L("slot and toolhead %1%")) % t.number).str() + " (" + t.filament + " " + t.color + ")\n";
    }
    if (const std::optional<bool> answer = test_confirm_answer())
        return *answer;
    GUI::MessageDialog dlg(wxGetApp().mainframe,
                           from_u8(text) + "\n" +
                               _L("Adapt this project to the connected printer? The printer, process and filament presets "
                                  "change; the toolheads can still be changed when printing."),
                           _L("Adapt the project"), wxYES_NO | wxICON_QUESTION);
    return dlg.ShowModal() == wxID_YES;
}

json apply(const Plan& plan)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const size_t  slots  = plan.slot_toolhead.size();

    // As the nozzle sync button of the sidebar. The slicer may ask what to do with
    // unsaved changes: keeping the current printer stops here.
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(plan.printer->name);
    if (bundle.printers.get_edited_preset().name != plan.printer->name)
        return { { "done", false }, { "message", "The printer preset was not changed (the user kept the current one)" } };

    std::string process = bundle.prints.get_edited_preset().name;
    if (const Preset* p = closest_process_preset(plan.layer_height); p != nullptr && p->name != process) {
        wxGetApp().get_tab(Preset::TYPE_PRINT)->select_preset(p->name);
        process = bundle.prints.get_edited_preset().name;
    }

    // On this printer filament slot N prints with toolhead N by default: the
    // project gets one slot per toolhead, with its preset and color, and every
    // reference to an original slot moves to the slot of its toolhead.
    const size_t      toolheads = plan.toolheads.size();
    std::vector<int>  remap(slots + 1, 0); // original slot (1-based) -> new slot
    for (size_t i = 0; i < slots; ++i)
        remap[i + 1] = plan.toolhead_of(i).number;

    if (bundle.filament_presets.size() != toolheads)
        bundle.set_num_filaments(unsigned(toolheads));
    // Moves the painted areas (colors painted on the model), applied by on_filaments_change.
    bundle.set_filament_id_remap(std::vector<unsigned int>(remap.begin(), remap.end()));

    auto* colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    for (const Toolhead& t : plan.toolheads) {
        if (!loaded(t) || t.number < 1 || size_t(t.number) > toolheads)
            continue;
        const size_t idx = size_t(t.number - 1);
        if (const Preset* preset = filament_preset_for(t))
            bundle.set_filament_preset(idx, preset->name);
        if (colors != nullptr && idx < colors->values.size() && !t.color.empty())
            colors->values[idx] = t.color;
    }

    // Before on_filaments_change, which would reset numbers beyond the new slot count.
    GUI::Plater* plater = wxGetApp().plater();
    for (ModelObject* obj : plater->model().objects) {
        remap_filament_settings(obj->config, remap);
        for (ModelVolume* vol : obj->volumes)
            remap_filament_settings(vol->config, remap);
        for (auto& range : obj->layer_config_ranges)
            remap_filament_settings(range.second, remap);
    }
    plater->on_filaments_change(toolheads);
    // Colors of the 3D view, object list and filament lists, as after a color change.
    plater->on_config_change(bundle.full_config());
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    bundle.export_selections(*wxGetApp().app_config);
    plater->sidebar().update_all_preset_comboboxes(false);
    plater->changed_objects([&plater]() {
        std::vector<size_t> all(plater->model().objects.size());
        for (size_t i = 0; i < all.size(); ++i)
            all[i] = i;
        return all;
    }());
    plater->update_project_dirty_from_presets();

    json filaments = json::array();
    for (size_t i = 0; i < slots; ++i) {
        const Toolhead& t = plan.toolhead_of(i);
        filaments.push_back({ { "original_slot", i + 1 },
                              { "original", { { "type", plan.types[i] }, { "color", plan.colors[i] }, { "preset", i < plan.presets.size() ? plan.presets[i] : "" } } },
                              { "new_slot_and_toolhead", t.number },
                              { "loaded", { { "filament", t.filament }, { "color", t.color } } } });
    }

    // What the switch changed in the process settings: the designer's choices to
    // restore where they make sense on the new printer.
    json                                     changed = json::array();
    const std::map<std::string, std::string> after   = process_settings();
    for (const auto& [key, before] : plan.source_settings) {
        const auto it = after.find(key);
        if (it == after.end() || it->second == before)
            continue;
        const ConfigOptionDef* def = print_config_def.get(key);
        changed.push_back({ { "key", key }, { "label", def ? def->label : key }, { "original", before }, { "now", it->second } });
    }

    size_t overrides = 0;
    for (const ModelObject* obj : plater->model().objects)
        if (!obj->config.empty())
            ++overrides;

    return { { "done", true },
             { "printer", { { "before", plan.source_printer }, { "before_model", plan.source_model }, { "after", plan.printer->name } } },
             { "process", { { "before", plan.source_process }, { "after", process } } },
             { "filaments", std::move(filaments) },
             { "slot_numbering",
               "Filament slots now match the toolheads. Original values of settings holding a filament number "
               "(support_filament, wall_filament...) use the original_slot numbers: translate them to "
               "new_slot_and_toolhead before restoring them." },
             { "changed_settings", std::move(changed) },
             { "objects_with_own_settings", overrides },
             { "next",
               "Restore the original values that express the designer's intent (layer height, walls, infill, supports, "
               "brim, seam, ironing...) when they suit the new printer, with apply_changes; keep the new printer's "
               "values for machine-specific settings (speeds and accelerations beyond its limits, prime tower or "
               "flushing made for another printer). Then get_issues, slice and get_view." } };
}

class AdaptationWatch : public wxEvtHandler
{
public:
    AdaptationWatch() : m_timer(this)
    {
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
        m_timer.Start(3000);
    }

    void stop() { m_timer.Stop(); }

private:
    void tick()
    {
        GUI::Plater* plater = wxGetApp().plater();
        if (plater == nullptr || wxGetApp().is_gcode_viewer() || !McpSettings::load().suggest_adaptation)
            return;
        if (!wxGetApp().gateway_device_connected() || wxGetApp().preset_bundle->m_connect_machine_info_list.empty()) {
            m_model.clear();
            return;
        }
        if (m_model.empty()) {
            query_model();
            return;
        }
        PresetBundle&     bundle = *wxGetApp().preset_bundle;
        const std::string model  = config_string(bundle.printers.get_edited_preset().config, "printer_model");
        if (model == m_model || plater->model().objects.empty())
            return;

        // Once per project and printer preset: closing the notification is an answer.
        const std::string key = bundle.printers.get_edited_preset().name + "|" + into_u8(plater->get_project_name());
        if (key == m_notified)
            return;
        m_notified = key;

        const std::string text = (boost::format(_u8L("This project is set up for %1% but the connected printer is a %2%.")) %
                                  (model.empty() ? bundle.printers.get_edited_preset().name : model) % m_model)
                                     .str();
        plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel, text,
            _u8L("Adapt it to this printer"), [](wxEvtHandler*) {
                adapt_from_notification();
                return true;
            });
    }

    void query_model()
    {
        const auto gateway = wxGetApp().gateway_service();
        if (m_querying || !gateway)
            return;
        m_querying = true;
        gateway->request("query.device.list", json::object(), [this](Gateway::GatewayError error, const json& result) {
            m_querying = false;
            if (error)
                return;
            for (const json& device : result.value("devices", json::array()))
                if (device.is_object() && device.value("connected", false))
                    m_model = device.value("machineType", std::string());
        });
    }

    static void adapt_from_notification()
    {
        try {
            const std::atomic<bool> never_abandoned { false };
            const json              result = adapt_to_printer(json::object(), never_abandoned);
            if (!result.value("done", false))
                return;
            const size_t changed = result["changed_settings"].size();
            GUI::MessageDialog dlg(wxGetApp().mainframe,
                                   wxString::Format(_L("The project now uses %s. %zu settings of the original project differ "
                                                      "from the new profile: ask the AI assistant (/adapt_project) to restore "
                                                      "the original intent where it suits this printer."),
                                                    from_u8(result["printer"]["after"].get<std::string>()), changed),
                                   _L("Adapt the project"), wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
        } catch (const std::exception& e) {
            GUI::MessageDialog dlg(wxGetApp().mainframe, from_u8(e.what()), _L("Adapt the project"), wxOK | wxICON_WARNING);
            dlg.ShowModal();
        }
    }

    wxTimer     m_timer;
    std::string m_model;    // of the connected printer
    std::string m_notified; // project and printer preset of the last notification
    bool        m_querying { false };
};

} // namespace

json adapt_to_printer(const json& args, const std::atomic<bool>& abandoned)
{
    require_plater();
    const Plan plan = make_plan(args);
    if (!confirm(plan))
        return declined();
    if (abandoned)
        return timed_out();
    return apply(plan);
}

void start_adaptation_watch()
{
    static bool started = false;
    if (started || wxGetApp().mainframe == nullptr)
        return;
    started = true;
    // Stopped with the main frame but never freed: a gateway reply may still be on its way.
    auto* watch = new AdaptationWatch();
    wxGetApp().mainframe->Bind(wxEVT_DESTROY, [watch](wxWindowDestroyEvent& evt) {
        if (evt.GetEventObject() == wxGetApp().mainframe)
            watch->stop();
        evt.Skip();
    });
}

}} // namespace Slic3r::AIAssistant
