#include "McpObjects.hpp"
#include "McpIssues.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Geometry.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

#include <boost/format.hpp>

#include <cmath>
#include <initializer_list>
#include <set>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::from_u8;
using GUI::wxGetApp;

namespace {

constexpr int MAX_COPIES = 50;

double round2(double v) { return std::round(v * 100.) / 100.; }

// Operations that add or remove objects: the indices of the following operations
// can only be checked when they run.
bool changes_object_list(const std::string& action)
{
    return action == "cut" || action == "split" || action == "duplicate" || action == "delete";
}

int resolve_object(const json& ref)
{
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    if (ref.is_number_integer()) {
        const int idx = ref.get<int>();
        if (idx < 1 || idx > int(objects.size()))
            throw ToolError("No object #" + std::to_string(idx) + ", the project has " + std::to_string(objects.size()));
        return idx - 1;
    }
    if (ref.is_string())
        return find_object(ref.get<std::string>());
    throw ToolError("'object' must be an object name or its 1-based index from get_project");
}

double number(const json& op, const char* name)
{
    if (!op.contains(name) || !op[name].is_number())
        throw ToolError(std::string("'") + name + "' must be a number");
    return op[name].get<double>();
}

// {x, y, z} argument, NaN for the missing axes. Throws when no axis is given.
Vec3d axes(const json& op, const char* name, bool with_z = true)
{
    if (!op.contains(name) || !op[name].is_object())
        throw ToolError(std::string("'") + name + "' must be an object such as {\"x\": 10}");
    Vec3d out  = Vec3d::Constant(std::nan(""));
    bool  some = false;
    for (int i = 0; i < (with_z ? 3 : 2); ++i) {
        const char* axis = i == 0 ? "x" : i == 1 ? "y" : "z";
        if (!op[name].contains(axis))
            continue;
        if (!op[name][axis].is_number())
            throw ToolError(std::string("'") + name + "." + axis + "' must be a number");
        out[i] = op[name][axis].get<double>();
        some   = true;
    }
    if (!some)
        throw ToolError(std::string("'") + name + "' gives no axis");
    return out;
}

std::string axes_text(const Vec3d& v, const char* unit)
{
    std::string out;
    for (int i = 0; i < 3; ++i)
        if (!std::isnan(v[i]))
            out += (out.empty() ? "" : ", ") + std::string(i == 0 ? "X " : i == 1 ? "Y " : "Z ") +
                   float_to_string_decimal_point(v[i], 2) + unit;
    return out;
}

Axis axis_arg(const json& op)
{
    const std::string axis = op.value("axis", std::string());
    if (axis == "x")
        return X;
    if (axis == "y")
        return Y;
    if (axis == "z")
        return Z;
    throw ToolError("'axis' must be x, y or z");
}

// ------------------------------------------------------------ parts

struct PartType
{
    const char*     name;
    ModelVolumeType type;
    // Description of the operation adding one: %1% the shape and its size, %2% the object.
    const char*     add_text;
};

const PartType PART_TYPES[] = {
    { "part", ModelVolumeType::MODEL_PART, L("Add a part (%1%) to %2%") },
    { "negative", ModelVolumeType::NEGATIVE_VOLUME, L("Add a negative part (%1%) to %2%") },
    { "modifier", ModelVolumeType::PARAMETER_MODIFIER, L("Add a modifier (%1%) to %2%") },
    { "support_blocker", ModelVolumeType::SUPPORT_BLOCKER, L("Add a support blocker (%1%) to %2%") },
    { "support_enforcer", ModelVolumeType::SUPPORT_ENFORCER, L("Add a support enforcer (%1%) to %2%") },
};

const PartType& part_type(const json& op)
{
    const std::string name = op.value("type", std::string("part"));
    for (const PartType& t : PART_TYPES)
        if (name == t.name)
            return t;
    throw ToolError("'type' must be part, negative, modifier, support_blocker or support_enforcer");
}

const char* part_type_name(ModelVolumeType type)
{
    for (const PartType& t : PART_TYPES)
        if (t.type == type)
            return t.name;
    return "other";
}

// Size of the new shape: box x/y/z; cylinder and cone diameter x and height z; sphere diameter x.
Vec3d shape_size(const json& op)
{
    const std::string shape = op.value("shape", std::string("box"));
    if (shape != "box" && shape != "cylinder" && shape != "cone" && shape != "sphere")
        throw ToolError("'shape' must be box, cylinder, cone or sphere");
    const Vec3d size = axes(op, "size_mm");
    const bool  box  = shape == "box";
    if (std::isnan(size.x()) || (box && std::isnan(size.y())) || ((box || shape != "sphere") && std::isnan(size.z())))
        throw ToolError(box ? "A box needs size_mm x, y and z" :
                        shape == "sphere" ? "A sphere needs its diameter in size_mm.x" :
                                            "A " + shape + " needs its diameter in size_mm.x and its height in size_mm.z");
    const Vec3d out = box ? size : shape == "sphere" ? Vec3d::Constant(size.x()) : Vec3d(size.x(), size.x(), size.z());
    if (out.minCoeff() <= 0.)
        throw ToolError("'size_mm' must be positive");
    return out;
}

int resolve_part(const ModelObject& obj, const json& ref)
{
    if (ref.is_number_integer()) {
        const int idx = ref.get<int>();
        if (idx < 1 || idx > int(obj.volumes.size()))
            throw ToolError("No part #" + std::to_string(idx) + " in '" + obj.name + "', it has " + std::to_string(obj.volumes.size()));
        return idx - 1;
    }
    if (!ref.is_string())
        throw ToolError("'part' must be a part name or its 1-based index from get_project");
    int found = -1;
    for (size_t i = 0; i < obj.volumes.size(); ++i) {
        if (obj.volumes[i]->name != ref.get<std::string>())
            continue;
        if (found >= 0)
            throw ToolError("Several parts are named '" + ref.get<std::string>() + "', use the part index");
        found = int(i);
    }
    if (found < 0)
        throw ToolError("No part named '" + ref.get<std::string>() + "' in '" + obj.name + "'");
    return found;
}

int filament_count() { return int(wxGetApp().plater()->get_extruder_colors_from_plater_config().size()); }

int filament_arg(const json& op)
{
    if (!op.contains("filament") || !op["filament"].is_number_integer())
        throw ToolError("'filament' must be a 1-based filament slot (0: the object's filament, for a part)");
    const int f = op["filament"].get<int>();
    if (f < 0 || f > filament_count())
        throw ToolError("No filament slot " + std::to_string(f) + ", the project has " + std::to_string(filament_count()));
    return f;
}

// Rebuilds the parts of an object in the object list and the 3D view.
void parts_changed(GUI::Plater& plater, int obj_idx)
{
    ModelObject* obj = wxGetApp().model().objects[obj_idx];
    if (GUI::ObjectList* list = wxGetApp().obj_list()) {
        if (obj->volumes.size() > 1)
            list->reorder_volumes_and_get_selection(obj_idx);
        else
            list->add_volumes_to_object_in_list(size_t(obj_idx));
        list->notify_instance_updated(obj_idx);
    }
    plater.changed_object(*obj);
    plater.canvas3D()->update_instance_printable_state_for_object(size_t(obj_idx));
}

void add_part(GUI::Plater& plater, const json& op, int obj_idx)
{
    ModelObject& obj = *wxGetApp().model().objects[obj_idx];
    if (obj.instances.size() != 1)
        throw ToolError("Adding a part needs an object with a single instance");
    const PartType&   type  = part_type(op);
    const std::string shape = op.value("shape", std::string("box"));
    const Vec3d       size  = shape_size(op);

    TriangleMesh mesh = shape == "box"      ? make_cube(size.x(), size.y(), size.z()) :
                        shape == "cylinder" ? make_cylinder(0.5 * size.x(), size.z()) :
                        shape == "cone"     ? make_cone(0.5 * size.x(), size.z()) :
                                              make_sphere(0.5 * size.x(), PI / 90);

    // Center x/y on the plate and bottom z, as in get_project; default: under the object's center.
    const json  geo    = object_geometry(size_t(obj_idx));
    Vec3d       at     = Vec3d(geo["position_mm"][0].get<double>(), geo["position_mm"][1].get<double>(), 0.);
    if (op.contains("at_mm")) {
        const Vec3d given = axes(op, "at_mm");
        for (int i = 0; i < 3; ++i)
            if (!std::isnan(given[i]))
                at[i] = given[i];
    }
    GUI::PartPlateList& plates = plater.get_partplate_list();
    const int           plate  = plates.find_instance_belongs(obj_idx, 0);
    const Vec3d         origin = plate >= 0 ? plates.get_plate(plate)->get_origin() : plates.get_curr_plate()->get_origin();
    const Vec3d         center = Vec3d(origin.x() + at.x(), origin.y() + at.y(), at.z() + 0.5 * size.z());

    // The mesh is centered by add_volume; as the object list does, the part keeps
    // the bed axes whatever the rotation and scale of the object.
    ModelVolume*         volume = obj.add_volume(std::move(mesh), type.type);
    const ModelInstance& inst   = *obj.instances.front();
    const Transform3d    to_bed = inst.get_transformation().get_matrix_no_offset().inverse();
    volume->set_transformation(to_bed);
    volume->set_offset(to_bed * (center - inst.get_offset()));
    volume->name = op.value("name", std::string("Generic-") + shape);

    int filament = op.contains("filament") ? filament_arg(op) : 0;
    if (!op.contains("filament") && type.type == ModelVolumeType::MODEL_PART && obj.config.has("extruder"))
        filament = obj.config.opt_int("extruder");
    volume->config.set_key_value("extruder", new ConfigOptionInt(filament));
    volume->source.is_from_builtin_objects = true;
    save_object_mesh(obj);
    parts_changed(plater, obj_idx);
}

void delete_part(GUI::Plater& plater, const json& op, int obj_idx)
{
    ModelObject& obj = *wxGetApp().model().objects[obj_idx];
    const int    idx = resolve_part(obj, op["part"]);
    const ModelVolume* volume = obj.volumes[idx];
    if (volume->is_model_part() &&
        std::count_if(obj.volumes.begin(), obj.volumes.end(), [](const ModelVolume* v) { return v->is_model_part(); }) == 1)
        throw ToolError("This is the last solid part of the object: delete the object instead");
    if (obj.is_cut() && (volume->is_model_part() || volume->is_negative_volume()))
        throw ToolError("This object comes from a cut: delete its parts in the slicer");

    obj.delete_volume(size_t(idx));
    // As the object list: a single remaining part gives its settings to the object.
    if (obj.volumes.size() == 1 && !obj.volumes.front()->config.empty()) {
        obj.config.apply(obj.volumes.front()->config);
        obj.volumes.front()->config.reset();
    }
    parts_changed(plater, obj_idx);
}

void set_filament(GUI::Plater& plater, const json& op, int obj_idx)
{
    ModelObject& obj      = *wxGetApp().model().objects[obj_idx];
    const int    filament = filament_arg(op);
    if (op.contains("part")) {
        ModelVolume* volume = obj.volumes[resolve_part(obj, op["part"])];
        if (volume->type() != ModelVolumeType::MODEL_PART && volume->type() != ModelVolumeType::PARAMETER_MODIFIER)
            throw ToolError("Only parts and modifiers have a filament");
        volume->config.set_key_value("extruder", new ConfigOptionInt(filament));
    } else {
        if (filament == 0)
            throw ToolError("An object needs a filament slot from 1");
        obj.config.set_key_value("extruder", new ConfigOptionInt(filament));
    }
    if (GUI::ObjectList* list = wxGetApp().obj_list())
        list->update_filament_values_for_items(size_t(filament_count()));
    plater.changed_object(obj);
    plater.notify_filament_usage_changed();
}

void rename(const json& op, int obj_idx)
{
    ModelObject&      obj  = *wxGetApp().model().objects[obj_idx];
    const std::string name = op.value("name", std::string());
    if (name.empty())
        throw ToolError("'name' must give the new name");
    GUI::ObjectList* list = wxGetApp().obj_list();
    if (op.contains("part")) {
        const int idx            = resolve_part(obj, op["part"]);
        obj.volumes[idx]->name = name;
        if (list != nullptr)
            if (const wxDataViewItem item = list->GetModel()->GetItemByVolumeId(obj_idx, idx))
                list->GetModel()->SetName(from_u8(name), item);
    } else {
        obj.name = name;
        if (list != nullptr)
            if (const wxDataViewItem item = list->GetModel()->GetItemById(obj_idx))
                list->GetModel()->SetName(from_u8(name), item);
    }
}

// A translated description with its arguments put in (%1%, %2%...): whole sentences, so
// that each language can order the words its own way.
template<typename... Args> std::string text_of(const std::string& format, const Args&... args)
{
    boost::format f(format);
    (void) std::initializer_list<int>{ ((f % args), 0)... };
    return f.str();
}

// Checks the arguments of one operation and returns its description for the
// user. `obj_idx` is the object it applies to when already known, otherwise -1.
std::string describe(const json& op, int obj_idx)
{
    const std::string action = op.value("action", std::string());
    if (action == "arrange")
        return _u8L("Arrange the objects of the current plate");

    if (!op.contains("object"))
        throw ToolError("'" + action + "' needs 'object'");
    const std::string name = "'" + (obj_idx >= 0 ? wxGetApp().model().objects[obj_idx]->name : op["object"].dump()) + "'";

    if (action == "scale") {
        if (op.contains("factor")) {
            const double f = number(op, "factor");
            if (f <= 0.)
                throw ToolError("'factor' must be positive");
            return text_of(_u8L("Scale %1% to %2% %%"), name, float_to_string_decimal_point(f * 100., 1));
        }
        const Vec3d size = axes(op, "size_mm");
        for (int i = 0; i < 3; ++i)
            if (!std::isnan(size[i]) && size[i] <= 0.)
                throw ToolError("'size_mm' must be positive");
        return text_of(op.value("keep_proportions", true) ? _u8L("Resize %1% to %2%, keeping its proportions")
                                                          : _u8L("Resize %1% to %2%"),
                       name, axes_text(size, " mm"));
    }
    if (action == "rotate")
        return text_of(_u8L("Rotate %1% by %2%"), name, axes_text(axes(op, "degrees"), "°"));
    if (action == "move") {
        if (op.contains("to_mm"))
            return text_of(_u8L("Move %1% to %2%"), name, axes_text(axes(op, "to_mm", false), " mm"));
        return text_of(_u8L("Move %1% by %2%"), name, axes_text(axes(op, "by_mm", false), " mm"));
    }
    if (action == "center")
        return text_of(_u8L("Center %1% on its plate"), name);
    if (action == "drop")
        return text_of(_u8L("Drop %1% to the bed"), name);
    if (action == "mirror") {
        const Axis axis = axis_arg(op);
        return text_of(_u8L("Mirror %1% along %2%"), name, std::string(axis == X ? "X" : axis == Y ? "Y" : "Z"));
    }
    if (action == "cut") {
        const double h = number(op, "height_mm");
        if (h <= 0.)
            throw ToolError("'height_mm' must be positive");
        const std::string keep = op.value("keep", std::string("both"));
        if (keep != "both" && keep != "upper" && keep != "lower")
            throw ToolError("'keep' must be both, upper or lower");
        if (obj_idx >= 0 && wxGetApp().model().objects[obj_idx]->instances.size() != 1)
            throw ToolError("Cut needs an object with a single instance");
        std::string text = text_of(_u8L("Cut %1% at %2% mm from its bottom"), name, float_to_string_decimal_point(h, 2));
        if (keep == "upper")
            text += ", " + _u8L("keep the upper part");
        else if (keep == "lower")
            text += ", " + _u8L("keep the lower part");
        if (keep != "lower" && op.value("place_upper_on_cut", true))
            text += ", " + _u8L("upper part placed on its cut face");
        return text;
    }
    if (action == "split") {
        if (obj_idx >= 0) {
            const ModelObject* obj = wxGetApp().model().objects[obj_idx];
            if (obj->volumes.size() == 1 && !obj->volumes.front()->is_splittable())
                throw ToolError(name + " is a single part and cannot be split");
        }
        return text_of(_u8L("Split %1% into objects"), name);
    }
    if (action == "duplicate") {
        const int copies = op.value("copies", 1);
        if (copies < 1 || copies > MAX_COPIES)
            throw ToolError("'copies' must be between 1 and " + std::to_string(MAX_COPIES));
        return text_of(_u8L("Duplicate %1% x%2%"), name, copies);
    }
    if (action == "delete")
        return text_of(_u8L("Delete %1%"), name);
    if (action == "orient")
        return text_of(_u8L("Auto-orient %1%"), name);
    if (action == "add_part") {
        const PartType& type = part_type(op);
        const Vec3d     size = shape_size(op);
        if (op.contains("filament"))
            filament_arg(op);
        const std::string shape = op.value("shape", std::string("box")) + " " + float_to_string_decimal_point(size.x(), 1) +
                                  " x " + float_to_string_decimal_point(size.y(), 1) + " x " +
                                  float_to_string_decimal_point(size.z(), 1) + " mm";
        std::string text = text_of(_u8L(type.add_text), shape, name);
        if (op.contains("at_mm"))
            text += ", " + text_of(_u8L("at %1%"), axes_text(axes(op, "at_mm"), " mm"));
        return text;
    }
    // The operations below apply to the whole object, or to one of its parts.
    const bool        on_part = op.contains("part");
    const std::string part    = on_part ? op["part"].dump() : std::string();
    if (action == "delete_part") {
        if (!on_part)
            throw ToolError("'delete_part' needs 'part'");
        return text_of(_u8L("Delete part %1% of %2%"), part, name);
    }
    if (action == "set_filament") {
        const int f = filament_arg(op);
        return on_part ? text_of(_u8L("Print part %1% of %2% with filament %3%"), part, name, f)
                       : text_of(_u8L("Print %1% with filament %2%"), name, f);
    }
    if (action == "rename") {
        if (op.value("name", std::string()).empty())
            throw ToolError("'rename' needs 'name'");
        const std::string new_name = "'" + op["name"].get<std::string>() + "'";
        return on_part ? text_of(_u8L("Rename part %1% of %2% to %3%"), part, name, new_name)
                       : text_of(_u8L("Rename %1% to %2%"), name, new_name);
    }
    throw ToolError("Unknown action '" + action + "'");
}


bool confirm(const std::string& reason, const std::vector<std::string>& lines)
{
    std::string text = reason.empty() ? std::string() : reason + "\n\n";
    for (const std::string& line : lines)
        text += "- " + line + "\n";
    GUI::MessageDialog dlg(wxGetApp().mainframe,
                           from_u8(text) + "\n" +
                               _L("The AI assistant asks to change the objects of the project. Apply these operations? "
                                  "Undo restores the previous state."),
                           _L("AI Assistant"), wxYES_NO | wxICON_QUESTION);
    return dlg.ShowModal() == wxID_YES;
}

// Before an operation that removes or replaces objects or parts: a selection
// left on them makes the object list and the 3D view read deleted volumes
// while the scene is rebuilt (crash when drawing the selection).
void clear_selection(GUI::Plater& plater)
{
    if (GUI::ObjectList* list = wxGetApp().obj_list())
        list->unselect_objects();
    plater.canvas3D()->get_selection().remove_all();
}

GUI::Selection& select_object(GUI::Plater& plater, int obj_idx)
{
    GUI::Selection& sel = plater.canvas3D()->get_selection();
    sel.set_mode(GUI::Selection::Instance);
    sel.add_object(unsigned(obj_idx), true);
    sel.setup_cache();
    return sel;
}

void scale(GUI::Plater& plater, const json& op, int obj_idx)
{
    GUI::Selection& sel  = select_object(plater, obj_idx);
    const Vec3d     size = sel.get_bounding_box().size();
    Vec3d           factor = Vec3d::Ones();
    if (op.contains("factor")) {
        factor = Vec3d::Constant(op["factor"].get<double>());
    } else {
        const Vec3d target       = axes(op, "size_mm");
        const bool  proportional = op.value("keep_proportions", true);
        int         given        = 0;
        for (int i = 0; i < 3; ++i) {
            if (std::isnan(target[i]))
                continue;
            if (size[i] <= 0.)
                throw ToolError("The object is flat along this axis");
            factor[i] = target[i] / size[i];
            if (proportional)
                factor = Vec3d::Constant(factor[i]);
            ++given;
        }
        if (proportional && given > 1)
            throw ToolError("With keep_proportions, give a single dimension in 'size_mm'");
    }
    sel.scale(factor, GUI::TransformationType(GUI::TransformationType::World_Relative_Joint));
    plater.canvas3D()->do_scale("");
}

void rotate(GUI::Plater& plater, const json& op, int obj_idx)
{
    const Vec3d degrees = axes(op, "degrees");
    GUI::Selection& sel = select_object(plater, obj_idx);
    for (int i = 0; i < 3; ++i) {
        if (std::isnan(degrees[i]) || degrees[i] == 0.)
            continue;
        Vec3d rotation  = Vec3d::Zero();
        rotation[i]     = degrees[i] * M_PI / 180.;
        sel.setup_cache();
        sel.rotate(rotation, GUI::TransformationType(GUI::TransformationType::World_Relative_Joint));
    }
    // Without a snapshot name the object is put back on the bed.
    plater.canvas3D()->do_rotate("");
}

void move(GUI::Plater& plater, const json& op, int obj_idx)
{
    GUI::Selection& sel          = select_object(plater, obj_idx);
    Vec3d           displacement = Vec3d::Zero();
    if (op.contains("to_mm")) {
        const Vec3d   target = axes(op, "to_mm", false);
        const json    geo    = object_geometry(size_t(obj_idx));
        for (int i = 0; i < 2; ++i)
            if (!std::isnan(target[i]))
                displacement[i] = target[i] - geo["position_mm"][i].get<double>();
    } else {
        const Vec3d by = axes(op, "by_mm", false);
        for (int i = 0; i < 2; ++i)
            if (!std::isnan(by[i]))
                displacement[i] = by[i];
    }
    sel.translate(displacement, GUI::TransformationType(GUI::TransformationType::World_Relative_Joint));
    plater.canvas3D()->do_move("");
}

void cut(GUI::Plater& plater, const json& op, int obj_idx)
{
    const ModelObject* obj = wxGetApp().model().objects[obj_idx];
    if (obj->instances.size() != 1)
        throw ToolError("Cut needs an object with a single instance");
    if (!obj->cut_id.id().invalid())
        throw ToolError("This object comes from a cut with connectors: cut it with the Cut tool of the slicer");

    const BoundingBoxf3 box    = obj->instance_bounding_box(0);
    const double        height = op["height_mm"].get<double>();
    if (height >= box.size().z())
        throw ToolError("'height_mm' must be below the object height (" + float_to_string_decimal_point(box.size().z(), 2) + " mm)");

    const std::string        keep = op.value("keep", std::string("both"));
    ModelObjectCutAttributes attributes = ModelObjectCutAttribute::InvalidateCutInfo;
    if (keep != "lower") {
        attributes = attributes | ModelObjectCutAttribute::KeepUpper;
        if (op.value("place_upper_on_cut", true))
            attributes = attributes | ModelObjectCutAttribute::PlaceOnCutUpper;
    }
    if (keep != "upper")
        attributes = attributes | ModelObjectCutAttribute::KeepLower;

    // As Plater::cut_horizontal(), which is not public.
    wxBusyCursor wait;
    const Vec3d offset = obj->instances.front()->get_offset();
    Cut         cut(obj, 0, Geometry::translation_transform((box.min.z() + height) * Vec3d::UnitZ() - offset), attributes);
    plater.apply_cut_object_to_model(size_t(obj_idx), cut.perform_with_plane());
}

// Starts orient or arrange; the caller waits for the job with ToolRetry.
void start_job(GUI::Plater& plater, const std::string& action, int obj_idx)
{
    if (action == "orient") {
        select_object(plater, obj_idx);
        plater.set_prepare_state(GUI::Job::PREPARE_STATE_DEFAULT);
        plater.orient();
    } else {
        plater.canvas3D()->get_selection().remove_all();
        // Current plate only, like the Arrange button of the plate.
        plater.set_prepare_state(GUI::Job::PREPARE_STATE_MENU);
        plater.arrange();
    }
}

// Runs one operation; returns true when it started a background job.
bool run(GUI::Plater& plater, const json& op)
{
    const std::string action = op.value("action", std::string());
    describe(op, -1);
    const int obj_idx = action == "arrange" ? -1 : resolve_object(op["object"]);
    // Checks that depend on the object (single part, instances).
    describe(op, obj_idx);

    if (action == "scale")
        scale(plater, op, obj_idx);
    else if (action == "rotate")
        rotate(plater, op, obj_idx);
    else if (action == "move")
        move(plater, op, obj_idx);
    else if (action == "center") {
        select_object(plater, obj_idx);
        plater.center_selection();
    } else if (action == "drop") {
        select_object(plater, obj_idx);
        plater.drop_selection();
    } else if (action == "mirror") {
        select_object(plater, obj_idx);
        plater.mirror(axis_arg(op));
    } else if (action == "cut") {
        clear_selection(plater);
        cut(plater, op, obj_idx);
    }
    else if (action == "split") {
        select_object(plater, obj_idx);
        plater.split_object();
    } else if (action == "duplicate")
        select_object(plater, obj_idx).clone(op.value("copies", 1));
    else if (action == "delete") {
        clear_selection(plater);
        plater.remove(size_t(obj_idx));
    } else if (action == "add_part") {
        clear_selection(plater);
        add_part(plater, op, obj_idx);
    } else if (action == "delete_part") {
        clear_selection(plater);
        delete_part(plater, op, obj_idx);
    }
    else if (action == "set_filament")
        set_filament(plater, op, obj_idx);
    else if (action == "rename")
        rename(op, obj_idx);
    else {
        start_job(plater, action, obj_idx);
        return true;
    }
    return false;
}

json objects_summary()
{
    json                   out     = json::array();
    const ModelObjectPtrs& objects = wxGetApp().model().objects;
    for (size_t i = 0; i < objects.size(); ++i) {
        json o = { { "index", i + 1 }, { "name", objects[i]->name } };
        o.update(object_geometry(i));
        if (objects[i]->volumes.size() > 1)
            o["parts"] = object_parts(*objects[i]);
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace

json object_parts(const ModelObject& obj)
{
    json out = json::array();
    for (size_t i = 0; i < obj.volumes.size(); ++i) {
        const ModelVolume* v = obj.volumes[i];
        json part = { { "index", i + 1 }, { "name", v->name }, { "type", part_type_name(v->type()) } };
        if (v->config.has("extruder"))
            part["filament"] = v->config.opt_int("extruder");
        out.push_back(std::move(part));
    }
    return out;
}

json object_geometry(size_t obj_idx)
{
    const ModelObject* obj = wxGetApp().model().objects[obj_idx];
    if (obj->instances.empty())
        return json::object();
    const BoundingBoxf3 box = obj->instance_bounding_box(0);
    const Vec3d         size = box.size();
    const Vec3d         center = box.center();

    GUI::PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    const int           plate  = plates.find_instance_belongs(int(obj_idx), 0);
    const Vec3d origin = plate >= 0 ? plates.get_plate(plate)->get_origin() : plates.get_curr_plate()->get_origin();

    json out = { { "size_mm", { round2(size.x()), round2(size.y()), round2(size.z()) } },
                 { "position_mm", { round2(center.x() - origin.x()), round2(center.y() - origin.y()), round2(box.min.z()) } } };
    out["plate"] = plate >= 0 ? json(plate + 1) : json("outside");
    return out;
}

json edit_objects(const json& args, const std::atomic<bool>& abandoned, json& state)
{
    GUI::Plater& plater = require_plater();
    if (!plater.get_ui_job_worker().is_idle())
        throw ToolRetry();

    if (!state.value("confirmed", false)) {
        if (!args.contains("operations") || !args["operations"].is_array() || args["operations"].empty())
            throw ToolError("'operations' must list at least one operation");
        if (plater.canvas3D()->get_gizmos_manager().get_current_type() != GUI::GLGizmosManager::Undefined)
            throw ToolError("A tool (cut, paint...) is open in the 3D view: ask the user to close it first");

        std::vector<std::string> lines;
        bool                     list_changed = false;
        for (const json& op : args["operations"]) {
            if (!op.is_object())
                throw ToolError("Each operation must be an object");
            const std::string action  = op.value("action", std::string());
            const int obj_idx = action == "arrange" || list_changed || !op.contains("object") ? -1 : resolve_object(op["object"]);
            lines.push_back(describe(op, obj_idx));
            list_changed = list_changed || changes_object_list(action);
        }

        if (!confirm(args.value("reason", std::string()), lines) || abandoned)
            return { { "applied", false }, { "message", "The user declined the operations" } };
        state = { { "confirmed", true }, { "next", 0 }, { "done", json::array() } };
        // The result is shown in the Prepare view.
        plater.select_view_3D("3D");
    }

    const json& ops  = args["operations"];
    size_t      next = state["next"].get<size_t>();
    {
        // One undo step for the operations run in this call.
        GUI::Plater::TakeSnapshot snapshot(&plater, "AI assistant: edit objects");
        while (next < ops.size()) {
            if (abandoned)
                break;
            const json& op = ops[next];
            bool        started_job;
            try {
                started_job = run(plater, op);
            } catch (const ToolError& e) {
                return { { "applied", !state["done"].empty() },
                         { "done", state["done"] },
                         { "failed", { { "operation", op }, { "error", e.what() } } },
                         { "not_run", ops.size() - next - 1 },
                         { "objects", objects_summary() } };
            }
            state["done"].push_back(op.value("action", std::string()) +
                                    (op.contains("object") ? " " + op["object"].dump() : std::string()));
            state["next"] = ++next;
            if (started_job)
                // Arrange and orient run in the background: resume once they end.
                throw ToolRetry();
        }
    }

    json out = { { "applied", true }, { "done", state["done"] }, { "objects", objects_summary() } };
    try {
        out["issues"] = collect_issues()["summary"].get<std::string>() + " (details: get_issues)";
    } catch (const ToolRetry&) {
        out["issues"] = "unknown, the slicer is updating (call get_issues)";
    }
    return out;
}

}} // namespace Slic3r::AIAssistant
