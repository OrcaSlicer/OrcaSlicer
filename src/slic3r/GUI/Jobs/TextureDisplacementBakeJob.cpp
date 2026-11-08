#include "TextureDisplacementBakeJob.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <string>
#include <vector>
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Point.hpp"
#include <exception>
#include <cstddef>
#include <cstdint>
#include "slic3r/GUI/Jobs/Worker.hpp"
#include <memory>

#include "libslic3r/Model.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <array>

namespace Slic3r::GUI {

TextureDisplacementBakeJob::TextureDisplacementBakeJob(TextureDisplacementBakeInput &&input, std::function<void()> on_finished)
    : m_input(std::move(input)), m_on_finished(std::move(on_finished))
{
}

void TextureDisplacementBakeJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Baking texture displacement");
    ctl.update_status(1, status);

    // Only ever touches m_input (captured by value before this job was queued) and local state -
    // never the live Model - so this is safe to run concurrently with the UI thread.
    //
    // The progress hook matters for more than cosmetics: the framework's progress notification only
    // grows a close button once it reaches 100%, so a job that reports 0 and nothing else leaves an
    // uncloseable notification pinned on screen. It also carries the Cancel button's effect into the
    // bake, which on a subdivided mesh can run for several seconds.
    // Colour, when any layer asks for it, is computed in the same pass as the displacement: both need
    // the same per-layer projection and UV work, and doing it twice would double the expensive part.
    //
    // The quantizers are built once for the whole job rather than per part: each fills a colour lookup
    // cube that costs tens of milliseconds, and the palette is the object's, not the part's.
    TextureColorRequest  color_request;
    TextureColorRequest *color = nullptr;
    if (!m_input.color.empty()) {
        color_request.quantize = GLGizmoTextureDisplacement::make_palette_quantizer(m_input.color.palette);
        if (!m_input.color.palette_pure.empty())
            color_request.quantize_pure = GLGizmoTextureDisplacement::make_palette_quantizer(m_input.color.palette_pure);
        color_request.despeckle_passes = m_input.color.despeckle_passes;
        if (color_request.quantize)
            color = &color_request;
    }

    // Each part gets its own slice of the 1..100 range, in order, so one progress bar covers the whole
    // bake however many parts it displaces.
    const size_t count = m_input.volumes.size();
    m_results.resize(count);
    int last_reported = 1;
    for (size_t i = 0; i < count; ++i) {
        const TextureDisplacementBakeVolume &volume = m_input.volumes[i];
        if (color != nullptr)
            color_request.out_triangle = &m_results[i].triangle_color;
        m_results[i].mesh = TriangleMesh(build_texture_displacement(
            volume.base_mesh, volume.layers, volume.facets_data, volume.options,
            [&ctl, &status, &last_reported, i, count](int percent) {
                if (ctl.was_canceled())
                    return false;
                // The notification repaints (and wakes the idle loop) on every call, so only push a
                // message when the displayed integer percentage actually moves.
                const int overall = int((i * 100 + size_t(std::max(0, percent))) / count);
                if (overall > last_reported) {
                    last_reported = overall;
                    ctl.update_status(overall, status);
                }
                return true;
            },
            color, volume.volume_to_world, nullptr, &m_results[i].stats,
            volume.model_uv.empty() ? nullptr : &volume.model_uv, &volume.texture_anchor, volume.resting_drop));
        if (ctl.was_canceled())
            break;
    }

    // Always finish at 100: this is what closes the notification. Reported even on cancel, where
    // build_texture_displacement() returns an empty mesh and finalize() commits nothing.
    ctl.update_status(100, status);
}

void TextureDisplacementBakeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    struct OnExit
    {
        std::function<void()> fn;
        ~OnExit() { if (fn) fn(); }
    } on_exit{m_on_finished};

    if (canceled || eptr)
        return;

    // A bake that moved nothing - no layer could be sampled, or every sample was zero - must not be
    // committed: committing is what clears the baked layers' paint, so the user would see the painted
    // region simply vanish with no relief in its place and no idea why. Keep the paint and say so.
    // Judged per part, because on a multibody object only some parts may carry a layer that displaces;
    // the ones that did move are committed and the rest are left exactly as they were.
    const auto moved = [this](size_t i) {
        const TriangleMesh &result = m_results[i].mesh;
        if (result.empty())
            return false;
        const indexed_triangle_set &out  = result.its;
        const indexed_triangle_set &base = m_input.volumes[i].base_mesh;
        bool unchanged = out.indices.size() == base.indices.size() && out.vertices.size() == base.vertices.size();
        for (size_t v = 0; unchanged && v < out.vertices.size(); ++v)
            unchanged = (out.vertices[v] - base.vertices[v]).cwiseAbs().maxCoeff() < 1e-5f;
        return !unchanged;
    };

    std::vector<size_t> to_commit;
    for (size_t i = 0; i < m_results.size(); ++i)
        if (moved(i))
            to_commit.push_back(i);
    if (to_commit.empty()) {
        show_error(nullptr, _u8L("The bake produced no displacement, so nothing was changed and the paint was kept. "
                                 "Check that the painted layer has a texture and a non-zero depth."));
        return;
    }

    Plater *plater = wxGetApp().plater();

    // The filament each palette entry prints in. This is where a mix becomes a mixed filament slot,
    // and only a mix the bake actually painted with: the preview never creates one, so the project
    // gains only the slots this result needs. Decided once for the whole bake, over the entries every
    // part used between them, so the same mix cannot end up in two slots. Done before anything below
    // names them - adding a slot runs ModelVolume::update_extruder_count(), which clamps paint above
    // the old filament count.
    const auto filaments_for_palette = [this, plater, &to_commit]() {
        // palette_filaments() only cares *which* entries appear, so the union of the parts' entries
        // stands in for their concatenated per-triangle colours.
        std::vector<char>    seen(m_input.color.palette.size() + 1, 0);
        std::vector<uint8_t> used;
        for (const size_t i : to_commit)
            for (const uint8_t entry : m_results[i].triangle_color)
                if (entry > 0 && size_t(entry) < seen.size() && !seen[size_t(entry)]) {
                    seen[size_t(entry)] = 1;
                    used.push_back(entry);
                }
        Sidebar &sidebar = plater->sidebar();
        return GLGizmoTextureDisplacement::palette_filaments(
            m_input.color.palette, used, [&sidebar](const PrintableColor &mix) {
                return sidebar.ensure_mixed_filament({ unsigned(mix.a + 1), unsigned(mix.b + 1) },
                                                     { mix.a_percent(), 100 - mix.a_percent() });
            });
    };

    const auto commit = [this, plater, &to_commit, &filaments_for_palette]() {
        const std::vector<int> filament_of = filaments_for_palette();
        ModelObject           *object      = nullptr;

        for (const size_t i : to_commit) {
            ModelVolume *volume = get_model_volume(m_input.volumes[i].volume_id, plater->model().objects);
            // The lookup doubles as a staleness check: anything that replaces a volume's mesh also gives it
            // a new id, so a part replaced since this bake started - by an undo, another bake, or a
            // boolean - simply fails to find its volume and is skipped.
            if (volume == nullptr)
                continue;
            const TextureDisplacementBakeVolume &input          = m_input.volumes[i];
            const std::vector<uint8_t>          &triangle_color = m_results[i].triangle_color;

            // Whether the bake replaced the triangles or only moved the vertices it was given. The classic
            // path is topology-preserving by contract, so every per-triangle annotation on the volume still
            // means what it did; the default pipeline refines, cuts and simplifies, so none of them do.
            // Both are checked: the flag is what the bake promised, the count is what it delivered. Only
            // the triangles matter - the annotations are per facet and reference no vertex index - so a
            // vertex count that changed on its own (the bake compacts unreferenced vertices away) is not a
            // reason to remap anything.
            const bool topology_changed = input.options.pipeline_v2 ||
                                          m_results[i].mesh.its.indices.size() != input.base_mesh.indices.size();

            // Carry the volume's existing paint - the model's own colours above all - across that rebuild.
            // Without it a bake left the mmu_segmentation_facets bitstream pointing at a triangle set that
            // no longer existed, which is what scrambled the colours of an imported coloured model even
            // with colour switched off on every layer and nothing in the bake that was meant to touch them.
            //
            // transfer_facet_paint() rather than ModelVolume::restore_painting(): the latter's remap wants
            // a target triangle overlapping the source paint to within 0.01 mm, which the baked surface is
            // nowhere near - it has moved by the relief depth. Nothing to do when the topology is intact:
            // triangle i is still triangle i, so the paint is already exactly right and carrying it at all
            // could only lose detail.
            std::array<TriangleSelector::TriangleSplittingData, 4> before;
            if (topology_changed) {
                before[0] = volume->supported_facets.get_data();
                before[1] = volume->seam_facets.get_data();
                before[2] = volume->mmu_segmentation_facets.get_data();
                before[3] = volume->fuzzy_skin_facets.get_data();
            }

            volume->set_mesh(std::move(m_results[i].mesh));
            volume->set_new_unique_id();
            volume->calculate_convex_hull();

            if (topology_changed) {
                const std::array<FacetsAnnotation *, 4> target{ &volume->supported_facets, &volume->seam_facets,
                                                                &volume->mmu_segmentation_facets,
                                                                &volume->fuzzy_skin_facets };
                for (size_t ch = 0; ch < target.size(); ++ch) {
                    const std::vector<uint8_t> state =
                        transfer_facet_paint(input.base_mesh, before[ch], volume->mesh().its);
                    // Nothing was painted in this channel, or there is nothing to paint it onto: either
                    // way the annotation must not keep describing the mesh that is gone.
                    if (state.empty()) {
                        target[ch]->reset();
                        continue;
                    }
                    TriangleSelector selector(volume->mesh());
                    bool             any = false;
                    for (size_t t = 0; t < state.size(); ++t)
                        if (state[t] != 0) {
                            selector.set_facet(int(t), EnforcerBlockerType(state[t]));
                            any = true;
                        }
                    if (any)
                        target[ch]->set(selector);
                    else
                        target[ch]->reset();
                }
            }

            // Colour lands in mmu_segmentation_facets, merged *over* whatever is already painted there
            // rather than replacing it: a triangle the texture does not colour keeps its existing filament,
            // and one the user never painted at all stays at NONE, which already means "the volume's own
            // filament". That is what confines the effect to the painted area without having to invent a
            // colour for everything outside it. What was already painted is whatever the transfer above
            // left in the annotation, so this bake's colour lands on top of it rather than instead of it.
            if (!triangle_color.empty() && triangle_color.size() == volume->mesh().its.indices.size()) {
                TriangleSelector selector(volume->mesh());
                const TriangleSelector::TriangleSplittingData &existing = volume->mmu_segmentation_facets.get_data();
                if (!existing.bitstream.empty())
                    selector.deserialize(existing, false);
                for (size_t t = 0; t < triangle_color.size(); ++t) {
                    const size_t entry = size_t(triangle_color[t]);
                    if (entry > 0 && entry <= filament_of.size() && filament_of[entry - 1] >= 0)
                        selector.set_facet(int(t), EnforcerBlockerType(filament_of[entry - 1] + 1));
                }
                volume->mmu_segmentation_facets.set(selector);
            }

            // Clear the paint mask of every layer that was actually baked so a repeat bake (or the paint
            // overlay) doesn't act on triangles that no longer represent the same unbaked surface. The
            // texture layer definitions themselves are left untouched so the user can keep sculpting with
            // the same textures.
            //
            // A mask the bake did *not* consume only still means what it did if the topology is unchanged,
            // which is true of the classic path (it moves existing vertices) but not of the one-run
            // pipeline, which rebuilds and then simplifies the mesh. A mask left behind against the old
            // topology is exactly what makes the gizmo reload an empty selector over a non-empty mask and
            // then erase it on the next flush - see GLGizmoTextureDisplacement::update_from_model_object().
            for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
                const auto it = std::find_if(input.layers.begin(), input.layers.end(),
                                             [slot](const TextureDisplacementLayer &l) { return l.slot == slot; });
                if (topology_changed || (it != input.layers.end() && !it->empty()))
                    volume->texture_displacement_facet(slot).reset();
            }

            // The model's own UV channel. Resampled onto a rebuilt mesh from the surface the bake started
            // from: a displaced vertex came from its foot point on that surface, which is the position its
            // uv belongs to - so no distance limit, the relief depth is exactly how far it has moved.
            if (!input.model_uv.empty())
                volume->texture_uv = topology_changed ? transfer_uv_channel(input.base_mesh, input.model_uv,
                                                                            volume->mesh().its) :
                                                         input.model_uv;

            if (object == nullptr)
                object = volume->get_object();
        }

        if (object == nullptr)
            return;

        if (ObjectList *obj_list = wxGetApp().obj_list()) {
            const ModelObjectPtrs &objs = plater->model().objects;
            auto it = std::find(objs.begin(), objs.end(), object);
            if (it != objs.end())
                obj_list->update_info_items(size_t(it - objs.begin()));
        }

        plater->changed_object(*object);
    };

    // Standard mode's Bake is remesh -> subdivide -> displace under a single snapshot, and this job runs
    // long after that snapshot's scope has closed. Adding one here would put an undo step *between* the
    // subdivision and the displacement: the first Undo would land on a mesh carrying every added
    // triangle and no relief at all, and pressing Bake again from there would subdivide that mesh a
    // second time. So the caller says who owns the undo step.
    if (m_input.take_snapshot) {
        Plater::TakeSnapshot snapshot(plater, _u8L("Bake texture displacement"), UndoRedo::SnapshotType::GizmoAction);
        commit();
    } else {
        commit();
    }

    // The refinement went finer than the budget could keep: the simplification had to take detail back
    // out to fit, so what was baked carries less of the texture than the resolution asked for. Said
    // here, with the numbers, because it is the only place that knows them - and not as an error
    // dialog: the result is a usable mesh, just not the one the settings described. Summed over the
    // parts that hit it, since the budget is spent per part and one message is enough.
    size_t refined = 0, budget = 0;
    for (const size_t i : to_commit)
        if (m_results[i].stats.budget_limited) {
            refined += m_results[i].stats.triangles_refined;
            budget  += m_results[i].stats.triangles_budget;
        }
    if (refined > 0) {
        // Thousands under a million, so a small budget is not reported as "0.1 M".
        const auto count = [](size_t n) {
            return n >= 1000000 ? Slic3r::format("%1$.1f M", double(n) / 1000000.) :
                                  Slic3r::format("%1% k", (n + 500) / 1000);
        };
        wxGetApp().notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
            Slic3r::format(_u8L("The triangle budget limited the detail: this resolution needs %1% triangles, "
                                "the budget kept %2%. Raise Budget or use a coarser Resolution for the full detail."),
                           count(refined), count(budget)));
    }
}

TextureDisplacementBakeVolume texture_displacement_bake_volume(const ModelVolume                           &volume,
                                                               const std::vector<TextureDisplacementLayer> &layers,
                                                               const TextureDisplacementOptions            &options,
                                                               const Vec3d                                 &texture_anchor,
                                                               float                                        resting_drop)
{
    TextureDisplacementBakeVolume out;
    out.volume_id       = volume.id();
    out.base_mesh       = volume.mesh().its;
    out.layers          = layers;
    out.options         = options;
    out.volume_to_world = texture_displacement_volume_to_world(volume);
    out.texture_anchor  = texture_anchor;
    out.resting_drop    = resting_drop;
    if (volume.has_texture_uv())
        out.model_uv = volume.texture_uv;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        out.facets_data[size_t(i)] = volume.texture_displacement_facet(i).get_data();
    return out;
}

void queue_texture_displacement_bake(std::vector<TextureDisplacementBakeVolume> volumes,
                                     const TextureColorSettings &color,
                                     std::function<void()> on_finished, bool take_snapshot)
{
    TextureDisplacementBakeInput input;
    input.volumes       = std::move(volumes);
    input.color         = color;
    input.take_snapshot = take_snapshot;

    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementBakeJob>(std::move(input), std::move(on_finished)));
}

} // namespace Slic3r::GUI
