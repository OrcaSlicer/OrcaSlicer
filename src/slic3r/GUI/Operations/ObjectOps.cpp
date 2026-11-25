#include "ObjectOps.hpp"

#include "Outcome.hpp"
#include "PlateOps.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <wx/dataview.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

Plater& the_plater()
{
    assert(wxThread::IsMain());
    return *wxGetApp().plater();
}

// The objects `targets` names or names something of, each once, by index.
std::set<int> objects_of(const Targets& targets)
{
    std::set<int> out(targets.objects.begin(), targets.objects.end());
    for (const auto& [object, instance] : targets.instances)
        out.insert(object);
    for (const auto& [object, part] : targets.parts)
        out.insert(object);
    return out;
}

// What ObjectList::del_subobject_from_object() asks before it deletes a solid or negative part of a cut object, and
// Plater's delete_object_from_model() before it deletes a cut object; only `targets` as deletion() gives them.
std::optional<Refusal> cut_refusal(const Model& model, const Targets& doomed)
{
    for (int object : doomed.objects)
        if (model.objects[std::size_t(object)]->is_cut())
            return Refusal{RefusalKind::Precondition, "cut_object",
                           L("You are trying to delete an object which is a part of a cut object.\nThis action will break a cut "
                             "correspondence.\nAfter that, model consistency can't be guaranteed.")};
    for (const auto& [object, part] : doomed.parts) {
        const ModelObject& owner  = *model.objects[std::size_t(object)];
        const ModelVolume& volume = *owner.volumes[std::size_t(part)];
        if (owner.is_cut() && (volume.is_model_part() || volume.is_negative_volume()))
            return Refusal{RefusalKind::Precondition, "cut_object",
                           L("This action will break a cut correspondence.\nAfter that, model consistency can't be guaranteed.\n\nTo "
                             "manipulate with solid parts or negative volumes you have to invalidate cut information first.")};
    }
    return std::nullopt;
}

// An object left without a solid part by the parts of `doomed` (deletion()), which the object list refuses.
std::optional<Refusal> last_solid_part_refusal(const Model& model, const Targets& doomed)
{
    std::map<int, std::set<std::size_t>> named;
    for (const auto& [object, part] : doomed.parts)
        named[object].insert(std::size_t(part));
    for (const auto& [object, parts] : named) {
        const ModelObject& owner = *model.objects[std::size_t(object)];
        bool               kept = false, deleted = false;
        for (std::size_t v = 0; v < owner.volumes.size(); ++v)
            if (owner.volumes[v]->is_model_part())
                (parts.count(v) != 0 ? deleted : kept) = true;
        if (deleted && !kept)
            return Refusal{RefusalKind::Precondition, "last_solid_part", L("Deleting the last solid part is not allowed.")};
    }
    return std::nullopt;
}

Refusal part_not_listed()
{
    return {RefusalKind::Precondition, "part_not_listed", L("The object list does not show this part, so it cannot be renamed.")};
}

// Whether `targets` names a cut object or something of one.
bool names_cut_object(const Model& model, const Targets& targets)
{
    const std::set<int> objects = objects_of(targets);
    return std::any_of(objects.begin(), objects.end(), [&model](int object) { return model.objects[std::size_t(object)]->is_cut(); });
}

// The refusal of a change to the meshes while a gizmo is open, which keeps references to them and undo steps of its own,
// in the words the GUI's check (GLGizmosManager::check_gizmos_closed_except()) shows in a notification; none when no gizmo
// is open.
std::optional<Refusal> gizmo_open(Plater& plater)
{
    if (plater.get_view3D_canvas3D()->get_gizmos_manager().get_current_type() == GLGizmosManager::Undefined)
        return std::nullopt;
    return Refusal{RefusalKind::Precondition, "gizmo_open", L("Error: Please close all toolbar menus first")};
}

// The refusal of a file that loads as more than one object or part, which Replace 3D file tells in a message box.
Refusal several_parts_loaded()
{
    return {RefusalKind::Precondition, "several_parts", L("Unable to replace with more than one volume")};
}

} // namespace

void select_targets(const Targets& targets)
{
    GLCanvas3D& canvas = shown_prepare_canvas();
    Selection&  sel    = canvas.get_selection();
    bool        first  = true;
    for (int object : targets.objects) {
        sel.add_object(unsigned(object), first);
        first = false;
    }
    for (const auto& [object, instance] : targets.instances) {
        sel.add_instance(unsigned(object), unsigned(instance), first);
        first = false;
    }
    for (const auto& [object, part] : targets.parts) {
        sel.add_volume(unsigned(object), unsigned(part), 0, first);
        first = false;
    }
    wxGetApp().obj_list()->update_selections();
    canvas.set_as_dirty();
}

namespace {

// One instance, or the parts of one object, at a time: what Mirror takes of the targets, for an object every instance
// of it, in the order named.
std::vector<Targets> mirror_turns(const Model& model, const Targets& targets)
{
    std::vector<Targets> out;
    const auto instance = [&out](int object, int index) {
        Targets one;
        one.instances = {{object, index}};
        out.push_back(std::move(one));
    };
    for (int object : targets.objects)
        for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
            instance(object, int(i));
    for (const auto& [object, index] : targets.instances)
        instance(object, index);
    std::map<int, Targets> parts;
    for (const auto& part : targets.parts)
        parts[part.first].parts.push_back(part);
    for (auto& [object, of_object] : parts)
        out.push_back(std::move(of_object));
    return out;
}

} // namespace

Refusal nothing_selected()
{
    return {RefusalKind::Precondition, "nothing_selected", L("Nothing is selected: name the objects, or select them first.")};
}

Refusal wrong_selection()
{
    return {RefusalKind::Precondition, "wrong_selection", L("This does not act on what is selected: name its targets, or select them first.")};
}

Refusal one_object_only()
{
    return {RefusalKind::Precondition, "one_object_only", L("This acts on one object or part at a time: name one, or select only one.")};
}

Refusal gizmo_keeps_selection()
{
    return {RefusalKind::Precondition, "gizmo_open", L("Close the open gizmo first: while it is open, Select All selects nothing.")};
}

Targets deletion(const Model& model, const Targets& targets)
{
    std::set<int>                whole(targets.objects.begin(), targets.objects.end());
    std::map<int, std::set<int>> instances, parts;
    for (const auto& [object, instance] : targets.instances)
        instances[object].insert(instance);
    for (const auto& [object, part] : targets.parts)
        parts[object].insert(part);
    for (const auto& [object, named] : instances)
        if (named.size() == model.objects[std::size_t(object)]->instances.size())
            whole.insert(object);
    for (const auto& [object, named] : parts)
        if (named.size() == model.objects[std::size_t(object)]->volumes.size())
            whole.insert(object);

    Targets out;
    out.from_selection = targets.from_selection;
    out.objects.assign(whole.begin(), whole.end());
    for (const auto& instance : targets.instances)
        if (whole.count(instance.first) == 0)
            out.instances.push_back(instance);
    for (const auto& part : targets.parts)
        if (whole.count(part.first) == 0)
            out.parts.push_back(part);
    return out;
}

std::optional<Refusal> delete_refusal(const Model& model, const Targets& targets)
{
    const Targets doomed = deletion(model, targets);
    if (std::optional<Refusal> refusal = cut_refusal(model, doomed))
        return refusal;
    return last_solid_part_refusal(model, doomed);
}

TargetIds target_ids(const Model& model, const Targets& targets)
{
    TargetIds out;
    for (int object : targets.objects)
        out.objects.push_back(model.objects[std::size_t(object)]->id());
    for (const auto& [object, instance] : targets.instances)
        out.instances.push_back(model.objects[std::size_t(object)]->instances[std::size_t(instance)]->id());
    for (const auto& [object, part] : targets.parts)
        out.parts.push_back(model.objects[std::size_t(object)]->volumes[std::size_t(part)]->id());
    return out;
}

DeletedTargets deleted_targets(const Targets& doomed, const TargetIds& before, const Model& after)
{
    // Where `after` holds the object, or the instance or part of `items`, of identity `id`: its index, or [object, index].
    const auto object_at = [&after](ObjectID id) -> std::optional<int> {
        for (std::size_t o = 0; o < after.objects.size(); ++o)
            if (after.objects[o]->id() == id)
                return int(o);
        return std::nullopt;
    };
    const auto item_at = [&after](ObjectID id, const auto& items) -> std::optional<std::pair<int, int>> {
        for (std::size_t o = 0; o < after.objects.size(); ++o) {
            const auto& of_object = items(*after.objects[o]);
            for (std::size_t i = 0; i < of_object.size(); ++i)
                if (of_object[i]->id() == id)
                    return std::pair(int(o), int(i));
        }
        return std::nullopt;
    };
    const auto instances = [](const ModelObject& object) -> const ModelInstancePtrs& { return object.instances; };
    const auto volumes   = [](const ModelObject& object) -> const ModelVolumePtrs& { return object.volumes; };

    DeletedTargets out;
    out.deleted.from_selection = out.kept.from_selection = doomed.from_selection;
    for (std::size_t i = 0; i < doomed.objects.size(); ++i)
        if (const std::optional<int> now = object_at(before.objects[i]))
            out.kept.objects.push_back(*now);
        else
            out.deleted.objects.push_back(doomed.objects[i]);
    for (std::size_t i = 0; i < doomed.instances.size(); ++i)
        if (const auto now = item_at(before.instances[i], instances))
            out.kept.instances.push_back(*now);
        else
            out.deleted.instances.push_back(doomed.instances[i]);
    for (std::size_t i = 0; i < doomed.parts.size(); ++i)
        if (const auto now = item_at(before.parts[i], volumes))
            out.kept.parts.push_back(*now);
        else
            out.deleted.parts.push_back(doomed.parts[i]);
    return out;
}

Outcome<DeletedTargets> delete_objects(const DeleteObjectsArgs& args)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (args.all) {
        if (model.objects.empty())
            return Refusal{RefusalKind::Precondition, "no_objects", L("There are no objects to delete.")};
        DeletedTargets every;
        for (std::size_t i = 0; i < model.objects.size(); ++i)
            every.deleted.objects.push_back(int(i));
        plater.delete_all_objects_from_model();
        return every;
    }

    const Targets doomed = deletion(model, args.targets);
    if (args.checks == DeleteChecks::Refuse)
        if (std::optional<Refusal> refusal = delete_refusal(model, doomed))
            return *refusal;
    // In index order: the object list deletes them from the last, so no deletion moves an index still to come.
    std::vector<ItemForDelete> items;
    for (int object : doomed.objects)
        items.emplace_back(itObject, object, 0);
    for (const auto& [object, instance] : doomed.instances)
        items.emplace_back(itInstance, object, instance);
    for (const auto& [object, part] : doomed.parts)
        items.emplace_back(itVolume, object, part);
    std::sort(items.begin(), items.end());

    const TargetIds      before = target_ids(model, doomed);
    Plater::TakeSnapshot snapshot(&plater, "Delete Selected Objects");
    plater.get_ui_job_worker().cancel_all();
    ObjectList& list = *wxGetApp().obj_list();
    // One object or one instance as Selection::erase() deletes a selection of one, the rest as it deletes several.
    if (items.size() == 1 && items.front().type != itVolume)
        list.delete_from_model_and_list(items.front().type, items.front().obj_idx, items.front().sub_obj_idx);
    else
        list.delete_from_model_and_list(items);
    // The object list stops at an item it refuses (an object's last solid part) or asks about (a cut object, whose
    // question cancelled or answered by invalidating the cut deletes nothing), having deleted the items after it.
    DeletedTargets done = deleted_targets(doomed, before, model);
    if (done.deleted.objects.empty() && done.deleted.instances.empty() && done.deleted.parts.empty())
        return nothing_deleted();
    return done;
}

Refusal nothing_deleted() { return {RefusalKind::Failed, "nothing_deleted", L("Nothing was deleted.")}; }

std::optional<Refusal> rename_refusal(const std::string& name)
{
    if (name.empty())
        return Refusal{RefusalKind::Precondition, "empty_name", L("The name cannot be empty.")};
    if (Plater::has_illegal_filename_characters(name))
        // TRN %1% is the characters a name cannot hold.
        return Refusal{RefusalKind::Precondition, "invalid_name", L("Invalid name, the following characters are not allowed: %1%"),
                       {"<>:/\\|?*\""}};
    return std::nullopt;
}

Refusal rename_wrong_selection() { return {RefusalKind::Precondition, "wrong_selection", L("Select one object or part first.")}; }

std::optional<Refusal> unlisted_part_refusal(const Model& model, int object, int part)
{
    const ModelObject& owner = *model.objects[std::size_t(object)];
    if (part >= 0 && owner.is_cut() && owner.volumes[std::size_t(part)]->is_cut_connector())
        return part_not_listed();
    return std::nullopt;
}

Outcome<std::string> rename_object(const RenameObjectArgs& args)
{
    if (std::optional<Refusal> refusal = rename_refusal(args.name))
        return *refusal;
    const Model& model = the_plater().model();
    if (std::optional<Refusal> refusal = unlisted_part_refusal(model, args.object, args.part))
        return *refusal;
    const ModelObject& object = *model.objects[std::size_t(args.object)];
    ObjectList&        list   = *wxGetApp().obj_list();
    // A part's row is by its place among the rows, which leave a cut's connectors out.
    const wxDataViewItem item = args.part < 0 ?
                                    list.GetModel()->GetItemById(args.object) :
                                    list.GetModel()->GetItemByVolumeId(args.object,
                                                                       list.GetModel()->get_real_volume_index_in_ui(args.object, args.part));
    if (!item.IsOk())
        return part_not_listed();
    std::string before = args.part < 0 ? object.name : object.volumes[std::size_t(args.part)]->name;
    list.rename_item(item, from_u8(args.name));
    return before;
}

std::optional<Refusal> separate_refusal(const Model& model, const Targets& targets)
{
    if (!targets.parts.empty())
        return wrong_selection();
    const std::set<int> objects = objects_of(targets);
    if (objects.empty())
        return nothing_selected();
    if (objects.size() > 1)
        return one_object_only();
    const ModelObject& object = *model.objects[std::size_t(*objects.begin())];
    for (const auto& [owner, instance] : targets.instances)
        if (instance < 0 || std::size_t(instance) >= object.instances.size())
            // TRN %1% is an object's name, %2% an instance's index.
            return Refusal{RefusalKind::Precondition, "no_such_instance", L("\"%1%\" has no instance %2%."), {object.name, std::to_string(instance)}};
    if (object.instances.size() == 1)
        // TRN %1% is an object's name.
        return Refusal{RefusalKind::Precondition, "one_instance", L("\"%1%\" has one instance: there are no instances to separate."), {object.name}};
    return std::nullopt;
}

Outcome<Targets> separate_instances(const Targets& targets)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (std::optional<Refusal> refusal = separate_refusal(model, targets))
        return *refusal;
    const int     object = *objects_of(targets).begin();
    std::set<int> instances;
    for (const auto& [owner, instance] : targets.instances)
        instances.insert(instance);
    const bool        every  = !targets.objects.empty() || instances.size() == model.objects[std::size_t(object)]->instances.size();
    const std::size_t before = model.objects.size();

    Plater::TakeSnapshot snapshot(&plater, _u8L("Instances to Separated Objects"));
    ObjectList&          list = *wxGetApp().obj_list();
    if (every)
        list.instances_to_separated_objects(object);
    else
        list.instances_to_separated_object(object, instances);

    Targets holders;
    if (every)
        holders.objects.push_back(object);
    for (std::size_t i = before; i < model.objects.size(); ++i)
        holders.objects.push_back(int(i));
    return holders;
}

std::optional<Refusal> split_refusal(const Model& model, const Targets& targets)
{
    const auto one_shell = [](const std::string& name, const ModelVolume& mesh) -> std::optional<Refusal> {
        if (mesh.is_splittable())
            return std::nullopt;
        // TRN %1% is an object's or a part's name.
        return Refusal{RefusalKind::Precondition, "one_shell", L("\"%1%\" is one piece: its mesh has one shell, so it cannot be split."),
                       {name}};
    };
    // An object of several parts splits into objects by its parts.
    for (int object : targets.objects)
        if (const ModelObject& owner = *model.objects[std::size_t(object)]; owner.volumes.size() == 1)
            if (std::optional<Refusal> refusal = one_shell(owner.name, *owner.volumes.front()))
                return refusal;
    for (const auto& [object, part] : targets.parts) {
        const ModelVolume& volume = *model.objects[std::size_t(object)]->volumes[std::size_t(part)];
        if (std::optional<Refusal> refusal = one_shell(volume.name, volume))
            return refusal;
    }
    return std::nullopt;
}

Outcome<Targets> split_object(const SplitObjectArgs& args)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (std::optional<Refusal> refusal = split_refusal(model, args.target))
        return *refusal;
    const int object = args.target.objects.empty() ? args.target.parts.front().first : args.target.objects.front();
    if (args.to_objects) {
        const std::size_t before = model.objects.size();
        switch (plater.split_object(object, args.prompts, args.keep_z)) {
        case Plater::SplitResult::OnePiece:
            return Refusal{RefusalKind::Precondition, "one_piece", L("The selected object couldn't be split.")};
        case Plater::SplitResult::Unanswered:
            return Refusal{RefusalKind::Precondition, "auto_drop_question",
                           L("Object with floating parts was detected: disable Auto-Drop to preserve Z positioning?")};
        case Plater::SplitResult::Split: break;
        }
        // The object is removed, and its pieces are added last.
        Targets pieces;
        for (std::size_t i = before - 1; i < model.objects.size(); ++i)
            pieces.objects.push_back(int(i));
        return pieces;
    }

    const ModelObject& owner = *model.objects[std::size_t(object)];
    const int          part  = args.target.parts.empty() ? -1 : args.target.parts.front().second;
    const bool         shown = owner.hides_imported_parts(); // the split shows the bodies the import hides
    if (part < 0 && owner.volumes.size() > 1 && !shown)
        // TRN %1% is an object's name.
        return Refusal{RefusalKind::Precondition, "several_parts", L("\"%1%\" has several parts: select the part to split into parts."),
                       {owner.name}};
    const ModelVolume*                 split = shown ? nullptr : owner.volumes[std::size_t(std::max(part, 0))];
    const std::set<const ModelVolume*> before(owner.volumes.begin(), owner.volumes.end());
    wxGetApp().obj_list()->split(object, part, args.expand);
    // The part keeps its place, as the first of its shells, and the others follow it.
    Targets parts;
    for (std::size_t v = 0; v < owner.volumes.size(); ++v)
        if (shown || owner.volumes[v] == split || before.count(owner.volumes[v]) == 0)
            parts.parts.emplace_back(object, int(v));
    return parts;
}

std::optional<Refusal> assemble_refusal(const Model& model, const Targets& targets)
{
    if (targets.objects.size() + targets.instances.size() < 2)
        return Refusal{RefusalKind::Precondition, "too_few_objects", L("Select two or more objects first.")};
    for (int object : objects_of(targets))
        if (model.objects[std::size_t(object)]->is_cut())
            return Refusal{RefusalKind::Precondition, "cut_object", L("Cut objects can't be assembled.")};
    return std::nullopt;
}

Outcome<int> assemble_objects(const Targets& targets)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (std::optional<Refusal> refusal = assemble_refusal(model, targets))
        return *refusal;
    ObjectList& list = *wxGetApp().obj_list();
    // The list's selected rows are merged as they are selected, as Assemble merges them.
    if (targets.from_selection) {
        list.merge(true);
        return int(model.objects.size()) - 1;
    }
    ObjectDataViewModel& rows = *list.GetModel();
    wxDataViewItemArray  items;
    for (int object : targets.objects)
        items.Add(rows.GetItemById(object));
    for (const auto& [object, instance] : targets.instances)
        items.Add(rows.GetItemByInstanceId(object, instance));
    list.merge(true, items);
    return int(model.objects.size()) - 1;
}

std::optional<Refusal> part_type_refusal(const Model& model, const Targets& targets, ModelVolumeType type,
                                         std::optional<ModelVolumeType> subtype)
{
    if (subtype && !is_precise_seam(type))
        return Refusal{RefusalKind::Precondition, "subtype_without_seam",
                       // TRN "precise_seam" is a value of the command's type argument: keep it as it is.
                       L("A subtype is a precise seam's: give it with the type precise_seam.")};
    // As ObjectList::set_volume_type() reads them: a painted modifier keeps its type, and a seam keeps its subtype unless
    // one is given.
    const ModelVolumeType     to = subtype.value_or(type);
    std::vector<const ModelVolume*> parts;
    for (const auto& [object, part] : targets.parts)
        if (const ModelVolume* volume = model.objects[std::size_t(object)]->volumes[std::size_t(part)]; !volume->is_painted_modifier())
            parts.push_back(volume);
    const auto becomes = [&](const ModelVolume* volume) { return !subtype && is_precise_seam(to) && volume->is_precise_seam() ? volume->type() : to; };

    if ((to == ModelVolumeType::SUPPORT_BLOCKER || to == ModelVolumeType::SUPPORT_ENFORCER || is_precise_seam(to)) &&
        std::any_of(parts.begin(), parts.end(), [](const ModelVolume* volume) { return volume->is_svg() || volume->is_text(); }))
        return Refusal{RefusalKind::Precondition, "text_part", L("A text or SVG part can only be a part, a negative part or a modifier.")};
    if (std::none_of(parts.begin(), parts.end(), [&](const ModelVolume* volume) { return volume->type() != becomes(volume); }))
        return std::nullopt;
    if (to != ModelVolumeType::MODEL_PART) {
        std::map<const ModelObject*, int> named;
        for (const ModelVolume* volume : parts)
            if (volume->type() == ModelVolumeType::MODEL_PART)
                ++named[volume->get_object()];
        for (const auto& [object, count] : named)
            if (count == std::count_if(object->volumes.begin(), object->volumes.end(),
                                       [](const ModelVolume* volume) { return volume->type() == ModelVolumeType::MODEL_PART; }))
                return Refusal{RefusalKind::Precondition, "last_solid_part", L("The type of the last solid object part cannot be changed.")};
    }
    return std::nullopt;
}

Refusal part_wrong_selection() { return {RefusalKind::Precondition, "wrong_selection", L("Select a part first.")}; }

Outcome<Targets> change_part_type(const ChangePartTypeArgs& args)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (std::optional<Refusal> refusal = part_type_refusal(model, args.targets, args.type, args.subtype))
        return *refusal;
    std::vector<const ModelVolume*> volumes;
    for (const auto& [object, part] : args.targets.parts)
        volumes.push_back(model.objects[std::size_t(object)]->volumes[std::size_t(part)]);
    // A subtype given is set as the Precise Seam Type menu sets it; without one a seam keeps its own.
    if (const wxString kept = wxGetApp().obj_list()->set_volume_type(args.subtype.value_or(args.type), !args.subtype, Prompts::Skip,
                                                                     args.targets.parts);
        !kept.empty())
        // A reason of the object list's that part_type_refusal() does not foresee.
        return Refusal{RefusalKind::Failed, "type_kept", L("The parts kept their type: the object list refused the change.")};
    Targets moved;
    for (std::size_t k = 0; k < volumes.size(); ++k) {
        const ModelVolumePtrs& now = model.objects[std::size_t(args.targets.parts[k].first)]->volumes;
        moved.parts.emplace_back(args.targets.parts[k].first, int(std::find(now.begin(), now.end(), volumes[k]) - now.begin()));
    }
    return moved;
}

Outcome<bool> order_objects(const OrderObjectsArgs& args)
{
    Plater&        plater = the_plater();
    const Model&   model  = plater.model();
    PartPlateList& plates = plater.get_partplate_list();
    const int      count  = plates.get_plate_count();
    const int      plate  = args.plate.value_or(plates.get_curr_plate_index());
    if (plate < 0 || plate >= count)
        return no_such_plate(plate, count);
    if (args.all ? model.objects.empty() : plates.get_plate(plate)->empty())
        return Refusal{RefusalKind::Precondition, "no_objects_to_order", L("There are no objects to order.")};
    // order_objects() only shows a notification while a gizmo is open.
    if (std::optional<Refusal> refusal = gizmo_open(plater))
        return *refusal;
    wxGetApp().obj_list()->order_objects(args.order, args.all, plate, args.descending);
    // Descending where sorting them ascending would change their order.
    const auto plate_of = [&plates](std::size_t object) { return plates.find_instance(int(object), 0); };
    return plate_order(model.objects, plate_of, args.all ? -1 : plate, args.order, false) != model.objects;
}

Outcome<std::vector<int>> reorder_objects(const ReorderObjectsArgs& args)
{
    Plater&        plater = the_plater();
    const Model&   model  = plater.model();
    PartPlateList& plates = plater.get_partplate_list();
    const int      count  = plates.get_plate_count();
    const int      plate  = args.plate.value_or(plates.get_curr_plate_index());
    if (plate < 0 || plate >= count)
        return no_such_plate(plate, count);
    const auto plate_of = [&plates, &model] {
        std::vector<int> on(model.objects.size());
        for (std::size_t object = 0; object < on.size(); ++object)
            on[object] = plates.find_instance(int(object), 0);
        return on;
    };
    const Outcome<std::vector<ObjectMove>> moves = object_order_moves(plate_of(), plate, args.order);
    if (!moves)
        return moves.refusal();
    if (!moves.value().empty()) {
        plater.take_snapshot(_u8L("Object order changed"));
        for (const ObjectMove& move : moves.value())
            wxGetApp().obj_list()->move_object(move.from, move.to, plate);
    }
    std::vector<int>       ordered;
    const std::vector<int> now = plate_of();
    for (std::size_t object = 0; object < now.size(); ++object)
        if (now[object] == plate)
            ordered.push_back(int(object));
    return ordered;
}

std::optional<Refusal> mirror_refusal(const Model& model, const Targets& targets)
{
    if (names_cut_object(model, targets))
        return Refusal{RefusalKind::Precondition, "cut_object", L("Cut objects can't be mirrored.")};
    return std::nullopt;
}

Outcome<Done> mirror_objects(const MirrorObjectsArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = mirror_refusal(plater.model(), args.targets))
        return *refusal;
    if (args.targets.from_selection) {
        if (!plater.get_selection().is_from_single_instance())
            return Refusal{RefusalKind::Precondition, "several_instances", L("Mirror takes one instance, or parts of one: select only one.")};
        plater.mirror(args.axis);
        return Done{};
    }
    Plater::SingleSnapshot single(&plater);
    for (const Targets& turn : mirror_turns(plater.model(), args.targets)) {
        select_targets(turn);
        plater.mirror(args.axis);
    }
    select_targets(args.targets);
    return Done{};
}

std::map<int, std::vector<int>> objects_by_plate(const std::vector<int>& objects, const std::function<int(int)>& plate_of, int current_plate)
{
    std::map<int, std::vector<int>> out;
    for (int object : objects) {
        const int plate = plate_of(object);
        out[plate < 0 ? current_plate : plate].push_back(object);
    }
    return out;
}

Outcome<std::map<int, std::vector<int>>> center_objects(const CenterObjectsArgs& args)
{
    Plater&        plater = the_plater();
    PartPlateList& plates = plater.get_partplate_list();
    if (args.plate && (*args.plate < 0 || *args.plate >= plates.get_plate_count()))
        return no_such_plate(*args.plate, plates.get_plate_count());
    GLCanvas3D& canvas  = *plater.get_view3D_canvas3D();
    const int   current = plates.get_curr_plate_index();
    if (args.targets.from_selection) {
        if (args.plate)
            canvas.do_center_plate(*args.plate);
        else if (plater.get_selection().can_center()) // as the Center menu item is enabled: a centred selection adds no undo step
            plater.center_selection();
        const std::set<int> objects = objects_of(args.targets);
        return std::map<int, std::vector<int>>{{args.plate.value_or(current), std::vector<int>(objects.begin(), objects.end())}};
    }
    const auto plate_of = [&](int object) { return args.plate ? *args.plate : plates.find_instance(object, 0); };
    const std::map<int, std::vector<int>> groups = objects_by_plate(args.targets.objects, plate_of, current);
    Plater::SingleSnapshot                single(&plater);
    for (const auto& [plate, objects] : groups) {
        Targets group;
        group.objects = objects;
        select_targets(group);
        canvas.do_center_plate(plate);
    }
    select_targets(args.targets);
    return groups;
}

std::optional<Refusal> scale_to_fit_refusal(const Model& model, const Targets& targets)
{
    if (names_cut_object(model, targets))
        return Refusal{RefusalKind::Precondition, "cut_object", L("Cut objects can't be scaled to fit the print volume.")};
    return std::nullopt;
}

Outcome<Done> scale_to_fit(const Targets& targets)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = scale_to_fit_refusal(plater.model(), targets))
        return *refusal;
    if (const BuildVolume_Type type = plater.build_volume().type(); type != BuildVolume_Type::Rectangle && type != BuildVolume_Type::Circle)
        return Refusal{RefusalKind::Precondition, "bed_shape", L("Scale to fit needs a rectangular or circular bed.")};
    if (!targets.from_selection)
        select_targets(targets);
    plater.scale_selection_to_fit_print_volume();
    return Done{};
}

Targets objects_above_bed(const Model& model, const Targets& targets)
{
    // As Selection::can_drop() measures it.
    const auto off_bed = [](double min_z) { return std::abs(min_z) > -SINKING_Z_THRESHOLD; };
    Targets    out;
    for (int object : targets.objects) {
        const ModelObject& owner = *model.objects[std::size_t(object)];
        for (std::size_t i = 0; i < owner.instances.size(); ++i)
            if (off_bed(owner.get_instance_min_z(i))) {
                out.objects.push_back(object);
                break;
            }
    }
    for (const auto& [object, instance] : targets.instances)
        if (off_bed(model.objects[std::size_t(object)]->get_instance_min_z(std::size_t(instance))))
            out.instances.emplace_back(object, instance);
    for (const auto& [object, part] : targets.parts) {
        const ModelObject& owner  = *model.objects[std::size_t(object)];
        const ModelVolume& volume = *owner.volumes[std::size_t(part)];
        if (owner.instances.empty())
            continue;
        const Transform3d placed = owner.instances.front()->get_matrix() * volume.get_matrix();
        if (off_bed(volume.mesh().transformed_bounding_box(placed).min.z()))
            out.parts.emplace_back(object, part);
    }
    return out;
}

Refusal already_on_bed() { return {RefusalKind::Precondition, "on_bed", L("Nothing to drop: it already rests on the bed.")}; }

Outcome<bool> drop_objects(const Targets& targets)
{
    Plater& plater = the_plater();
    if (targets.from_selection) {
        if (!plater.get_selection().can_drop())
            return false;
        plater.drop_selection();
        return true;
    }
    // Each instance and each part that is off the bed, as Drop drops it selected alone: an object's instances one by one.
    const Model&                  model = plater.model();
    std::set<std::pair<int, int>> instances;
    Targets                       each;
    for (int object : targets.objects)
        for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
            instances.emplace(object, int(i));
    instances.insert(targets.instances.begin(), targets.instances.end());
    each.instances.assign(instances.begin(), instances.end());
    each.parts = targets.parts;
    each       = objects_above_bed(model, each);
    std::vector<Targets> turns;
    for (const auto& instance : each.instances) {
        Targets one;
        one.instances = {instance};
        turns.push_back(std::move(one));
    }
    for (const auto& part : each.parts) {
        Targets one;
        one.parts = {part};
        turns.push_back(std::move(one));
    }
    {
        Plater::SingleSnapshot single(&plater);
        for (const Targets& turn : turns) {
            select_targets(turn);
            plater.drop_selection();
        }
    }
    select_targets(targets);
    return !turns.empty();
}

std::vector<std::pair<int, int>> orientable(const Model& model, const std::set<int>& objects, const std::function<bool(int, int)>& locked)
{
    std::vector<std::pair<int, int>> out;
    for (int object : objects) {
        const ModelObject& owner = *model.objects[std::size_t(object)];
        if (!owner.printable)
            continue;
        for (std::size_t i = 0; i < owner.instances.size(); ++i)
            if (!locked(object, int(i)))
                out.emplace_back(object, int(i));
    }
    return out;
}

std::function<bool(int, int)> on_locked_plate(PartPlateList& plates)
{
    return [&plates](int object, int instance) {
        const int plate = plates.find_instance(object, instance);
        return plate >= 0 && plate < plates.get_plate_count() && plates.is_locked(plate);
    };
}

Outcome<Done> orient_objects(const OrientObjectsArgs& args)
{
    Plater&        plater = the_plater();
    const Model&   model  = plater.model();
    PartPlateList& plates = plater.get_partplate_list();
    if (model.objects.empty())
        return Refusal{RefusalKind::Precondition, "no_objects_to_orient", L("There are no objects to orient.")};
    if (!plater.get_ui_job_worker().is_idle())
        return job_running();
    std::set<int> objects(args.targets.objects.begin(), args.targets.objects.end());
    for (int plate : args.plates) {
        if (plate < 0 || plate >= plates.get_plate_count())
            return no_such_plate(plate, plates.get_plate_count());
        bool holds = false;
        for (std::size_t o = 0; o < model.objects.size(); ++o)
            for (std::size_t i = 0; i < model.objects[o]->instances.size(); ++i)
                if (plates.find_instance(int(o), int(i)) == plate) {
                    objects.insert(int(o));
                    holds = true;
                }
        // Before the lock: a plate of no objects would leave none to orient, which the job takes for every object.
        if (!holds)
            // TRN %1% is a plate index.
            return Refusal{RefusalKind::Precondition, "no_objects_to_orient", L("Plate %1% has no objects to orient."), {std::to_string(plate)}};
        if (plates.is_locked(plate))
            return Refusal{RefusalKind::Precondition, "plate_locked", L("This plate is locked.\nCannot auto-orient on this plate.")};
    }
    const bool                          named  = !args.targets.from_selection || !args.plates.empty();
    const std::function<bool(int, int)> locked = on_locked_plate(plates);
    if (named && orientable(model, objects, locked).empty()) {
        // The job turns every other object when it can turn none of those it is given: a call that names its objects
        // turns none it did not name.
        const bool all_locked = std::all_of(objects.begin(), objects.end(), [&](int object) {
            for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
                if (!locked(object, int(i)))
                    return false;
            return true;
        });
        if (all_locked)
            return Refusal{RefusalKind::Precondition, "plate_locked",
                           L("All the selected objects are on a locked plate.\nCannot auto-orient these objects.")};
        return Refusal{RefusalKind::Precondition, "not_printable",
                       L("Auto-Orient can turn none of these objects: they are not printable, or stand on a locked plate.")};
    }
    if (named) {
        Targets selected;
        selected.objects.assign(objects.begin(), objects.end());
        select_targets(selected);
    }
    // A plate's Auto Rotate icon leaves the plate-only state behind.
    plater.set_prepare_state(Job::PREPARE_STATE_DEFAULT);
    plater.orient(std::vector<int>(objects.begin(), objects.end()));
    return Done{};
}

Outcome<int> add_primitive(const AddPrimitiveArgs& args)
{
    if (args.name)
        if (std::optional<Refusal> refusal = rename_refusal(*args.name))
            return *refusal;
    Plater&                plater = the_plater();
    Plater::SingleSnapshot single(&plater);
    show_prepare_view();
    if (args.plate)
        if (const Outcome<Done> selected = select_plate({*args.plate}); !selected)
            return selected.refusal();
    ObjectList& list = *wxGetApp().obj_list();
    if (args.prompts == Prompts::Ask && !list.can_add_shape_object())
        return Refusal{RefusalKind::Precondition, "object_selected",
                       L("Deselect all objects to add a primitive object. To add a shape to the selected object, use its Add Part menu.")};
    const std::size_t before = plater.model().objects.size();
    list.load_shape_object(args.shape, args.prompts, args.size);
    if (plater.model().objects.size() == before)
        return Refusal{RefusalKind::Failed, "not_added", L("No object was added.")};
    const int index = int(before);
    if (args.name)
        if (const Outcome<std::string> renamed = rename_object({index, -1, *args.name}); !renamed)
            return renamed.refusal();
    if (args.position) {
        plater.model().objects[std::size_t(index)]->instances.front()->set_offset(*args.position);
        placed_instance(index, 0, true);
    }
    return index;
}

std::vector<std::pair<int, int>> set_printable(Model& model, const Targets& targets, bool printable)
{
    std::vector<std::pair<int, int>> out;
    const auto set = [&](int object, int instance) {
        if (std::find(out.begin(), out.end(), std::make_pair(object, instance)) != out.end())
            return;
        model.objects[std::size_t(object)]->instances[std::size_t(instance)]->printable = printable;
        out.emplace_back(object, instance);
    };
    for (int object : targets.objects)
        for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
            set(object, int(i));
    for (const auto& [object, instance] : targets.instances)
        set(object, instance);
    return out;
}

Outcome<std::vector<std::pair<int, int>>> set_objects_printable(const SetPrintableArgs& args)
{
    Plater& plater = the_plater();
    // The Printable cell's undo step, which has no name.
    Plater::TakeSnapshot                   snapshot(&plater, "");
    const std::vector<std::pair<int, int>> set = set_printable(plater.model(), args.targets, args.printable);
    ObjectList&                            list = *wxGetApp().obj_list();
    std::set<std::size_t>                  objects;
    for (const auto& [object, instance] : set) {
        list.update_printable_state(object, instance);
        objects.insert(std::size_t(object));
    }
    plater.get_view3D_canvas3D()->update_instance_printable_state_for_objects(std::vector<std::size_t>(objects.begin(), objects.end()));
    plater.update();
    plater.reload_paint_after_background_process_apply();
    return set;
}

std::vector<int> set_auto_drop(Model& model, const Targets& targets, bool auto_drop)
{
    std::vector<int> out;
    const auto       set = [&](int object) {
        if (std::find(out.begin(), out.end(), object) != out.end())
            return;
        model.objects[std::size_t(object)]->auto_drop = auto_drop;
        out.push_back(object);
    };
    for (int object : targets.objects)
        set(object);
    for (const auto& [object, instance] : targets.instances)
        set(object);
    for (const auto& [object, part] : targets.parts)
        set(object);
    return out;
}

Outcome<std::vector<int>> set_objects_auto_drop(const SetAutoDropArgs& args)
{
    Plater& plater = the_plater();
    // The 3D view's Auto Drop's undo step.
    Plater::TakeSnapshot   snapshot(&plater, args.auto_drop ? "Set Selection Auto-Drop Enabled" : "Set Selection Auto-Drop Disabled");
    const std::vector<int> set = set_auto_drop(plater.model(), args.targets, args.auto_drop);
    plater.update();
    return set;
}

MeshErrors mesh_errors(const ModelObject& object)
{
    const TriangleMeshStats  stats = object.get_object_stl_stats();
    const RepairedMeshErrors fixed = stats.repaired_errors;
    MeshErrors               out;
    out.open_edges        = stats.open_edges;
    out.manifold          = stats.manifold();
    out.degenerate_facets = fixed.degenerate_facets;
    out.edges_fixed       = fixed.edges_fixed;
    out.facets_removed    = fixed.facets_removed;
    out.facets_reversed   = fixed.facets_reversed;
    out.backwards_edges   = fixed.backwards_edges;
    return out;
}

Outcome<FixedModels> fix_objects(const FixObjectsArgs& args, const FixProgress& progress)
{
    Plater& plater = the_plater();
    Model&  model  = plater.model();
    if (std::optional<Refusal> refusal = gizmo_open(plater))
        return *refusal;

    // What is repaired, in turn: [object, part], part -1 for a whole object.
    std::vector<std::pair<int, int>> models;
    if (args.all || args.plate) {
        std::vector<int> objects;
        if (args.all) {
            for (std::size_t o = 0; o < model.objects.size(); ++o)
                objects.push_back(int(o));
        } else {
            PartPlateList& plates = plater.get_partplate_list();
            if (*args.plate < 0 || *args.plate >= plates.get_plate_count())
                return no_such_plate(*args.plate, plates.get_plate_count());
            for (const ModelObject* object : plates.get_plate(*args.plate)->get_objects_on_this_plate())
                objects.push_back(int(std::find(model.objects.begin(), model.objects.end(), object) - model.objects.begin()));
            std::sort(objects.begin(), objects.end());
            objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
        }
        if (objects.empty())
            return Refusal{RefusalKind::Precondition, "no_objects", L("There are no objects to repair.")};
        for (int object : objects)
            models.emplace_back(object, -1);
    } else {
        // An instance stands for its object, whose meshes its instances share.
        std::vector<int> whole = args.targets.objects;
        for (const auto& [object, instance] : args.targets.instances)
            if (std::find(whole.begin(), whole.end(), object) == whole.end())
                whole.push_back(object);
        for (int object : whole)
            models.emplace_back(object, -1);
        for (const auto& [object, part] : args.targets.parts)
            if (std::find(whole.begin(), whole.end(), object) == whole.end())
                models.emplace_back(object, part);
    }

    const bool           keep_painting = wxGetApp().app_config->get_bool("keep_painting");
    ObjectList&          list          = *wxGetApp().obj_list();
    Plater::TakeSnapshot snapshot(&plater, _u8L("Repairing model object"));
    return repair_models(model, models, progress, keep_painting, [&](int index, int part) {
        model.objects[std::size_t(index)]->ensure_on_bed();
        plater.changed_mesh(index);
        plater.get_partplate_list().notify_instance_update(index, 0);
        list.update_plate_values_for_items();
        list.update_item_error_icon(index, part);
        list.update_info_items(std::size_t(index));
    });
}

FixedModels repair_models(Model& model, const std::vector<std::pair<int, int>>& models, const FixProgress& progress,
                          bool keep_painting, const std::function<void(int object, int part)>& changed)
{
    FixedModels out;
    for (std::size_t k = 0; k < models.size(); ++k) {
        const auto [index, part] = models[k];
        ModelObject& object      = *model.objects[std::size_t(index)];
        FixedModel   fixed;
        fixed.object             = index;
        fixed.part               = part;
        fixed.name               = part < 0 ? object.name : object.volumes[std::size_t(part)]->name;
        fixed.before             = mesh_errors(object);
        const auto painted       = [&object]() {
            return std::count_if(object.volumes.begin(), object.volumes.end(),
                                 [](const ModelVolume* volume) { return volume->is_any_painted(); });
        };
        const auto painted_before = painted();
        const auto report         = [&progress, k](int percent, const std::string& what) { return progress(k, percent, what); };
        // A cancel ends the repair. The parts repaired before it keep their new mesh, so a model whose mesh changed before
        // the cancel goes to `changed` and is listed as any other.
        fixed.cancelled = !fix_model_with_cgal(object, part, report, fixed.failed, keep_painting, &fixed.changed);
        if (fixed.changed) {
            fixed.painting_removed = painted() < painted_before;
            if (changed)
                changed(index, part);
        }
        fixed.after   = mesh_errors(object);
        out.cancelled = fixed.cancelled;
        if (!fixed.cancelled || fixed.changed)
            out.models.push_back(std::move(fixed));
        if (out.cancelled)
            break;
    }
    return out;
}

Outcome<ClonedObjects> clone_objects(const CloneObjectsArgs& args, const std::function<bool(int done, int count)>& pasted)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (!args.targets.from_selection)
        select_targets(args.targets);
    ClonedObjects     out;
    const std::size_t before = model.objects.size();
    out.arranged             = args.arrange.value_or(wxGetApp().app_config->get("auto_arrange") == "true");
    // Better to stop rendering canvas while processing. The Clone dialog's Cancel thaws it as it ends the dialog.
    plater.Freeze();
    const bool all_pasted = plater.canvas3D()->get_selection().clone(args.count, [&pasted, &args](int done) {
        return !pasted || pasted(done, args.count);
    });
    if (!all_pasted) {
        plater.undo();
        if (plater.IsFrozen())
            plater.Thaw();
        return Refusal{RefusalKind::Failed, "cancelled", L("Cloning was cancelled: the copies were taken back.")};
    }
    if (out.arranged) {
        plater.set_prepare_state(Job::PREPARE_STATE_MENU);
        plater.arrange();
    }
    if (plater.IsFrozen())
        plater.Thaw();
    for (std::size_t o = before; o < model.objects.size(); ++o)
        out.objects.objects.push_back(int(o));
    return out;
}

bool has_reload_source(const ModelVolume& volume)
{
    return !volume.source.is_from_builtin_objects && !volume.source.input_file.empty() &&
           !boost::filesystem::path(volume.source.input_file).extension().string().empty();
}

ReloadSources reload_sources(const Model& model, const Targets& targets, const std::function<bool(const std::string& path)>& exists)
{
    // Every part of an object named, or of the object of an instance named, whose parts its instances share.
    std::set<int> whole(targets.objects.begin(), targets.objects.end());
    for (const auto& [object, instance] : targets.instances)
        whole.insert(object);
    std::vector<std::pair<int, int>> parts;
    for (int object : whole)
        for (std::size_t v = 0; v < model.objects[std::size_t(object)]->volumes.size(); ++v)
            parts.emplace_back(object, int(v));
    for (const auto& part : targets.parts)
        if (std::find(parts.begin(), parts.end(), part) == parts.end())
            parts.push_back(part);

    ReloadSources out;
    for (const auto& [object, part] : parts) {
        const ModelObject& owner  = *model.objects[std::size_t(object)];
        const ModelVolume& volume = *owner.volumes[std::size_t(part)];
        if (!has_reload_source(volume))
            continue;
        std::string found;
        if (exists(volume.source.input_file)) {
            found = volume.source.input_file;
        } else if (!owner.input_file.empty()) {
            // A file of its name in the folder of its object's file, where a project moved with its parts finds them.
            boost::filesystem::path beside = boost::filesystem::path(owner.input_file).remove_filename();
            if (!beside.empty()) {
                beside /= boost::filesystem::path(volume.source.input_file).filename();
                if (exists(beside.string()))
                    found = beside.string();
            }
        }
        if (found.empty()) {
            out.missing.emplace_back(object, part);
            continue;
        }
        out.parts.emplace_back(object, part);
        if (std::find(out.paths.begin(), out.paths.end(), found) == out.paths.end())
            out.paths.push_back(found);
    }
    std::sort(out.paths.begin(), out.paths.end());
    return out;
}

Outcome<ReloadedParts> reload_parts(const ReloadFromDiskArgs& args)
{
    Plater&      plater = the_plater();
    const Model& model  = plater.model();
    if (std::optional<Refusal> refusal = gizmo_open(plater))
        return *refusal;
    Targets targets = args.targets;
    if (args.all) {
        // As Reload All does, but for the cut objects, which Reload from disk refuses.
        targets = Targets{};
        for (std::size_t o = 0; o < model.objects.size(); ++o)
            if (!model.objects[o]->is_cut())
                targets.objects.push_back(int(o));
        if (targets.objects.empty())
            return Refusal{RefusalKind::Precondition, "no_objects", L("There are no objects to reload.")};
    } else if (names_cut_object(model, targets)) {
        return Refusal{RefusalKind::Precondition, "cut_object", L("Cut objects can't be reloaded from disk.")};
    }
    const ReloadSources sources = reload_sources(model, targets, [](const std::string& path) {
        boost::system::error_code ec;
        return boost::filesystem::exists(path, ec);
    });
    if (sources.parts.empty() && sources.missing.empty())
        return Refusal{RefusalKind::Precondition, "no_source", L("None of these parts was loaded from a file, so none can be reloaded.")};
    ReloadedParts out;
    out.missing = sources.missing;
    if (!sources.parts.empty()) {
        const Plater::ReloadResult reloaded = plater.reload_parts_from_disk(sources.parts);
        out.reloaded                        = reloaded.reloaded;
        out.unmatched                       = reloaded.unmatched;
        out.unreadable                      = reloaded.unreadable;
    }
    return out;
}

std::optional<Refusal> replacement_refusal(const Model& loaded)
{
    if (loaded.objects.size() > 1 || (loaded.objects.size() == 1 && loaded.objects.front()->volumes.size() > 1))
        return several_parts_loaded();
    return std::nullopt;
}

Outcome<std::pair<int, int>> replaced_part(const Model& model, const Targets& targets)
{
    if (!targets.parts.empty())
        return targets.parts.front();
    const int          index  = targets.objects.empty() ? targets.instances.front().first : targets.objects.front();
    const ModelObject& object = *model.objects[std::size_t(index)];
    if (object.volumes.size() != 1)
        return Refusal{RefusalKind::Precondition, "several_parts",
                       // TRN %1% is an object's name.
                       L("\"%1%\" has several parts: name the part to replace."), {object.name}};
    return std::make_pair(index, 0);
}

Outcome<int> replace_part(const ReplacePartArgs& args)
{
    Plater& plater = the_plater();
    if (std::optional<Refusal> refusal = gizmo_open(plater))
        return *refusal;
    const ModelObject& object = *plater.model().objects[std::size_t(args.object)];
    if (object.is_cut())
        return Refusal{RefusalKind::Precondition, "cut_object", L("A part of a cut object can't be replaced.")};
    // The replacement sorts the object's parts by type, which can move the new part: it is the one whose id is not kept.
    std::vector<ObjectID> kept;
    for (std::size_t v = 0; v < object.volumes.size(); ++v)
        if (int(v) != args.part)
            kept.push_back(object.volumes[v]->id());
    switch (plater.replace_part_with_file(args.object, args.part, args.path)) {
    case Plater::ReplaceResult::Replaced: break;
    case Plater::ReplaceResult::Unreadable:
        // TRN %1% is a file's path.
        return Refusal{RefusalKind::Failed, "not_loaded", L("%1% could not be loaded."), {args.path}};
    case Plater::ReplaceResult::Cancelled:
        return Refusal{RefusalKind::Failed, "cancelled", L("The STEP file's mesh options were cancelled, so nothing was replaced.")};
    case Plater::ReplaceResult::SeveralParts: return several_parts_loaded();
    }
    for (std::size_t v = 0; v < object.volumes.size(); ++v)
        if (std::find(kept.begin(), kept.end(), object.volumes[v]->id()) == kept.end())
            return int(v);
    return args.part;
}

void placed_instance(int object, int instance, bool drop)
{
    Plater&      plater = the_plater();
    ModelObject& owner  = *plater.model().objects[std::size_t(object)];
    // The bounding box is cached: the drop must measure the new placement.
    owner.invalidate_bounding_box();
    // Only the instance placed comes to rest on the bed, by its own lowest point, as the canvas drops the instances a
    // move or scale changed: the object's other instances keep their heights.
    if (drop && owner.auto_drop) {
        const double min_z = owner.get_instance_min_z(std::size_t(instance));
        if (min_z != 0.0)
            owner.translate_instance(std::size_t(instance), Vec3d(0.0, 0.0, -min_z));
    }
    plater.get_partplate_list().notify_instance_update(object, instance);
    wxGetApp().obj_list()->update_plate_values_for_items();
    plater.changed_object(object);
}

}}} // namespace Slic3r::GUI::Ops
