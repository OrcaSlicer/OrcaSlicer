#pragma once

#include "ObjectOps.hpp"
#include "Outcome.hpp"

#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"

#include "libslic3r/Point.hpp"
#include "libslic3r/enum_bitmask.hpp"

#include <optional>

// Declared rather than included, as the other operation headers declare them: Model.hpp reaches OCCT, which switches
// parts of windows.h off (NOUSER, NOGDI) for whatever includes windows.h after it, as wx does in a file built without
// the precompiled header, such as a unit test.
namespace Slic3r {
class ModelObject;
enum class ModelObjectCutAttribute : int; // CutUtils.hpp
} // namespace Slic3r

// Opening a gizmo of the Prepare view on what a command names: the operation the gizmo commands (object_paint,
// object_measure, object_simplify, part_boolean, and the openings of the commands that also act from arguments) share.
// No gizmo behaves differently for being opened here: the operation selects as a click on the model would, asks the gizmo
// whether it can open, and opens it as its toolbar icon does. The jobs a gizmo command does from arguments with no gizmo
// (object_cut's cut by a plane) are here too, each doing what the gizmo's own button does, through the same functions.
namespace Slic3r { namespace GUI { namespace Ops {

struct OpenGizmoArgs
{
    GLGizmosManager::EType type = GLGizmosManager::Undefined;
    Targets                targets; // from_selection: the selection as it is
};

struct OpenedGizmo
{
    GLGizmosManager::EType type = GLGizmosManager::Undefined;
    Targets                on; // what is selected once it is open: the targets named, else the selection as targets
};

// Opens the gizmo of `type` on `targets` in the Prepare view, which it shows. A gizmo of that type open on the targets
// already stays open: opening is not a toggle, where a second click on its icon would close it. Otherwise the open gizmo
// is closed, the targets are selected, the gizmo opens and the canvas draws twice, so its panel's items exist (a panel's
// first frame only sizes it). A refusal after the targets were selected puts back the selection it found, as a refused
// export puts back the current plate, and leaves no undo step of that selection; a gizmo closed on the way stays closed,
// with the step it records as it closes, as for any opening.
// Refuses: needs_one_part (Simplify, before anything changes), gizmo_refusal()'s codes, gizmo_not_opened. UI thread.
Outcome<OpenedGizmo> open_gizmo(const OpenGizmoArgs& args);

// What the gizmos' conditions read besides the selection, as the app has it now.
struct GizmoFacts
{
    bool fff            = true;  // the selected printer is an FFF printer
    int  filaments      = 1;     // filament slots
    bool layers_editing = false; // Variable layer height is editing on the Prepare canvas
    bool dowel_alone    = false; // the selected object is a cut object left as one dowel connector
};
// The facts for the Prepare view's selection. UI thread.
GizmoFacts gizmo_facts();

// Why the gizmo of `type` stays closed, or none: a fact first, whatever `activable` says, since Color Painting checks
// its printer and filaments in on_is_selectable(), which GLGizmosManager::open_gizmo() does not ask; then, with every fact
// met and `activable` (the gizmo's is_activable()) false, the selection's condition of that gizmo, the one left. Codes:
// layers_editing_on, dowel_alone, needs_fff_printer, needs_two_filaments, needs_one_instance, needs_instance_or_part,
// needs_two_parts, needs_one_part, needs_selection. Pure.
std::optional<Refusal> gizmo_refusal(GLGizmosManager::EType type, const GizmoFacts& facts, bool activable);

// Closes the gizmo open on the Prepare canvas, which writes its own state to the model as it closes, as Perform cut
// does before it cuts (GLCanvas3D::reset_all_gizmos()). UI thread.
void close_open_gizmo();

// ---- A cut by a plane, as the Cut gizmo's Perform cut does it for an object with no connectors ----

// Where a kept piece goes: left as cut, laid on its cut face, or turned upside down (the gizmo's three choices per piece).
enum class CutPlacement { AsCut, PlaceOnCut, Flip };

struct CutObjectArgs
{
    int   object = 0, instance = 0;
    Vec3d point  = Vec3d::Zero();  // a point of the plane, in the 3D view's coordinates
    Vec3d normal = Vec3d::UnitZ(); // the plane's normal; the upper piece is the one it points into
    bool  keep_upper = true, keep_lower = true;
    // The gizmo's defaults: the upper piece laid on its cut face, the lower one as cut.
    CutPlacement upper = CutPlacement::PlaceOnCut, lower = CutPlacement::AsCut;
    bool         to_parts = false; // Cut to parts: both pieces stay, as two parts of one object, with no placement
};

// What Perform cut builds for `args`: the pieces kept and placed, KeepPaint with the keep_painting preference, and
// InvalidateCutInfo for an object that has no cut id (`has_cut_id` false). Pure.
// The type is ModelObjectCutAttributes (CutUtils.hpp).
enum_bitmask<ModelObjectCutAttribute> cut_attributes(const CutObjectArgs& args, bool keep_painting, bool has_cut_id);

// Why the instance cannot be cut along the plane, or none: has_connectors (connectors are placed in the Cut gizmo, which
// cuts them), dowel_alone (a cut object left as one dowel connector, which the gizmo does not open on), and
// plane_misses_object (no vertex of the instance's parts on one side of the plane, which a zero normal meets too). The
// plane is in the model's coordinates, which are the 3D view's for an FFF printer. Pure.
std::optional<Refusal> cut_refusal(const ModelObject& object, int instance, const Vec3d& point, const Vec3d& normal);

// What GLGizmoCut3D::get_cut_matrix() gives for the plane: the translation to `point` from the instance's offset, less
// the SLA elevation `sla_shift_z`, times the turn of +Z to `normal`; a normal facing straight down is the gizmo's
// Flip, a half turn about X. Pure.
Transform3d cut_matrix(const ModelObject& object, int instance, const Vec3d& point, const Vec3d& normal, double sla_shift_z);

// Cuts the instance along the plane as Perform cut does: the refusals first, then the open gizmo closed, one undo step
// "Cut by Plane", and the object replaced by its pieces at the end of the object list, upper first. It asks nothing
// about the open edges a cut may leave (Perform cut offers to repair them). Returns the pieces' indices, or with
// to_parts the one object and its parts. UI thread.
Outcome<Targets> cut_object(const CutObjectArgs& args);

}}} // namespace Slic3r::GUI::Ops
