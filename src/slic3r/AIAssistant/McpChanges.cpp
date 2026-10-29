#include "McpChanges.hpp"
#include "McpChangesDialog.hpp"
#include "McpExperiences.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"

#include <algorithm>
#include <climits>
#include <memory>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::wxGetApp;

namespace {

struct Change
{
    std::string                   key;
    const ConfigOptionDef*        def { nullptr };
    std::unique_ptr<ConfigOption> value;
    std::string                   old_value;
    // Global change: preset type of the tab holding the key. Object change: object index.
    Preset::Type                  preset_type { Preset::TYPE_INVALID };
    int                           object_idx { -1 };
    // Filament change: 1-based slot and the preset it edits.
    int                           filament_slot { 0 };
    std::string                   filament_preset;
    // Per-toolhead printer change limited to one toolhead (1-based), 0 for the whole list.
    int                           toolhead { 0 };
    // Assistant's reason for this change, shown to the user.
    std::string                   comment;
};

const char* preset_type_name(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT: return "process";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER: return "printer";
    default: return "unknown";
    }
}

bool is_object_setting(const std::string& key)
{
    return PrintObjectConfig::defaults().option(key) != nullptr || PrintRegionConfig::defaults().option(key) != nullptr;
}

int int_arg(const json& item, const char* name, int default_value)
{
    if (!item.contains(name) || item[name].is_null())
        return default_value;
    if (!item[name].is_number_integer())
        throw ToolError(std::string("'") + name + "' must be an integer");
    return item[name].get<int>();
}

// Accepts the get_config format (string or array) as well as plain JSON numbers and booleans.
std::string value_arg(const json& raw)
{
    if (raw.is_string())
        return raw.get<std::string>();
    if (raw.is_boolean())
        return raw.get<bool>() ? "1" : "0";
    if (raw.is_array()) {
        std::string out;
        for (const json& item : raw)
            out += (out.empty() ? "" : ",") + value_arg(item);
        return out;
    }
    return raw.dump();
}

// Returns an error message, or an empty string when the value is within the setting limits.
std::string check_range(const ConfigOptionDef& def, const ConfigOption& opt)
{
    // Nullable options store "nil" as a sentinel value that must not be range checked.
    if ((def.min == INT_MIN && def.max == INT_MAX) || def.nullable)
        return {};

    std::vector<double> values;
    switch (def.type) {
    case coFloat:
    case coPercent: values.push_back(opt.getFloat()); break;
    case coInt: values.push_back(opt.getInt()); break;
    case coFloats:
    case coPercents:
        if (const auto* vec = dynamic_cast<const ConfigOptionVector<double>*>(&opt))
            values.assign(vec->values.begin(), vec->values.end());
        break;
    case coInts:
        if (const auto* vec = dynamic_cast<const ConfigOptionVector<int>*>(&opt))
            values.assign(vec->values.begin(), vec->values.end());
        break;
    default: return {}; // float-or-percent and others: limits do not apply to the raw value
    }

    for (double v : values)
        if (v < def.min || v > def.max)
            // json::dump is locale independent, unlike std::to_string.
            return "value " + json(v).dump() + " is outside [" + json(def.min).dump() + ", " + json(def.max).dump() + "]";
    return {};
}

// Current value of a filament setting in the preset of the given slot.
const ConfigOption* filament_slot_option(Change& c, const json& item)
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const int           slots  = int(bundle.filament_presets.size());
    c.filament_slot            = int_arg(item, "filament_slot", slots == 1 ? 1 : 0);
    if (c.filament_slot < 1 || c.filament_slot > slots)
        throw ToolError("filament setting: give 'filament_slot' between 1 and " + std::to_string(slots));
    c.filament_preset = bundle.filament_presets[c.filament_slot - 1];

    const Preset& edited = bundle.filaments.get_edited_preset();
    if (c.filament_preset == edited.name)
        return edited.config.option(c.key);

    // Editing another slot means switching the Filament settings to its preset,
    // which would prompt about the unsaved changes of the current one.
    if (bundle.filaments.current_is_dirty())
        throw ToolError("the filament preset '" + edited.name + "' has unsaved changes: ask the user to save or discard them "
                        "before changing slot " + std::to_string(c.filament_slot));
    const Preset* preset = bundle.filaments.find_preset(c.filament_preset);
    if (preset == nullptr)
        throw ToolError("filament preset '" + c.filament_preset + "' not found");
    return preset->config.option(c.key);
}

Change parse_change(const json& item)
{
    if (!item.is_object() || !item.contains("key") || !item["key"].is_string() || !item.contains("value"))
        throw ToolError("Each change needs a 'key' and a 'value'");

    Change c;
    c.key = item["key"].get<std::string>();
    // Settings that run programs on this computer: never set by the assistant, even
    // confirmed, as a crafted project or prompt could get a script approved.
    static const std::set<std::string> forbidden = { "post_process" };
    if (forbidden.count(c.key))
        throw ToolError("this setting runs programs on the computer and cannot be changed by the assistant; ask the user "
                        "to edit it in the slicer if needed");
    c.def = print_config_def.get(c.key);
    if (c.def == nullptr)
        throw ToolError("unknown setting");
    if (c.def->readonly)
        throw ToolError("read-only setting");
    const std::string value = value_arg(item["value"]);
    if (item.contains("comment") && item["comment"].is_string())
        c.comment = item["comment"].get<std::string>();

    // Holds the inherited values for object overrides; `current` may point into it.
    DynamicPrintConfig  full;
    const ConfigOption* current = nullptr;
    if (item.contains("object") && !item["object"].is_null()) {
        if (!item["object"].is_string())
            throw ToolError("'object' must be an object name");
        if (!is_object_setting(c.key))
            throw ToolError("this setting cannot be overridden per object");
        c.object_idx           = find_object(item["object"].get<std::string>());
        const ModelObject* obj = wxGetApp().model().objects[c.object_idx];
        full                   = wxGetApp().preset_bundle->full_config();
        current = obj->config.get().option(c.key) ? obj->config.get().option(c.key) : full.option(c.key);
        if (current == nullptr)
            throw ToolError("setting not found");
    } else {
        for (Preset::Type type : { Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER }) {
            GUI::Tab* tab = wxGetApp().get_tab(type);
            if (tab != nullptr && tab->get_config()->option(c.key) != nullptr) {
                c.preset_type = type;
                current       = tab->get_config()->option(c.key);
                break;
            }
        }
        if (current == nullptr)
            throw ToolError("not a process, filament or printer preset setting");
        if (c.preset_type == Preset::TYPE_FILAMENT)
            current = filament_slot_option(c, item);
        if (current == nullptr)
            throw ToolError("setting missing from the filament preset");
    }
    c.old_value = current->serialize();
    c.value.reset(current->clone());

    c.toolhead = int_arg(item, "toolhead", 0);
    if (c.toolhead != 0) {
        auto* vec = dynamic_cast<ConfigOptionVectorBase*>(c.value.get());
        if (c.preset_type != Preset::TYPE_PRINTER || vec == nullptr)
            throw ToolError("'toolhead' only applies to per-toolhead printer settings");
        if (c.toolhead < 1 || size_t(c.toolhead) > vec->size())
            throw ToolError("'toolhead' must be between 1 and " + std::to_string(vec->size()));
        std::unique_ptr<ConfigOption> single(c.def->create_empty_option());
        if (!single->deserialize(value) || static_cast<ConfigOptionVectorBase*>(single.get())->size() != 1)
            throw ToolError("invalid value '" + value + "' for one toolhead, see describe_setting");
        vec->set_at(single.get(), size_t(c.toolhead - 1), 0);
    } else if (!c.value->deserialize(value)) {
        throw ToolError("invalid value '" + value + "', see describe_setting");
    }

    if (c.filament_slot > 0)
        if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(c.value.get()); vec != nullptr && vec->size() != 1)
            throw ToolError("give a single value, for filament slot " + std::to_string(c.filament_slot) + " only");
    if (const std::string err = check_range(*c.def, *c.value); !err.empty())
        throw ToolError(err);
    return c;
}

std::string display_value(std::string value)
{
    constexpr size_t max_len = 60;
    if (value.size() > max_len)
        value = value.substr(0, max_len) + "...";
    return value;
}

std::string target_text(const Change& c)
{
    if (c.object_idx >= 0)
        return " [" + wxGetApp().model().objects[c.object_idx]->name + "]";
    if (c.filament_slot > 0)
        return " [" + _u8L("filament") + " " + std::to_string(c.filament_slot) + ": " + c.filament_preset + "]";
    if (c.toolhead > 0)
        return " [" + _u8L("toolhead") + " " + std::to_string(c.toolhead) + "]";
    return {};
}

// Returns the changes checked by the user, none when the dialog was cancelled.
std::vector<bool> confirm(const std::vector<Change>& changes, const std::string& reason)
{
    std::vector<ProposedChange> proposed;
    for (const Change& c : changes) {
        const std::string label = c.def->full_label.empty() ? c.def->label : c.def->full_label;
        proposed.push_back({ _u8L(label.c_str()) + target_text(c), c.key, display_value(c.old_value),
                             display_value(c.value->serialize()), _u8L(c.def->tooltip.c_str()), c.comment });
    }

    McpChangesDialog dlg(wxGetApp().mainframe, reason, proposed);
    if (dlg.ShowModal() != wxID_APPLY)
        return std::vector<bool>(changes.size(), false);
    return dlg.selected();
}

// Selects the preset of the target slot in the Filament settings, like the edit
// button of that slot in the sidebar.
void select_filament_slot(const Change& c)
{
    GUI::Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
    if (wxGetApp().preset_bundle->filaments.get_edited_preset().name == c.filament_preset)
        return;
    if (!tab->select_preset(c.filament_preset))
        throw ToolError("Could not open the filament preset '" + c.filament_preset + "'");
    tab->get_combo_box()->set_filament_idx(c.filament_slot - 1);
}

void apply(std::vector<Change>& changes)
{
    GUI::Plater&       plater = *wxGetApp().plater();
    DynamicPrintConfig presets_delta;
    std::vector<int>   changed_objects;

    for (Change& c : changes) {
        if (c.filament_slot > 0)
            select_filament_slot(c);
        if (c.object_idx < 0)
            presets_delta.set_key_value(c.key, c.value->clone());
        else if (std::find(changed_objects.begin(), changed_objects.end(), c.object_idx) == changed_objects.end())
            changed_objects.push_back(c.object_idx);
    }

    if (!changed_objects.empty()) {
        // Object overrides are part of the model: make them undoable.
        GUI::Plater::TakeSnapshot snapshot(&plater, "AI assistant changes");
        for (Change& c : changes)
            if (c.object_idx >= 0)
                wxGetApp().model().objects[c.object_idx]->config.set_key_value(c.key, c.value->clone());
        for (int idx : changed_objects) {
            ModelObject* obj = wxGetApp().model().objects[idx];
            plater.changed_object(*obj);
            if (GUI::ObjectList* list = wxGetApp().obj_list())
                list->add_settings_item(list->GetModel()->GetItemById(idx), &obj->config.get());
        }
    }

    // Same path as loading a config file: routes each key to its tab and refreshes the plater.
    if (!presets_delta.empty())
        wxGetApp().mainframe->load_config(presets_delta);
}

json describe_applied(const Change& c)
{
    json entry = { { "key", c.key }, { "old", c.old_value }, { "new", c.value->serialize() } };
    if (!c.comment.empty())
        entry["comment"] = c.comment;
    if (c.object_idx >= 0) {
        entry["object"] = wxGetApp().model().objects[c.object_idx]->name;
        return entry;
    }
    entry["preset_type"] = preset_type_name(c.preset_type);
    if (c.toolhead > 0)
        entry["toolhead"] = c.toolhead;
    if (c.filament_slot > 0) {
        entry["filament_slot"]   = c.filament_slot;
        entry["filament_preset"] = c.filament_preset;
        const std::vector<std::string>& presets = wxGetApp().preset_bundle->filament_presets;
        json                            shared  = json::array();
        for (size_t i = 0; i < presets.size(); ++i)
            if (int(i) + 1 != c.filament_slot && presets[i] == c.filament_preset)
                shared.push_back(i + 1);
        if (!shared.empty())
            entry["also_applies_to_slots"] = std::move(shared);
    }
    return entry;
}

} // namespace

json apply_changes(const json& args, const std::atomic<bool>& abandoned)
{
    require_plater();
    if (!args.contains("changes") || !args["changes"].is_array() || args["changes"].empty())
        throw ToolError("'changes' must be a non-empty array");
    const std::string reason = args.value("reason", std::string());

    std::vector<Change> changes;
    json                rejected  = json::array();
    json                unchanged = json::array();
    for (const json& item : args["changes"]) {
        try {
            Change c = parse_change(item);
            if (c.value->serialize() == c.old_value)
                unchanged.push_back(c.key);
            else
                changes.push_back(std::move(c));
        } catch (const ToolError& e) {
            rejected.push_back({ { "change", item }, { "error", e.what() } });
        }
    }
    // All or nothing: a partially valid proposal is sent back to the model to be fixed.
    if (!rejected.empty())
        throw ToolError("No change applied, invalid changes:\n" + rejected.dump(2));
    if (changes.empty())
        return { { "applied", false }, { "message", "All values are already set" }, { "unchanged", unchanged } };

    // Only one filament preset can be edited at a time in the slicer.
    std::set<std::string> filament_presets;
    for (const Change& c : changes)
        if (c.filament_slot > 0)
            filament_presets.insert(c.filament_preset);
    if (filament_presets.size() > 1)
        throw ToolError("No change applied: change the filament settings of one slot per call, and ask the user to save "
                        "that filament preset before changing another one");

    const std::vector<bool> selected = confirm(changes, reason);
    if (abandoned)
        return { { "applied", false }, { "message", "The request timed out before the user answered" } };

    std::vector<Change> accepted;
    json                declined = json::array();
    for (size_t i = 0; i < changes.size(); ++i)
        if (selected[i])
            accepted.push_back(std::move(changes[i]));
        else
            declined.push_back(describe_applied(changes[i]));
    // Declined values are kept in the journal too: the outcome of the print is judged with them.
    json declined_log = declined;
    for (json& d : declined_log)
        d["declined_by_user"] = true;
    log_applied_changes(declined_log, reason);
    if (accepted.empty())
        return { { "applied", false },
                 { "declined", std::move(declined) },
                 { "message", "The user declined all the changes. Ask them what they want instead." } };

    apply(accepted);

    json applied = json::array();
    for (const Change& c : accepted)
        applied.push_back(describe_applied(c));
    log_applied_changes(applied, reason);
    json result = { { "applied", true },
                    { "changes", std::move(applied) },
                    { "message", "Applied as unsaved preset edits (object overrides can be undone with Ctrl+Z). Filament "
                                 "preset edits are lost when another filament preset is opened unless the user saves them. "
                                 "Call get_issues to check the result." } };
    if (!declined.empty()) {
        result["declined"] = std::move(declined);
        result["message"]  = result["message"].get<std::string>() +
                            " The user unchecked the 'declined' changes: their settings keep the 'old' value. Do not "
                            "propose them again unless asked; take them into account when following the print and "
                            "ask the user why if it matters for the diagnosis.";
    }
    return result;
}

}} // namespace Slic3r::AIAssistant
