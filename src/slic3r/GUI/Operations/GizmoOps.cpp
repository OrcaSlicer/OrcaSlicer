#include "GizmoOps.hpp"

#include "ObjectOps.hpp"
#include "Outcome.hpp"
#include "PlateOps.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cassert>
#include <cstddef>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

using EType = GLGizmosManager::EType;

Refusal refusal(const char* code, const char* msgid) { return Refusal{RefusalKind::Precondition, code, msgid}; }

// The facts' refusals, in each gizmo's words where it has some.
Refusal layers_editing_on()
{
    return refusal("layers_editing_on", L("Close the variable layer height editing first: the Cut gizmo does not open while it is on."));
}
Refusal dowel_alone()
{
    return refusal("dowel_alone", L("The Cut gizmo does not open on a cut object that is left as a single dowel connector."));
}
Refusal needs_fff_printer() { return refusal("needs_fff_printer", L("Painting on the model is for FFF printers only.")); }
Refusal needs_two_filaments() { return refusal("needs_two_filaments", L("Color Painting needs at least two filaments.")); }

// The selection's condition of the gizmo of `type`, the one left when every fact is met and the gizmo still cannot open.
Refusal selection_refusal(EType type)
{
    switch (type) {
    case EType::MmSegmentation:
        return refusal("needs_instance_or_part", L("Select a single instance or a single part to paint in color."));
    case EType::MeshBoolean: return refusal("needs_two_parts", L("Mesh Boolean needs an object of two parts or more."));
    case EType::Simplify:
        // The gizmo's own sentence (GLGizmoSimplify::on_render_input_window()).
        return refusal("needs_one_part", L("Simplification is currently only allowed when a single part is selected"));
    case EType::Measure: return refusal("needs_selection", L("Select an object or a part to measure."));
    default: return refusal("needs_one_instance", L("Select a single instance of one object: the gizmo opens on one instance."));
    }
}

// The Prepare view's selection as the GLVolume indices it holds and the mode it reads them in, which put back select
// the same again.
struct HeldSelection
{
    Selection::EMode          mode = Selection::Instance;
    std::vector<unsigned int> volumes;
};

HeldSelection hold(const Selection& sel)
{
    const Selection::IndicesList& idxs = sel.get_volume_idxs();
    return {sel.get_mode(), std::vector<unsigned int>(idxs.begin(), idxs.end())};
}

void put_back(GLCanvas3D& canvas, const HeldSelection& held)
{
    Selection& sel = canvas.get_selection();
    if (held.volumes.empty())
        sel.clear();
    else {
        sel.set_mode(held.mode);
        sel.add_volumes(held.mode, held.volumes, true);
    }
    wxGetApp().obj_list()->update_selections();
    canvas.set_as_dirty();
}

// What `sel` holds as targets: its parts in part mode, else its instances.
Targets selected(const Selection& sel)
{
    std::set<std::pair<int, int>> instances, parts;
    for (unsigned int idx : sel.get_volume_idxs())
        if (const GLVolume* v = sel.get_volume(idx); v != nullptr && !v->is_wipe_tower) {
            instances.emplace(v->object_idx(), v->instance_idx());
            parts.emplace(v->object_idx(), v->volume_idx());
        }
    Targets out;
    out.from_selection = true;
    if (sel.get_mode() == Selection::Volume)
        out.parts.assign(parts.begin(), parts.end());
    else
        out.instances.assign(instances.begin(), instances.end());
    return out;
}

// Whether `sel` holds the volumes select_targets() selects for `targets`: an object's every instance, an instance, a part
// on instance 0. The volumes are compared, not the selection's mode: an object of one part has no part row in the
// object list, which selects its instance whole when its part is selected.
bool holds(const Selection& sel, const Model& model, const Targets& targets)
{
    using Volume = std::tuple<int, int, int>; // object, instance, part
    std::set<Volume> want;
    const auto instance = [&](int object, int index) {
        for (std::size_t v = 0; v < model.objects[std::size_t(object)]->volumes.size(); ++v)
            want.emplace(object, index, int(v));
    };
    for (int object : targets.objects)
        for (std::size_t i = 0; i < model.objects[std::size_t(object)]->instances.size(); ++i)
            instance(object, int(i));
    for (const auto& [object, index] : targets.instances)
        instance(object, index);
    for (const auto& [object, part] : targets.parts)
        want.emplace(object, 0, part);
    std::set<Volume> have;
    for (unsigned int idx : sel.get_volume_idxs()) {
        const GLVolume* v = sel.get_volume(idx);
        if (v == nullptr || v->is_wipe_tower)
            return false;
        have.emplace(v->object_idx(), v->instance_idx(), v->volume_idx());
    }
    return have == want;
}

// Whether Simplify can work on what it would open on: one volume, as GLGizmoSimplify reads its part
// (get_model_volume()), which a part named on instance 0 is.
bool one_volume(const Selection& sel, const Targets& targets)
{
    if (targets.from_selection)
        return sel.get_volume_idxs().size() == 1;
    return targets.objects.empty() && targets.instances.empty() && targets.parts.size() == 1;
}

} // namespace

std::optional<Refusal> gizmo_refusal(GLGizmosManager::EType type, const GizmoFacts& facts, bool activable)
{
    // Each gizmo's facts in the order it reads them: Cut its object before Variable layer height editing
    // (GLGizmoCut3D::on_is_activable()), the painters the printer (GLGizmoPainterBase::on_is_activable()), Color
    // Painting the printer, then the filaments (GLGizmoMmuSegmentation::on_is_selectable()).
    switch (type) {
    case EType::Cut:
        if (facts.dowel_alone)
            return dowel_alone();
        if (facts.layers_editing)
            return layers_editing_on();
        break;
    case EType::FdmSupports:
    case EType::Seam:
    case EType::FuzzySkin:
        if (!facts.fff)
            return needs_fff_printer();
        break;
    case EType::MmSegmentation:
        if (!facts.fff)
            return needs_fff_printer();
        if (facts.filaments < 2)
            return needs_two_filaments();
        break;
    default: break;
    }
    if (activable)
        return std::nullopt;
    return selection_refusal(type);
}

GizmoFacts gizmo_facts()
{
    assert(wxThread::IsMain());
    GizmoFacts  facts;
    Plater&     plater = *wxGetApp().plater();
    GLCanvas3D& canvas = *plater.get_view3D_canvas3D();
    facts.fff            = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
    facts.filaments      = wxGetApp().filaments_cnt();
    facts.layers_editing = canvas.is_layers_editing_enabled();
    // As GLGizmoCut3D::on_is_activable() reads it.
    if (const int object = canvas.get_selection().get_object_idx(); object >= 0 && std::size_t(object) < plater.model().objects.size()) {
        const ModelObject* mo = plater.model().objects[std::size_t(object)];
        facts.dowel_alone     = mo->is_cut() && mo->volumes.size() == 1 && mo->volumes[0]->is_cut_connector() &&
                            mo->volumes[0]->cut_info.connector_type == CutConnectorType::Dowel;
    }
    return facts;
}

void close_open_gizmo()
{
    assert(wxThread::IsMain());
    GLCanvas3D& canvas = *wxGetApp().plater()->get_view3D_canvas3D();
    if (canvas.get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
        canvas.reset_all_gizmos();
}

Outcome<OpenedGizmo> open_gizmo(const OpenGizmoArgs& args)
{
    assert(wxThread::IsMain());
    GLCanvas3D&      canvas = shown_prepare_canvas();
    GLGizmosManager& gizmos = canvas.get_gizmos_manager();
    Selection&       sel    = canvas.get_selection();
    GLGizmoBase*     gizmo  = gizmos.get_gizmo(args.type);
    if (gizmo == nullptr)
        return Refusal{RefusalKind::Failed, "gizmo_not_opened", L("The gizmo did not open.")};
    if (args.type == GLGizmosManager::Simplify && !one_volume(sel, args.targets))
        return selection_refusal(GLGizmosManager::Simplify);

    const Model& model = wxGetApp().plater()->model();
    const bool   named = !args.targets.from_selection;
    if (gizmos.get_current_type() == args.type && (!named || holds(sel, model, args.targets)))
        return OpenedGizmo{args.type, named ? args.targets : selected(sel)};

    const HeldSelection before = hold(sel);
    close_open_gizmo();
    std::optional<Refusal> refused;
    {
        // A selection change records an undo step of its own ("Selection-Add Instance"), which a refused call must not
        // leave behind; the gizmo's own steps, as it closes above and opens below, stay.
        Plater::SuppressSnapshots quiet(wxGetApp().plater());
        if (named)
            select_targets(args.targets);
        refused = gizmo_refusal(args.type, gizmo_facts(), gizmo->is_activable());
        if (refused && named)
            put_back(canvas, before);
    }
    if (refused)
        return *refused;
    if (!gizmos.open_gizmo(args.type)) {
        if (named) {
            Plater::SuppressSnapshots quiet(wxGetApp().plater());
            put_back(canvas, before);
        }
        return Refusal{RefusalKind::Failed, "gizmo_not_opened", L("The gizmo did not open.")};
    }
    for (int frame = 0; frame < 2; ++frame) {
        canvas.set_as_dirty();
        canvas.render();
    }
    return OpenedGizmo{args.type, named ? args.targets : selected(sel)};
}

}}} // namespace Slic3r::GUI::Ops
