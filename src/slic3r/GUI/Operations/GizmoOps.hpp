#pragma once

#include "ObjectOps.hpp"
#include "Outcome.hpp"

#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"

#include <optional>

// Opening a gizmo of the Prepare view on what a command names: the operation the gizmo commands (object_paint,
// object_measure, object_simplify, part_boolean, and the openings of the commands that also act from arguments) share.
// No gizmo behaves differently for being opened here: the operation selects as a click on the model would, asks the gizmo
// whether it can open, and opens it as its toolbar icon does.
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

}}} // namespace Slic3r::GUI::Ops
