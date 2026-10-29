#include "McpPresets.hpp"
#include "McpActions.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <boost/format.hpp>

#include <regex>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::into_u8;
using GUI::wxGetApp;

namespace {

struct PresetType
{
    const char*  name;
    Preset::Type type;
};

const PresetType PRESET_TYPES[] = {
    { "printer", Preset::TYPE_PRINTER },
    { "process", Preset::TYPE_PRINT },
    { "filament", Preset::TYPE_FILAMENT },
};

const PresetType& preset_type(const json& args)
{
    const std::string name = args.contains("type") && args["type"].is_string() ? args["type"].get<std::string>() : std::string();
    for (const PresetType& t : PRESET_TYPES)
        if (name == t.name)
            return t;
    throw ToolError("'type' must be printer, process or filament");
}

PresetCollection& collection(Preset::Type type)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    return type == Preset::TYPE_PRINTER ? static_cast<PresetCollection&>(bundle.printers) :
           type == Preset::TYPE_PRINT   ? bundle.prints :
                                          bundle.filaments;
}

std::string text_arg(const json& args, const char* name)
{
    if (!args.contains(name) || !args[name].is_string() || args[name].get<std::string>().empty())
        throw ToolError(std::string("'") + name + "' must be a non-empty string");
    return args[name].get<std::string>();
}

// Selected preset of a type, or of a filament slot (0-based).
std::string selected(Preset::Type type, int slot)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    if (type == Preset::TYPE_FILAMENT)
        return slot >= 0 && size_t(slot) < bundle.filament_presets.size() ? bundle.filament_presets[slot] : std::string();
    return collection(type).get_selected_preset_name();
}

// The preset list of the sidebar for a type (and filament slot).
GUI::PlaterPresetComboBox* sidebar_list(Preset::Type type, int slot)
{
    GUI::Sidebar& sidebar = wxGetApp().plater()->sidebar();
    if (type == Preset::TYPE_FILAMENT)
        return slot >= 0 && size_t(slot) < sidebar.combos_filament().size() ? sidebar.combos_filament()[slot] : nullptr;
    std::function<GUI::PlaterPresetComboBox*(wxWindow*)> find = [&](wxWindow* parent) -> GUI::PlaterPresetComboBox* {
        for (wxWindow* child : parent->GetChildren()) {
            if (auto* list = dynamic_cast<GUI::PlaterPresetComboBox*>(child); list != nullptr && list->get_type() == type)
                return list;
            if (GUI::PlaterPresetComboBox* found = find(child))
                return found;
        }
        return nullptr;
    };
    return find(&sidebar);
}

std::string color_arg(const json& args)
{
    const std::string color = args.contains("color") && args["color"].is_string() ? args["color"].get<std::string>() : std::string();
    if (!std::regex_match(color, std::regex("#[0-9a-fA-F]{6}")))
        throw ToolError("'color' must be a hex color such as #FF8000");
    return NormalizeFilamentHexColor(color);
}

json slots_summary()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto*   colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    json          out    = json::array();
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i)
        out.push_back({ { "filament_slot", i + 1 },
                        { "preset", bundle.filament_presets[i] },
                        { "color", colors != nullptr && i < colors->values.size() ? colors->values[i] : std::string() } });
    return out;
}

json selected_summary()
{
    return { { "printer", selected(Preset::TYPE_PRINTER, -1) },
             { "process", selected(Preset::TYPE_PRINT, -1) },
             { "filament_slots", slots_summary() } };
}

// Objects and parts printed with a slot (1-based), as "object" or "object / part".
std::vector<std::string> users_of_slot(int slot)
{
    std::vector<std::string> out;
    for (const ModelObject* obj : wxGetApp().model().objects) {
        const int object_slot = obj->config.has("extruder") ? obj->config.opt_int("extruder") : 0;
        if (object_slot == slot)
            out.push_back(obj->name);
        for (const ModelVolume* vol : obj->volumes)
            if (vol->config.has("extruder") && vol->config.opt_int("extruder") == slot && obj->volumes.size() > 1)
                out.push_back(obj->name + " / " + vol->name);
    }
    return out;
}

template<typename... Args> std::string text_of(const std::string& format, const Args&... args)
{
    boost::format f(format);
    (void) std::initializer_list<int>{ ((f % args), 0)... };
    return f.str();
}

json result(json extra = json::object())
{
    json out = done(std::move(extra));
    out["presets"] = selected_summary();
    return out;
}

// Checks that the sidebar offers the preset `name`; returns its item in the list
// (-1 for a process preset, selected through the settings tab).
int list_item(const PresetType& type, int slot, const std::string& name)
{
    const Preset* preset = collection(type.type).find_preset(name, false);
    if (preset == nullptr)
        throw ToolError(std::string("No ") + type.name + " preset named '" + name + "' (see get_project.presets.available)");
    if (!preset->is_visible || !preset->is_compatible)
        throw ToolError("'" + name + "' is hidden or not compatible with the selected printer: the sidebar does not offer it");
    // The process list is the one of the settings tab, shown in the sidebar.
    if (type.type == Preset::TYPE_PRINT)
        return -1;
    GUI::PlaterPresetComboBox* list = sidebar_list(type.type, slot);
    if (list == nullptr)
        throw ToolError("The preset list of the sidebar is not available");
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    for (unsigned int i = 0; i < list->GetCount(); ++i)
        if (bundle.get_preset_name_by_alias(type.type, Preset::remove_suffix_modified(into_u8(list->GetString(i)))) == name)
            return int(i);
    throw ToolError("'" + name + "' is not in the preset list of the sidebar");
}

// As a click in the list: the sidebar does the rest (nozzles, plates, flushing
// volumes...) and may ask what to do with unsaved changes. True when selected.
bool click(const PresetType& type, int slot, const std::string& name, int item)
{
    // The test mode cannot answer the slicer's own question about unsaved changes: they are dropped.
    if (test_confirm_answer())
        for (const PresetType& t : PRESET_TYPES)
            if (collection(t.type).current_is_dirty())
                collection(t.type).discard_current_changes();
    if (item < 0) {
        // What the list of the settings tab calls.
        wxGetApp().get_tab(type.type)->select_preset(name);
        return selected(type.type, slot) == name;
    }
    GUI::PlaterPresetComboBox* list = sidebar_list(type.type, slot);
    list->SetSelection(item);
    wxCommandEvent evt(wxEVT_COMBOBOX, list->GetId());
    evt.SetEventObject(list);
    evt.SetInt(item);
    list->GetEventHandler()->ProcessEvent(evt);
    return selected(type.type, slot) == name;
}

// ------------------------------------------------------------ actions

json select_preset(const json& args, const std::atomic<bool>& abandoned)
{
    const PresetType& type = preset_type(args);
    const std::string name = text_arg(args, "name");
    const int slot = type.type == Preset::TYPE_FILAMENT ? filament_slot_ref(args, "filament_slot") - 1 : -1;
    const int item = list_item(type, slot, name);
    if (selected(type.type, slot) == name)
        return { { "done", false }, { "message", "This preset is already selected" } };

    const std::string line = type.type == Preset::TYPE_PRINTER ? text_of(_u8L("Printer: %1%"), name) :
                             type.type == Preset::TYPE_PRINT   ? text_of(_u8L("Process: %1%"), name) :
                                                                 text_of(_u8L("Filament slot %1%: %2%"), slot + 1, name);
    if (!confirm(args.value("reason", std::string()), { line },
                 _L("The AI assistant asks to select this preset. The slicer asks what to do with unsaved changes. Select it?")))
        return declined();
    if (abandoned)
        return timed_out();
    if (!click(type, slot, name, item))
        return { { "done", false }, { "message", "The preset did not change: the user kept the current one" }, { "presets", selected_summary() } };
    return result();
}

json save_preset(const json& args, const std::atomic<bool>& abandoned)
{
    const PresetType& type = preset_type(args);
    const std::string name = text_arg(args, "name");
    PresetCollection& presets = collection(type.type);
    if (const Preset* existing = presets.find_preset(name, false); existing != nullptr && !existing->is_user())
        throw ToolError("'" + name + "' is a system preset: choose another name");
    if (Preset::remove_suffix_modified(name) != name)
        throw ToolError("'name' must not end with the modified mark");

    const Preset& edited = presets.get_edited_preset();
    const bool    exists = presets.find_preset(name, false) != nullptr;
    std::vector<std::string> lines = { text_of(_u8L("Save the edited preset '%1%' as '%2%'"), edited.name, name) };
    if (exists)
        lines.push_back(_u8L("The existing user preset with this name is replaced."));
    if (!confirm(args.value("reason", std::string()), lines, _L("The AI assistant asks to save a user preset. Save it?")))
        return declined();
    if (abandoned)
        return timed_out();

    // As the save button of the settings tab once the name is typed.
    GUI::Tab* tab = wxGetApp().get_tab(type.type);
    if (tab == nullptr)
        throw ToolError("The settings tab is not available");
    tab->save_preset(name);
    if (presets.get_selected_preset_name() != name)
        throw ToolError("The slicer did not save the preset '" + name + "'");
    return result({ { "saved", name } });
}

json add_slot(const json& args, const std::atomic<bool>& abandoned)
{
    PresetBundle&     bundle = *wxGetApp().preset_bundle;
    const std::string color  = args.contains("color") ? color_arg(args) : std::string();
    const std::string preset = args.contains("name") ? text_arg(args, "name") : std::string();
    // Checked on the first slot: every list offers the same presets.
    if (!preset.empty())
        list_item(PRESET_TYPES[2], 0, preset);
    const size_t before = bundle.filament_presets.size();
    std::string  line   = text_of(_u8L("Add filament slot %1%"), before + 1);
    if (!preset.empty())
        line += ": " + preset;
    if (!color.empty())
        line += " (" + color + ")";
    if (!confirm(args.value("reason", std::string()), { line }, _L("The AI assistant asks to add a filament slot. Add it?")))
        return declined();
    if (abandoned)
        return timed_out();

    // As the + button of the filament list.
    GUI::Sidebar& sidebar = wxGetApp().plater()->sidebar();
    if (color.empty())
        sidebar.add_filament();
    else
        sidebar.add_custom_filament(wxColour(from_u8(color)));
    if (bundle.filament_presets.size() != before + 1)
        throw ToolError("The slicer did not add a slot (too many filaments?)");
    if (!preset.empty() && bundle.filament_presets[before] != preset &&
        !click(PRESET_TYPES[2], int(before), preset, list_item(PRESET_TYPES[2], int(before), preset)))
        return result({ { "filament_slot", before + 1 }, { "message", "The slot was added, but its preset did not change" } });
    return result({ { "filament_slot", before + 1 } });
}

json remove_slot(const json& args, const std::atomic<bool>& abandoned)
{
    const int slot  = filament_slot_ref(args, "filament_slot");
    const int count = filament_slot_count();
    if (count < 2)
        throw ToolError("The last filament slot cannot be removed");
    int replace = -1;
    if (args.contains("replace_with")) {
        replace = filament_slot_ref(args, "replace_with");
        if (replace == slot)
            throw ToolError("'replace_with' must be another slot");
    }

    std::vector<std::string> lines = { text_of(_u8L("Remove filament slot %1% (%2%)"), slot,
                                               wxGetApp().preset_bundle->filament_presets[slot - 1]) };
    const std::vector<std::string> users = users_of_slot(slot);
    if (!users.empty()) {
        std::string names;
        for (const std::string& u : users)
            names += (names.empty() ? "" : ", ") + u;
        lines.push_back(replace > 0 ? text_of(_u8L("Printed with slot %1% afterwards: %2%"), replace, names) :
                                      text_of(_u8L("Printed with another slot afterwards: %1%"), names));
    }
    if (slot < count)
        lines.push_back(_u8L("The next slots move down by one."));
    if (!confirm(args.value("reason", std::string()), lines, _L("The AI assistant asks to remove a filament slot. Remove it?")))
        return declined();
    if (abandoned)
        return timed_out();

    // As Delete in the menu of the slot.
    wxGetApp().plater()->sidebar().delete_filament(size_t(slot - 1), replace > 0 ? replace - 1 : -1);
    if (filament_slot_count() != count - 1)
        return { { "done", false }, { "message", "The slot was not removed (the user cancelled)" }, { "presets", selected_summary() } };
    return result();
}

json set_color(const json& args, const std::atomic<bool>& abandoned)
{
    const int         slot  = filament_slot_ref(args, "filament_slot");
    const std::string color = color_arg(args);
    if (!confirm(args.value("reason", std::string()), { text_of(_u8L("Filament slot %1%: color %2%"), slot, color) },
                 _L("The AI assistant asks to change the color of a filament slot. Change it?")))
        return declined();
    if (abandoned)
        return timed_out();

    // As PlaterPresetComboBox::ApplyFilamentColor() for a single color, which is not public.
    DynamicPrintConfig& config = wxGetApp().preset_bundle->project_config;
    const size_t        index  = size_t(slot - 1);
    const FilamentColor single = FilamentColor::FromColors({ color }, FilamentColorMode::Segment);
    DynamicPrintConfig  change;
    auto*               colors = new ConfigOptionStrings(*config.option<ConfigOptionStrings>("filament_colour"));
    colors->values.resize(std::max(colors->values.size(), index + 1));
    colors->values[index] = single.PrimaryColor();
    change.set_key_value("filament_colour", colors);
    if (const auto* multi = config.option<ConfigOptionStrings>("filament_multi_colors")) {
        auto* values = new ConfigOptionStrings(*multi);
        values->values.resize(std::max(values->values.size(), index + 1));
        values->values[index] = single.ToMultiColorsString();
        change.set_key_value("filament_multi_colors", values);
    }
    if (const auto* modes = config.option<ConfigOptionInts>("filament_colour_mode")) {
        auto* values = new ConfigOptionInts(*modes);
        values->values.resize(std::max(values->values.size(), index + 1));
        values->values[index] = FilamentColorModeToConfig(single.NormalizedMode());
        change.set_key_value("filament_colour_mode", values);
    }
    config.apply(change);
    GUI::Plater* plater = wxGetApp().plater();
    plater->update_project_dirty_from_presets();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    if (GUI::PlaterPresetComboBox* list = sidebar_list(Preset::TYPE_FILAMENT, int(index)))
        list->update();
    plater->on_config_change(change);
    wxCommandEvent* evt = new wxCommandEvent(GUI::EVT_FILAMENT_COLOR_CHANGED);
    evt->SetInt(int(index));
    wxQueueEvent(plater, evt);
    return result();
}

} // namespace

json presets_summary()
{
    json available = json::object();
    for (const PresetType& t : PRESET_TYPES) {
        json names = json::array();
        for (const Preset& p : collection(t.type)())
            if (p.is_visible && p.is_compatible && !p.is_default)
                names.push_back(p.name);
        available[t.name] = std::move(names);
    }
    json out           = selected_summary();
    out["available"]   = std::move(available);
    out["unsaved"]     = { { "printer", wxGetApp().preset_bundle->printers.current_is_dirty() },
                           { "process", wxGetApp().preset_bundle->prints.current_is_dirty() },
                           { "filament", wxGetApp().preset_bundle->filaments.current_is_dirty() } };
    return out;
}

json edit_presets(const json& args, const std::atomic<bool>& abandoned)
{
    require_plater();
    const std::string action = args.contains("action") && args["action"].is_string() ? args["action"].get<std::string>() : std::string();
    if (action == "select")
        return select_preset(args, abandoned);
    if (action == "save")
        return save_preset(args, abandoned);
    if (action == "add_slot")
        return add_slot(args, abandoned);
    if (action == "remove_slot")
        return remove_slot(args, abandoned);
    if (action == "set_color")
        return set_color(args, abandoned);
    throw ToolError("'action' must be select, save, add_slot, remove_slot or set_color");
}

}} // namespace Slic3r::AIAssistant
