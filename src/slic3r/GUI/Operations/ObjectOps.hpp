#pragma once

#include "Outcome.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class Model;
class ModelObject;
class ModelVolume;
enum class ModelVolumeType : int;
namespace GUI {
class PartPlateList;
enum class ObjectOrder;
}
} // namespace Slic3r

// The objects of the project, their instances and their parts: what a command acts on, and the operations that add,
// delete, rename, separate, split, assemble, order, mirror, centre, scale, drop and orient them, change what a part
// is, mark them printable, turn their Auto Drop on or off, repair their meshes, clone them, reload them from their files
// and replace a part's mesh with a file's. An operation takes indices into Model::objects, which its caller has checked
// (Mcp::resolve_targets() does).
namespace Slic3r { namespace GUI { namespace Ops {

// What a command acts on: whole objects by index, then [object, instance] and [object, part] pairs, in the order
// named, each once.
struct Targets
{
    std::vector<int>                 objects;
    std::vector<std::pair<int, int>> instances;
    std::vector<std::pair<int, int>> parts;
    bool                             from_selection = false; // none was named, so these are the Prepare view's selection
};

// The refusal of a command given no target while nothing is selected (nothing_selected).
Refusal nothing_selected();
// The refusal of a command given no target while the selection holds what it does not act on (wrong_selection).
Refusal wrong_selection();
// The refusal of a command of one target given several, or several selected (one_object_only).
Refusal one_object_only();
// The refusal of Select All while a gizmo that keeps its selection is open (gizmo_open), which lets Select All
// select nothing.
Refusal gizmo_keeps_selection();

// Selects `targets` in the Prepare view, which it shows, and nothing else, as clicking them would: an object whole, an
// instance, a part on instance 0; the object list shows it. UI thread.
void select_targets(const Targets& targets);

// ---- Delete ----

// What deleting `targets` deletes, as the object list deletes a selection: an object every instance or every part
// of which is named goes whole, and what a whole object holds is not named again.
Targets deletion(const Model& model, const Targets& targets);

// What keeps `targets` from being deleted without a question, or none: a cut object or a solid or negative part of
// one (cut_object, with the question the object list asks before it breaks the cut), and an object's last solid part
// without the object (last_solid_part).
std::optional<Refusal> delete_refusal(const Model& model, const Targets& targets);

// Whether what delete_refusal() gives is refused before anything is deleted, or left to the object list, which asks
// its question about a cut object and shows its message about a last solid part, as for a person's Delete.
enum class DeleteChecks { Refuse, ObjectList };

// The identities (ObjectBase::id()) of what `targets` names in `model`, in the order named: its objects', its instances'
// and its parts'. Unlike an index, an identity stays with its object, instance or part when a delete moves what follows.
struct TargetIds
{
    std::vector<ObjectID> objects;
    std::vector<ObjectID> instances;
    std::vector<ObjectID> parts;
};
TargetIds target_ids(const Model& model, const Targets& targets);

// What a delete asked for went, and what stayed.
struct DeletedTargets
{
    Targets deleted; // with the indices it had
    Targets kept;    // with its indices afterwards
};
// What of `doomed`, named in the model before a delete, `before` being their identities there (target_ids()), the model
// afterwards, `after`, no longer holds: found by identity, since the delete moved the indices.
DeletedTargets deleted_targets(const Targets& doomed, const TargetIds& before, const Model& after);

struct DeleteObjectsArgs
{
    Targets      targets;
    bool         all    = false; // every object, as Delete All does, in place of the targets
    DeleteChecks checks = DeleteChecks::Refuse;
};
// Deletes the targets, as Delete does with them selected, in one "Delete Selected Objects" undo step, or every
// object, as Delete All does, cut objects too without a question. Returns what it deleted of what deletion() gives, and
// what of that it kept (deleted_targets()): with DeleteChecks::ObjectList the object list deletes from the last item and
// stops at one it refuses or asks about, after deleting those after it.
// Refuses: no_objects (all, with none), what delete_refusal() gives (with DeleteChecks::Refuse), and nothing_deleted
// (nothing_deleted()).
Outcome<DeletedTargets> delete_objects(const DeleteObjectsArgs& args);
// The refusal of a delete the object list did not make (nothing_deleted): it refused the last solid part, or the
// question it asked about a cut object was cancelled or answered by invalidating the cut.
Refusal nothing_deleted();

// ---- Rename ----

// What keeps `name` from being an object's or a part's name, or none: an empty name (empty_name), which the Rename
// dialog leaves unchanged, and a character a file name cannot hold (invalid_name), which the object list refuses.
std::optional<Refusal> rename_refusal(const std::string& name);

// The refusal of Rename with a selection it does not rename, such as an instance, in the object menu item's words
// (wrong_selection).
Refusal rename_wrong_selection();

// What keeps part `part` of object `object` (-1 for the object itself) from being renamed, or none: a part the object
// list does not show (part_not_listed), a cut's connector.
std::optional<Refusal> unlisted_part_refusal(const Model& model, int object, int part);

struct RenameObjectArgs
{
    int         object = 0;
    int         part   = -1; // a part of the object, or -1 for the object itself
    std::string name;
};
// Renames the object or the part as the Rename dialog does, in one "Rename Object" or "Rename Part" undo step. A part
// of an object of one part has no row of its own in the object list, which names the two alike, so the object is
// renamed with it. Returns the name it had.
// Refuses: what rename_refusal() and unlisted_part_refusal() give.
Outcome<std::string> rename_object(const RenameObjectArgs& args);

// ---- Set as Individual Objects ----

// What keeps `targets`, an object or instances of one object, from being made separate objects, or none: an object
// of one instance (one_instance), an instance the object lacks (no_such_instance), parts (wrong_selection), and
// instances of several objects (one_object_only).
std::optional<Refusal> separate_refusal(const Model& model, const Targets& targets);

// Makes the targets' instances separate objects, added after the others, as Set as Individual Objects does, in one
// undo step: with the whole object (or every instance) each instance but the first becomes an object of its own, and
// with some of its instances those become one new object. Returns the objects that hold the instances afterwards:
// the object and its new objects, or the new object.
// Refuses: what separate_refusal() gives.
Outcome<Targets> separate_instances(const Targets& targets);

// ---- Split ----

// What keeps `targets` from being split, or none: a mesh of one shell (one_shell), an object of one part or a part.
std::optional<Refusal> split_refusal(const Model& model, const Targets& targets);

struct SplitObjectArgs
{
    Targets             target;            // one object, or for parts one part of it
    bool                to_objects = true; // into objects, else into parts
    std::optional<bool> keep_z;            // the answer to Split's Auto-Drop question: true keeps the height of what floats
    Prompts             prompts = Prompts::Skip; // with keep_z none and Ask, the question is asked, and a message tells an
                                                 // object of one piece, as Split does; with Skip the split is refused for them
    bool                expand = false;    // into parts, the object's row is opened on its parts, as a split from that row
                                           // opens it
};
// Splits the target as Split > To Objects or To Parts does, in one undo step. Into objects, the object's pieces are
// added last as objects, which are selected, and the object is removed; into parts, its part, or its one part, becomes
// one part per shell, after it among its parts (an import shown as one part shows its bodies). Returns the new objects,
// or the parts the part became, with their indices afterwards.
// Refuses: what split_refusal() gives; several_parts (into parts, an object of several parts: its part splits);
// one_piece (into objects, an object whose solid parts are one piece; with ask, after the message); auto_drop_question
// (with neither keep_z nor ask, a split that would ask it).
Outcome<Targets> split_object(const SplitObjectArgs& args);

// ---- Assemble ----

// What keeps `targets` from being assembled into one object, or none: fewer than two objects or instances, as the
// object list counts its rows (too_few_objects), and a cut object (cut_object).
std::optional<Refusal> assemble_refusal(const Model& model, const Targets& targets);

// Assembles the targets into one object of parts named Assembly, as Assemble does with them selected, in one undo step:
// an instance named becomes an object of its own first, the objects are removed, and the new object is added last and
// selected. Targets read from the selection are the object list's selected rows, which it assembles as they are.
// Returns its index.
// Refuses: what assemble_refusal() gives.
Outcome<int> assemble_objects(const Targets& targets);

// ---- Change Type ----

// What keeps the parts of `targets` from becoming `type`, or with a precise seam's `subtype` that one, or none: what
// ObjectList::set_volume_type() refuses, a text or SVG part made a support blocker or enforcer or a precise seam
// (text_part) and an object's last solid part made something else (last_solid_part); and a subtype with a type other
// than a precise seam (subtype_without_seam).
std::optional<Refusal> part_type_refusal(const Model& model, const Targets& targets, ModelVolumeType type,
                                         std::optional<ModelVolumeType> subtype);

// The refusal of Change Type with a selection of anything but parts, in the menu item's words (wrong_selection).
Refusal part_wrong_selection();

struct ChangePartTypeArgs
{
    Targets                        targets; // parts
    ModelVolumeType                type;
    std::optional<ModelVolumeType> subtype; // a precise seam's; none keeps a seam's own, as Change Type does
};
// Changes what the parts are, as Change Type does, in one undo step; a painted modifier keeps its type. Parts are
// listed by type, so their indices change. Returns the parts with their indices afterwards.
// Refuses: what part_type_refusal() gives.
Outcome<Targets> change_part_type(const ChangePartTypeArgs& args);

// ---- Order ----

struct OrderObjectsArgs
{
    ObjectOrder         order;
    std::optional<int>  plate;      // none: the current plate
    bool                all = false; // every plate, in place of one
    std::optional<bool> descending;  // none: descending when the order they have is ascending already, as Order By does
};
// Orders a plate's objects, or every plate's, as the plate menu's Order By does (ObjectList::order_objects()), which
// sets the object list and print order, in one undo step, or none when the order stays as it was. Returns whether they
// are in descending order.
// Refuses: no_such_plate, no_objects_to_order, gizmo_open.
Outcome<bool> order_objects(const OrderObjectsArgs& args);

struct ReorderObjectsArgs
{
    std::optional<int> plate; // none: the current plate
    std::vector<int>   order; // the plate's objects, each once, in the order wanted
};
// Puts a plate's objects in `order`, which sets the object list and print order, as dragging each in the object list
// onto the one in its place does (ObjectList::OnDrop(), the drops object_order_moves() gives), in one undo step, or none
// when the order stays as it was. The objects of other plates keep their order. Returns the plate's objects, by their indices afterwards, in that order.
// Refuses: no_such_plate, and what object_order_moves() refuses.
Outcome<std::vector<int>> reorder_objects(const ReorderObjectsArgs& args);

// ---- Mirror, centre, scale to fit, drop, orient ----
// Each runs the function of its menu item on the targets selected in the Prepare view, the view shown first, and leaves them
// selected. Targets read from the selection are the selection, which it acts on as it is, as the menu item does.

// What keeps `targets` from being mirrored, or none: a cut object (cut_object).
std::optional<Refusal> mirror_refusal(const Model& model, const Targets& targets);

struct MirrorObjectsArgs
{
    Targets targets;
    Axis    axis = Axis::X;
};
// Mirrors the targets along `axis`, as Mirror does (Plater::mirror()), in one undo step: each instance of an object
// named, each instance named, and the parts of each object named, in turn, since Mirror takes one instance or parts of
// one. The selection is mirrored as it is.
// Refuses: what mirror_refusal() gives; several_instances (a selection of more than one instance).
Outcome<Done> mirror_objects(const MirrorObjectsArgs& args);

// `objects` grouped by the plate each stands on, `plate_of` (-1: none), an object on no plate going with
// `current_plate`'s group; each group in the order of `objects`.
std::map<int, std::vector<int>> objects_by_plate(const std::vector<int>& objects, const std::function<int(int)>& plate_of, int current_plate);

struct CenterObjectsArgs
{
    Targets            targets;
    std::optional<int> plate; // every target centred on this plate, in place of the one each stands on
};
// Centres the targets on their plate, as Center does, in one undo step. The selection with no plate moves as one to the
// centre of the current plate (Selection::center()), and with a plate to that plate's. Objects named are grouped by the
// plate the first instance of each stands on, or all on `plate` (objects_by_plate(), with the current plate for none),
// and each group moves as one to its plate's centre (Selection::center_plate()). Returns the objects centred on each
// plate.
// Refuses: no_such_plate.
Outcome<std::map<int, std::vector<int>>> center_objects(const CenterObjectsArgs& args);

// What keeps `targets` from being scaled to fit the print volume, or none: a cut object (cut_object).
std::optional<Refusal> scale_to_fit_refusal(const Model& model, const Targets& targets);

// Scales the targets as one to fit the current plate's print volume and moves them into it, as Scale to Fit Print Volume
// does (Plater::scale_selection_to_fit_print_volume()), in one undo step.
// Refuses: what scale_to_fit_refusal() gives; bed_shape (a bed neither rectangular nor circular).
Outcome<Done> scale_to_fit(const Targets& targets);

// What of `targets` does not rest on the bed, as Drop measures it: an object one of whose instances, an instance, or a
// part whose lowest point on instance 0, is above the bed or below it.
Targets objects_above_bed(const Model& model, const Targets& targets);

// The answer of a drop whose targets rest on the bed already: nothing moved (on_bed). A call that gets it did not fail.
Refusal already_on_bed();

// Drops the targets onto the bed, as Drop does (Plater::drop_selection()), in one undo step: each instance and each part
// named that objects_above_bed() gives, in turn, and the selection as one, by its lowest point. Returns whether anything
// moved.
Outcome<bool> drop_objects(const Targets& targets);

// The instances of `objects` Auto-Orient turns, as OrientJob::prepare_selection() takes them: each instance of a printable
// object (ModelObject::printable) that `locked` (object, instance) does not put on a locked plate.
std::vector<std::pair<int, int>> orientable(const Model& model, const std::set<int>& objects, const std::function<bool(int, int)>& locked);
// Whether an instance stands on a locked plate of `plates`, as the job reads it, for orientable().
std::function<bool(int, int)> on_locked_plate(PartPlateList& plates);

struct OrientObjectsArgs
{
    Targets          targets; // objects; none read from the selection: every object, as Auto-Orient with nothing selected
    std::vector<int> plates;  // the objects that stand on these plates too
};
// Starts Auto-Orient of the targets, as the toolbar's Auto-Orient does with them selected, in the background (OrientJob)
// as one undo step; the UI job worker is busy until it ends. An object on a locked plate, or one that is not printable,
// keeps its orientation. Objects named, or of the plates named, are the only ones turned; targets read from the
// selection are turned as the job turns a selection: every other object when it can turn none of them and none of them
// stands on a locked plate, and nothing, with the job's warning, when it can turn none and one stands on a locked plate.
// Refuses: no_objects_to_orient (no objects, or a plate named that holds none), job_running, no_such_plate,
// plate_locked (a plate named is locked, or every object named is on a locked plate), not_printable (nothing named that
// the job can turn: each is not printable, or stands on a locked plate).
Outcome<Done> orient_objects(const OrientObjectsArgs& args);

// ---- Add Primitive ----

struct AddPrimitiveArgs
{
    std::string                shape;       // the object list's name of the shape: Cube, Cylinder, Sphere, Cone, Disc, Torus
    std::vector<double>        size;        // the Primitive size dialog's fields; none: the built-in size, or with ask the
                                            // dialog's
    std::optional<int>         plate;       // the plate it is added on, which becomes the current one; none: the current plate
    std::optional<std::string> name;        // its name (rename_object()); none: its shape's
    std::optional<Vec3d>       position;    // where its first instance goes, in bed coordinates, as object_transform's
                                            // position, after which it rests on the bed; none: where Add Primitive puts it
    Prompts                    prompts = Prompts::Skip; // Ask: as Add Primitive, refused while an object is selected, and
                                                        // the size dialog asks
};
// Adds the shape as a new object, as Add Primitive does in the Prepare view, the view shown first, named and placed, in
// one undo step, and selects it. Returns its index.
// Refuses: what rename_refusal() gives for the name, before anything is added; no_such_plate; object_selected (with
// ask, while an object is selected); not_added (the size dialog cancelled); what rename_object() refuses.
Outcome<int> add_primitive(const AddPrimitiveArgs& args);

// ---- Printable ----

// Sets ModelInstance::printable of each instance `targets` names and of every instance of each object it names, as the
// object list's Printable cell does for an instance row and an object row. Returns those instances, each once, the
// objects' first, in the order named.
std::vector<std::pair<int, int>> set_printable(Model& model, const Targets& targets, bool printable);

struct SetPrintableArgs
{
    Targets targets; // objects and instances
    bool    printable = true;
};
// Marks the targets printable or not (set_printable()), as the Printable cell does, in one undo step; the object list,
// the 3D view and the slicing take the change. Returns the instances set.
Outcome<std::vector<std::pair<int, int>>> set_objects_printable(const SetPrintableArgs& args);

// ---- Auto Drop ----

// Sets ModelObject::auto_drop of each object `targets` names or names an instance or a part of, which all its instances
// share. Returns those objects, each once, in the order named.
std::vector<int> set_auto_drop(Model& model, const Targets& targets, bool auto_drop);

struct SetAutoDropArgs
{
    Targets targets;
    bool    auto_drop = true;
};
// Turns Auto Drop on or off for the targets' objects (set_auto_drop()), as the 3D view's Auto Drop does
// (Selection::set_auto_drop()), in one undo step. An object it is turned on for drops at its next move. Returns the
// objects set.
Outcome<std::vector<int>> set_objects_auto_drop(const SetAutoDropArgs& args);

// ---- Fix Model ----

// The mesh errors of an object's parts, as the object list's warning icon reads them
// (ModelObject::get_object_stl_stats()): the open edges left, and what the loader or a repair fixed.
struct MeshErrors
{
    int  open_edges        = 0;
    bool manifold          = true; // no open edge
    int  degenerate_facets = 0;
    int  edges_fixed       = 0;
    int  facets_removed    = 0;
    int  facets_reversed   = 0;
    int  backwards_edges   = 0;
};
MeshErrors mesh_errors(const ModelObject& object);

struct FixObjectsArgs
{
    Targets            targets;     // whole objects, and parts of objects
    std::optional<int> plate;       // in place of the targets, the objects on this plate, as Fix All on the current plate
    bool               all = false; // in place of the targets, every object
};
// One object, or one part of an object, as Fix Model repaired it.
struct FixedModel
{
    int         object = 0;
    int         part   = -1; // -1: the whole object
    std::string name;        // the object's, or the part's
    bool        changed = false;
    std::string failed; // why its meshes, or some of them, could not be repaired; empty when they could
    MeshErrors  before, after; // the object's
    bool        painting_removed = false;
    bool        cancelled        = false; // the repair was cancelled during this model, after its mesh had changed
};
struct FixedModels
{
    // In the order repaired. A cancel ends the repair: the model it stopped is listed, cancelled set, when its mesh had
    // changed by then (a part of an object repaired before the cancel keeps its new mesh); the models after it are as
    // they were.
    std::vector<FixedModel> models;
    bool                    cancelled = false; // the repair was cancelled
};
// The progress of fix_objects(): model `model` of those it repairs, in their order, is `percent` done, the name of the
// part being repaired in `what` when it repairs several. Returning false cancels.
using FixProgress = std::function<bool(std::size_t model, int percent, const std::string& what)>;

// Repairs `models` of `model` in turn, [object, part] with part -1 for an object whole, with fix_model_with_cgal(),
// keeping painting with `keep_painting`. `changed(object, part)` is called on each model whose mesh changed, the one a
// cancel stopped among them, before its errors after are read. It needs no app: fix_objects() is this with the app's
// updates in `changed`.
FixedModels repair_models(Model& model, const std::vector<std::pair<int, int>>& models, const FixProgress& progress,
                          bool keep_painting, const std::function<void(int object, int part)>& changed);

// Repairs the targets' meshes, as Fix Model does (repair_models()), in one "Repairing model object" undo step: each
// object whole, or each part named, in turn, keeping painting with the keep_painting preference on. A model whose mesh
// changed, the one a cancel stopped too, is put back on the bed and the object list, the plates and the slicing take it.
// Opens no dialog and pushes no notification: `progress` is told how it goes, from the UI thread, while the repair runs
// on a worker thread.
// Refuses: gizmo_open (a gizmo is open), no_such_plate, no_objects (a plate or a project with none).
Outcome<FixedModels> fix_objects(const FixObjectsArgs& args, const FixProgress& progress);

// ---- Clone ----

struct CloneObjectsArgs
{
    Targets             targets;   // objects, and instances of one; read from the selection, the selection as it is
    int                 count = 1; // copies of each
    std::optional<bool> arrange;   // arrange the current plate afterwards; none: the Clone dialog's choice, the auto_arrange
                                   // preference
};
struct ClonedObjects
{
    Targets objects;          // the new objects
    bool    arranged = false; // the current plate's arrangement was started
};
// Pastes `count` copies of the targets as new objects, added after the others near the originals, as the Clone dialog does
// (Selection::clone()), in one "Selection-clone" undo step, the last copies left selected; then with arrange starts the
// arrangement of the current plate, as the dialog does, in the background (the UI job worker is busy until it ends). The
// 3D view does not redraw while it pastes. `pasted(done, count)` is called after each copy, and returning false undoes them.
// Refuses: cancelled (pasted returned false).
Outcome<ClonedObjects> clone_objects(const CloneObjectsArgs& args, const std::function<bool(int done, int count)>& pasted = {});

// ---- Reload from disk ----

// Whether Reload from disk reloads `volume`: it was loaded from a file that has an extension, and is no built-in model.
bool has_reload_source(const ModelVolume& volume);

// The files the parts of `targets` (an object's every part, or the parts named) are reloaded from, as Reload from disk
// finds them, `exists` saying whether a file is there: a part's source file, else the file of that name beside its
// object's file. A part has_reload_source() is false for is left out.
struct ReloadSources
{
    std::vector<std::pair<int, int>> parts;   // [object, part] of the parts whose file is there
    std::vector<std::string>         paths;   // the files they load from, each once, sorted
    std::vector<std::pair<int, int>> missing; // [object, part] of the parts whose file is not there
};
ReloadSources reload_sources(const Model& model, const Targets& targets, const std::function<bool(const std::string& path)>& exists);

struct ReloadFromDiskArgs
{
    Targets targets;     // objects and parts
    bool    all = false; // in place of the targets, every object but the cut ones, as Reload All does
};
struct ReloadedParts
{
    std::vector<std::pair<int, int>> reloaded;   // the parts whose mesh the file replaced
    std::vector<std::pair<int, int>> missing;    // the parts whose file is not there, which no file dialog asks for
    std::vector<std::pair<int, int>> unmatched;  // the parts their file holds no part to take for
    std::string                      unreadable; // a file that could not be read, after which nothing more was loaded
};
// Reloads the targets' parts from their files (reload_sources()), as Reload from disk does
// (Plater::reload_parts_from_disk()), in one "Reload from disk" undo step: each keeps its settings, type, filament and
// placement, and its painting with the keep_painting preference on. A part whose file is gone is listed as missing; no
// dialog opens. A call that reloads nothing, every part's file being missing, takes no undo step.
// Refuses: gizmo_open (a gizmo is open), cut_object (a cut object named: a reload would replace a cut part with its whole
// file), no_objects (all, with none that is not cut), no_source (none of the parts was loaded from a file).
Outcome<ReloadedParts> reload_parts(const ReloadFromDiskArgs& args);

// ---- Replace 3D file ----

// What keeps the model loaded from a file from replacing a part, or none: more than one object, or one object of more than
// one part (several_parts), as Replace 3D file refuses it.
std::optional<Refusal> replacement_refusal(const Model& loaded);

// The part `targets` names, [object, part]: the part named, or the one part of the object named, as Replace 3D file takes a
// selected object of one part. Refuses several_parts for an object of several parts.
Outcome<std::pair<int, int>> replaced_part(const Model& model, const Targets& targets);

struct ReplacePartArgs
{
    int         object = 0;
    int         part   = 0;
    std::string path;
};
// Replaces the part's mesh with the mesh of the file at `path`, as Replace 3D file does once its file is picked
// (Plater::replace_part_with_file()), in one "Replace with 3D file" undo step: the part keeps its settings, type, filament
// and placement, and its painting with the keep_painting preference on, else the painting as it is. The loader asks a STEP
// file's mesh options as it does for an import. No message box tells a file of several parts. Gives the new part's index:
// the replacement sorts the object's parts by type, which can move it.
// Refuses: gizmo_open, cut_object, not_loaded (the file could not be read), cancelled (the STEP mesh options were
// cancelled), several_parts (replacement_refusal()).
Outcome<int> replace_part(const ReplacePartArgs& args);

// Brings the app up to date with instance `instance` of object `object`, placed anew in the model: with `drop` and the
// object's Auto Drop on, the instance alone set on the bed by its lowest point, its plate registered (slicing and the object list read the plate from the registry, which a move on the
// canvas updates) and the object reloaded. UI thread.
void placed_instance(int object, int instance, bool drop);

}}} // namespace Slic3r::GUI::Ops
