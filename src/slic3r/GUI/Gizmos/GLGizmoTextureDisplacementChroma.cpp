// One of the files GLGizmoTextureDisplacement is defined across. The class is declared once, in
// GLGizmoTextureDisplacement.hpp; its members are defined in GLGizmoTextureDisplacement.cpp and in the
// siblings named after the part of the gizmo they hold - Chroma, Library, Mesh, Preview, UV. The split
// is along those lines and nothing else: no member moved between parts, and the class did not change.
// Helpers more than one part needs live in GLGizmoTextureDisplacementInternal.hpp.

// The ChromaStack half of the gizmo: a picture printed as a stack of coloured filament. Solving the
// picture into bands (chroma_solve_*), the swaps those bands become in the object's layer ranges, the
// retouch brush, the printable plate and the flat tiles, the readiness and problem reports, and every
// panel the picture card is built from.

#include "GLGizmoTextureDisplacement.hpp"

#include <Eigen/Core>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>
#include <boost/log/trivial.hpp>

#include "ColorSpaceConvert.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/ChromaSplit.hpp"
#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/ChromaDiagnostics.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TransmissionDistance.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"

#include <slic3r/GUI/AnimatedTexture.hpp>
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp" // GLToolbar::Default_Icons_Size, to match the toolbar's icon size
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GuiColor.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp" // wxGetApp().mainframe, as the projector window's parent
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "slic3r/GUI/ChromaLibraries.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/GUI/TextureProjectorFrame.hpp"
#include "slic3r/GUI/UVEditorCanvas.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPrepareJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementDebugJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPreviewJob.hpp"
#include "slic3r/GUI/Jobs/ChromaStackJob.hpp"
#include "libslic3r/ChromaProfile.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <chrono>
#include <future>
#include <cstdio>
#include <cstring>
#include <glad/gl.h>
#include <imgui.h>
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <memory>
#include "slic3r/GUI/GLTexture.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include <string>
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include <math.h>
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/IconManager.hpp"
#include <optional>
#include "libslic3r/Config.hpp"
#include "libslic3r/TextureBake/TextureBakeDebug.hpp"
#include <ratio>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <vector>
#include <utility>
#include <wx/gdicmn.h>
#include <wx/busycursor.h>
#include <wx/string.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include "GLGizmoTextureDisplacementInternal.hpp"

namespace Slic3r::GUI {

using namespace gizmo_td;

void GLGizmoTextureDisplacement::rebuild_chroma_colors(const TextureDisplacementLayer &layer)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    const auto drop = [this]() {
        m_chroma_color_model.reset();
        m_chroma_color_key = 0;
    };
    if (mo == nullptr || mv == nullptr || mo->instances.empty() || layer.chroma.bands.empty())
        return drop();

    // Before a bake the surface on screen is the preview's displaced mesh; after one the relief is in
    // the volume itself and there is no preview at all (baking clears the paint it was made from).
    const indexed_triangle_set &its = m_preview_its.indices.empty() ? mv->mesh().its : m_preview_its;
    if (its.indices.empty())
        return drop();

    // The mesh is uploaded as it is, with no per-triangle colour and no index sorting: the shader
    // reads the colour from each fragment's own height, so the geometry carries nothing about the
    // stack and only has to be rebuilt when the surface itself changes.
    size_t     key = 0;
    const auto mix = [&key](size_t v) { key = key * 1099511628211ull + v + 0x9e3779b9ull; };
    mix(reinterpret_cast<uintptr_t>(its.indices.data()));
    mix(its.indices.size());
    mix(its.vertices.size());
    if (key != m_chroma_color_key || !m_chroma_color_model.is_initialized()) {
        m_chroma_color_model.reset();
        m_chroma_color_model.init_from(its);
        m_chroma_color_key = key;
    }

}

// The shader's own debug views, by the number it switches on. Mirrors CHROMA_DBG_* in
// resources/shaders/*/chromastack.fs: the two have to be changed together, which is why the names
// here are the names there.
enum ChromaDebugView : int {
    ChromaDebugOff    = 0,
    ChromaDebugHeight = 1,
    ChromaDebugBand   = 2,
    ChromaDebugFacing = 3,
    ChromaDebugUV     = 4,
    ChromaDebugSolve  = 5,
    ChromaDebugAlbedo = 6,
    ChromaDebugCount  = 7,
};

bool GLGizmoTextureDisplacement::render_chroma_colors(const TextureDisplacementLayer &layer, bool see_through)
{
    rebuild_chroma_colors(layer);
    const ModelObject *mo     = m_c->selection_info()->model_object();
    const ModelVolume *mv     = texture_volume();
    GLShaderProgram   *shader = wxGetApp().get_shader("chromastack");
    if (mo == nullptr || mv == nullptr || shader == nullptr || !m_chroma_color_model.is_initialized() ||
        layer.chroma.bands.empty())
        return false;

    const Selection   &selection    = m_parent.get_selection();
    const size_t       inst         = size_t(std::max(0, selection.get_instance_idx()));
    const Transform3d  trafo_matrix = mo->instances[inst]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera      &camera       = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix  = camera.get_view_matrix();
    const Matrix3d     normal_matrix = trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();

    shader->start_using();
    shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix", Matrix3d(view_matrix.matrix().block(0, 0, 3, 3) * normal_matrix));
    shader->set_uniform("volume_world_matrix", trafo_matrix);
    shader->set_uniform("world_normal_matrix", Matrix3f(normal_matrix.cast<float>()));
    const ClippingPlaneDataWrapper clp_data = get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);

    // The plate's picture plane: the bottom of the object's own mesh, which the relief is measured
    // from whichever way up the plate prints - it rises out of it face up and is cut up into the plate
    // from it face down. Deliberately the *undisplaced* bounding box, because a face-up plate's relief
    // grows the box upwards, and a box that followed the preview would move the origin the shader
    // measures the stack from every time the solve changed.
    shader->set_uniform("base_z", float(mo->instance_bounding_box(inst, false).min.z()));
    shader->set_uniform("layer_height", std::max(1e-3f, layer.chroma.layer_height_mm));

    // The stack itself, for the shader to composite per fragment. The base goes into the bottom band,
    // which is the filament it prints in and which the model treats as opaque anyway.
    const int count = std::min(int(layer.chroma.bands.size()), int(CHROMA_MAX_BANDS));
    shader->set_uniform("band_count", count);
    shader->set_uniform("total_layers", float(layer.chroma.total_layers()));
    shader->set_uniform("relief_layers", float(layer.chroma.relief_layers()));
    // The layer the preview stops at. Held as a layer index rather than a fraction so it survives a
    // re-solve that changes the stack's depth, and sent as 0 - the whole print - whenever it is at or
    // past the top, which is also what an unset uniform gives.
    const int total_layers = layer.chroma.total_layers();
    shader->set_uniform("layer_limit", (m_chroma_layer_limit > 0 && m_chroma_layer_limit < total_layers) ?
                                           float(m_chroma_layer_limit) :
                                           0.f);
    // Printed picture down, the stack is read from the top: the painted face is the plate's underside
    // and the base is above it, so what a fragment is seen through is the filament over it. Only a
    // generated plate can be face down - a picture painted onto somebody's model is printed whichever
    // way up that model is - so the flag is the plate's, not the stack's.
    shader->set_uniform("stack_flipped", (layer.chroma.plate.generated &&
                                          layer.chroma.plate.orientation == ChromaOrientation::FaceDown) ?
                                             1 : 0);
    for (int i = 0; i < count; ++i) {
        const ChromaBand &band = layer.chroma.bands[size_t(i)];
        const std::string idx  = "[" + std::to_string(i) + "]";
        shader->set_uniform(("band_color" + idx).c_str(), band.filament.color);
        shader->set_uniform(("band_density" + idx).c_str(),
                            chroma_layer_density(band.filament.td_mm, layer.chroma.layer_height_mm));
        shader->set_uniform(("band_layers" + idx).c_str(),
                            float(band.layers + (i == 0 ? std::max(0, layer.chroma.base_layers) : 0)));
    }
    // The direction the stack is built along, in eye space: what the view ray's path through it is
    // measured against, and the whole reason this is composited here rather than looked up.
    const Vec3d axis = view_matrix.matrix().block(0, 0, 3, 3) * Vec3d(0., 0., 1.);
    shader->set_uniform("stack_axis", Vec3f(axis.normalized().cast<float>()));
    // How translucent the print reads, from the filament on top - the one the light has to get through
    // before any of the stack below it shows. An opaque stack must not glow at its edges.
    const float td = std::clamp(layer.chroma.bands.back().filament.td_mm, 0.5f, 12.f);
    shader->set_uniform("translucency", std::clamp((td - 1.f) / 7.f, 0.f, 1.f));
    shader->set_uniform("alpha", 1.f);
    // Whether the shader hands the stack's own transmittance to the blender as this fragment's alpha.
    // Decided in on_render rather than here, because the surface under this pass has to be left out of
    // the frame for it to show anything, and that decision comes before any of it is drawn.
    shader->set_uniform("see_through", see_through ? 1 : 0);

    // Which way the light comes, and the lamp it comes from. Set on every pass, front lit included: GL
    // leaves an unset uniform at zero, so a stack switched to back lit without these would render black,
    // and a uniform that is only correct because nobody has touched it is the kind of thing that breaks
    // the day somebody does.
    const ChromaStack &stack = layer.chroma;
    shader->set_uniform("lighting_mode", int(stack.lighting));
    shader->set_uniform("led_color", Vec3f(stack.led_color.r(), stack.led_color.g(), stack.led_color.b()));
    shader->set_uniform("led_intensity", std::max(0.f, stack.led_intensity));
    shader->set_uniform("lighting_balance", std::clamp(stack.lighting_balance, 0.f, 1.f));

    // The error overlay, and the picture placement it needs to line up with what is wrong: the same five
    // values the displacement preview computes, uploaded whether or not the overlay is on, because the
    // shader only reads them when it is and this keeps the placement in one place.
    const GLTexture *error_tex = (layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS) ?
                                     m_chroma_error_tex[size_t(layer.slot)].get() :
                                     nullptr;
    const bool       show_error = m_show_chroma_error && error_tex != nullptr;
    shader->set_uniform("error_enabled", show_error ? 1 : 0);
    shader->set_uniform("tex_anchor", Vec3f(trafo_matrix.translation().cast<float>()));
    shader->set_uniform("tiling_scale", layer.tiling_scale);
    shader->set_uniform("tex_aspect", layer_texture_aspect(layer));
    shader->set_uniform("rotation_rad", layer.rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer.offset);
    if (show_error) {
        shader->set_uniform("error_tex", 0);
        glsafe(::glActiveTexture(GL_TEXTURE0));
        glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) error_tex->get_id()));
        // Clamped rather than repeated: off the picture the shader keeps the predicted colour, and a
        // wrapped fetch would stain the far edge with the near one on the way to that test.
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    }

    // ---- Lateral scattering ----
    //
    // Light goes into the filament at one point and comes back out at another, which is why a print is
    // softer than the solve that made it and why detail finer than the plastic's own scattering length
    // never survives. The straight-through part is the walk the shader already does; this is the part
    // it cannot do from one fragment's own height.
    //
    // The radius is the depth at which the filament stops hiding what is under it - the same number
    // CHROMA_TD_OPAQUE_FRACTION encodes - because a medium that absorbs over that distance scatters
    // sideways over about the same one. It is taken from the filament on top, which is the one the
    // light has to get into and back out of. Back lit the light crosses the whole column instead, so
    // the radius is the stack's own depth, which is exactly why a lithophane loses fine detail.
    const GLTexture *scatter_tex = (layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS) ?
                                       m_chroma_scatter_tex[size_t(layer.slot)].get() :
                                       nullptr;
    const bool show_scatter = m_show_chroma_scatter && scatter_tex != nullptr && !show_error &&
                              layer.tiling_scale > 1e-3f && m_chroma_debug == ChromaDebugOff;
    shader->set_uniform("scatter_enabled", show_scatter ? 1 : 0);
    if (show_scatter) {
        const float top_td    = std::max(0.2f, layer.chroma.bands.back().filament.td_mm);
        const float radius_mm = stack.lighting == ChromaLighting::FrontLit ?
                                    CHROMA_TD_OPAQUE_FRACTION * top_td :
                                    std::max(CHROMA_TD_OPAQUE_FRACTION * top_td, layer.chroma.total_mm());
        // Millimetres into the picture's own uv. The v axis carries the picture's aspect (see
        // picture_uv()), so the disc has to be stretched the same way or it comes out an ellipse on
        // anything that is not square.
        const float u = radius_mm / layer.tiling_scale;
        shader->set_uniform("scatter_uv", Vec2f(u, u * layer_texture_aspect(layer)));
        // How much of the light scatters rather than coming straight back. An opaque filament sends
        // almost none of it sideways, which is why this follows the same translucency the rim and the
        // wrapped terminator do.
        shader->set_uniform("scatter_strength", std::clamp((td - 1.f) / 7.f, 0.f, 1.f));
    }

    // The same image is what the "What the solve predicted" debug view draws, so it is bound whenever
    // either of them wants it rather than inside the scattering branch alone.
    if ((show_scatter || m_chroma_debug == ChromaDebugSolve) && scatter_tex != nullptr) {
        shader->set_uniform("scatter_tex", 1);
        glsafe(::glActiveTexture(GL_TEXTURE1));
        glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) scatter_tex->get_id()));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        // Back to the unit everything else in this pass assumes, so the next draw is not reading the
        // scatter texture through somebody else's sampler.
        glsafe(::glActiveTexture(GL_TEXTURE0));
    }

    // Which of the shader's own quantities replaces the colour. Set on every pass, off included: an
    // unset uniform is zero, which is CHROMA_DBG_OFF, but a view that is only correct because nobody
    // has touched the uniform is the kind of thing that breaks the day somebody does.
    shader->set_uniform("chroma_debug", m_chroma_debug);
    // A debug view with nothing solved to show would be a black model and no way to tell why, so it
    // falls back to the one view that needs no texture.
    if (m_chroma_debug == ChromaDebugSolve && scatter_tex == nullptr)
        shader->set_uniform("chroma_debug", int(ChromaDebugAlbedo));

    if (see_through) {
        // There is nothing underneath to fight with: on_render leaves the opaque surface out of the
        // frame when this pass is on, since a translucent surface drawn over an opaque copy of itself
        // blends with the copy and shows nothing through. So no polygon offset either - this pass *is*
        // the model.
        //
        // Drawn in two halves, the far side of the plate first, because blending is not commutative and
        // the blender has no idea which of two fragments is in front. A plate is a height field on a
        // slab, so along any view ray its back faces really are behind its front faces: back then front
        // is the right order here rather than an approximation of one, which is what makes a sort cheap
        // enough to do every frame.
        //
        // Only the front half writes depth. The back half must not, or it would occlude the front half
        // through its own depth test; the front half must, or every overlay drawn after this one would
        // have no surface left to sort against.
        const bool left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (left_handed)
            glsafe(::glFrontFace(GL_CW));
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glCullFace(GL_FRONT));
        glsafe(::glDepthMask(GL_FALSE));
        m_chroma_color_model.render();
        glsafe(::glCullFace(GL_BACK));
        glsafe(::glDepthMask(GL_TRUE));
        m_chroma_color_model.render();
        // Back to the state the rest of this gizmo's passes were written against: render_triangles()
        // turns culling off and never turns it on again.
        glsafe(::glDisable(GL_CULL_FACE));
        if (left_handed)
            glsafe(::glFrontFace(GL_CCW));
    } else {
        // Pulled forward and without depth writes, as the painted-colour overlay is: this is the same
        // surface as whatever is underneath it, so without the offset the two fight for the depth test.
        glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glPolygonOffset(-2.f, -2.f));
        glsafe(::glDepthMask(GL_FALSE));
        m_chroma_color_model.render();
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    }
    shader->stop_using();
    return true;
}

void GLGizmoTextureDisplacement::rebuild_chroma_sheet(const TextureDisplacementLayer &layer)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    if (mo == nullptr || mv == nullptr || mo->instances.empty() || layer.tiling_scale <= 1e-3f) {
        m_chroma_sheet.reset();
        m_chroma_sheet_key = 0;
        return;
    }

    const Selection  &selection    = m_parent.get_selection();
    const size_t      inst         = size_t(std::max(0, selection.get_instance_idx()));
    const Transform3d trafo_matrix = mo->instances[inst]->get_transformation().get_matrix() * mv->get_matrix();
    const Vec3f       anchor       = trafo_matrix.translation().cast<float>();
    const float       aspect       = layer_texture_aspect(layer);
    const float       rot          = layer.rotation_deg * float(M_PI) / 180.f;

    size_t     key = 0;
    const auto mix = [&key](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        key = key * 1099511628211ull + bits + 0x9e3779b9ull;
    };
    mix(anchor.x()); mix(anchor.y());
    mix(layer.tiling_scale); mix(aspect); mix(rot);
    mix(layer.offset.x()); mix(layer.offset.y());
    if (key == m_chroma_sheet_key && m_chroma_sheet.is_initialized())
        return;

    // picture_uv() inverted: the shader maps a world position to the picture's uv in closed form, so
    // the quad that carries a sheet is that map run backwards over the unit square. Building it this
    // way rather than from the plate's own width and height is what guarantees the sheets sit exactly
    // where the surface pass thinks the picture is, frame, offset and rotation included.
    const float cs = std::cos(rot), sn = std::sin(rot);
    const auto  corner = [&](float u, float v) {
        Vec2f t(u - layer.offset.x(), v - layer.offset.y());
        t.y() = (std::abs(aspect) > 1e-6f) ? t.y() / aspect : t.y();
        const Vec2f planar(t.x() * cs + t.y() * sn, -t.x() * sn + t.y() * cs);
        return Vec3f(anchor.x() + planar.x() * layer.tiling_scale,
                     anchor.y() - planar.y() * layer.tiling_scale, 0.f);
    };

    indexed_triangle_set quad;
    quad.vertices = { corner(0.f, 0.f), corner(1.f, 0.f), corner(1.f, 1.f), corner(0.f, 1.f) };
    // Wound so the quad faces +Z whichever way the picture is rotated. A sheet is drawn from both
    // sides, so this decides the sign of the normal and nothing else - and the normal is what the
    // shader measures the view ray's path through the layer against.
    const Vec3f n = (quad.vertices[1] - quad.vertices[0]).cross(quad.vertices[3] - quad.vertices[0]);
    if (n.z() >= 0.f)
        quad.indices = { Vec3i32(0, 1, 2), Vec3i32(0, 2, 3) };
    else
        quad.indices = { Vec3i32(0, 2, 1), Vec3i32(0, 3, 2) };

    m_chroma_sheet.reset();
    m_chroma_sheet.init_from(quad);
    m_chroma_sheet_key = key;
}

bool GLGizmoTextureDisplacement::chroma_layers_ready(const TextureDisplacementLayer &layer)
{
    const ModelObject *mo        = m_c->selection_info()->model_object();
    const GLTexture   *level_tex = (layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS) ?
                                       m_chroma_level_tex[size_t(layer.slot)].get() :
                                       nullptr;
    const ChromaStack &stack     = layer.chroma;
    if (mo == nullptr || texture_volume() == nullptr || level_tex == nullptr || mo->instances.empty() ||
        wxGetApp().get_shader("chroma_layers") == nullptr || !layer.chroma_enabled || !stack.plate.generated ||
        stack.bands.empty() || stack.relief_layers() <= 0 || stack.layer_height_mm <= 1e-3f ||
        layer.tiling_scale <= 1e-3f)
        return false;

    rebuild_chroma_sheet(layer);
    return m_chroma_sheet.is_initialized();
}

bool GLGizmoTextureDisplacement::render_chroma_layers(const TextureDisplacementLayer &layer)
{
    if (!chroma_layers_ready(layer))
        return false;

    const ModelObject *mo        = m_c->selection_info()->model_object();
    const ModelVolume *mv        = texture_volume();
    GLShaderProgram   *shader    = wxGetApp().get_shader("chroma_layers");
    const GLTexture   *level_tex = m_chroma_level_tex[size_t(layer.slot)].get();
    const ChromaStack &stack     = layer.chroma;

    const Selection   &selection    = m_parent.get_selection();
    const size_t       inst         = size_t(std::max(0, selection.get_instance_idx()));
    const Transform3d  trafo_matrix = mo->instances[inst]->get_transformation().get_matrix() * mv->get_matrix();
    const Camera      &camera       = wxGetApp().plater()->get_camera();
    const Transform3d &view_matrix  = camera.get_view_matrix();

    // Every sheet, bottom first, as the printer lays them down. Face down the bands print in reverse
    // (chroma_ranges()) and the backing goes on top of them instead of under, which is the whole of
    // what the orientation changes here: the sheets themselves are the same sheets.
    const float lh     = stack.layer_height_mm;
    const int   relief = stack.relief_layers();
    const int   base   = std::max(0, stack.base_layers);
    const bool  down   = stack.plate.orientation == ChromaOrientation::FaceDown;
    const float base_z = float(mo->instance_bounding_box(inst, false).min.z());
    const float relief_z0 = base_z + (down ? 0.f : float(base) * lh);

    struct Sheet
    {
        float    z;
        ColorRGB color;
        float    density;
        float    index;
        bool     full; // uncut by the height map: the backing
    };
    std::vector<Sheet> sheets;
    sheets.reserve(size_t(relief) + 1);

    int        printed = 0;
    const auto push_band = [&](const ChromaBand &band) {
        const float density = chroma_layer_density(band.filament.td_mm, lh);
        for (int k = 0; k < band.layers; ++k, ++printed)
            // At the middle of its own layer, which is where the filament in it is.
            sheets.push_back(Sheet{ relief_z0 + (float(printed) + 0.5f) * lh, band.filament.color, density,
                                    float(printed), false });
    };
    if (down)
        for (size_t i = stack.bands.size(); i-- > 0;)
            push_band(stack.bands[i]);
    else
        for (const ChromaBand &band : stack.bands)
            push_band(band);

    // The backing, as one sheet carrying its whole thickness. It prints in the bottom band's filament
    // and it is uncut by the picture, so nothing is lost by not slicing it: the shader works out what
    // it stops from the same density every other sheet uses, which also means a stack with no base at
    // all correctly lets the scene through instead of being given an opacity it has not got.
    //
    // Off unless the viewer asks for it, because at any real base thickness it is simply opaque, and
    // an opaque sheet behind the stack is the one thing that stops this view showing what the stack
    // itself does with the light. A base measured off the model is often a hundred layers, and the
    // picture then sits on a wall: the sheets are still there and still right, and nothing of them
    // can be seen. With it on, the view is what the print looks like on a table; with it off, it is
    // what the stack does, which is the question this view exists to answer.
    if (base > 0 && m_chroma_layers_backing)
        sheets.push_back(Sheet{ down ? relief_z0 + (float(relief) + 0.5f * float(base)) * lh :
                                       base_z + 0.5f * float(base) * lh,
                                stack.bands.front().filament.color,
                                chroma_layer_density(stack.bands.front().filament.td_mm, lh) * float(base),
                                0.f, true });

    // Back to front, which is what makes "over" the stack's own compositing rather than an arbitrary
    // order of blends. The sheets are parallel and stacked along world Z, so the camera's own up or
    // down decides it outright - there is nothing to sort.
    const bool downward = camera.is_looking_downward();
    std::sort(sheets.begin(), sheets.end(), [downward](const Sheet &a, const Sheet &b) {
        return downward ? a.z < b.z : a.z > b.z;
    });

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix", Matrix3d(view_matrix.matrix().block(0, 0, 3, 3)));
    shader->set_uniform("world_normal_matrix", Matrix3d(Matrix3d::Identity()));

    const ClippingPlaneDataWrapper clp_data = get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);

    shader->set_uniform("tex_anchor", Vec3f(trafo_matrix.translation().cast<float>()));
    shader->set_uniform("tiling_scale", layer.tiling_scale);
    shader->set_uniform("tex_aspect", layer_texture_aspect(layer));
    shader->set_uniform("rotation_rad", layer.rotation_deg * float(M_PI) / 180.f);
    shader->set_uniform("uv_offset", layer.offset);
    shader->set_uniform("relief_layers", float(relief));
    shader->set_uniform("stack_flipped", down ? 1 : 0);
    shader->set_uniform("alpha", 1.f);
    shader->set_uniform("led_color", Vec3f(stack.led_color.r(), stack.led_color.g(), stack.led_color.b()));
    shader->set_uniform("led_intensity", std::max(0.f, stack.led_intensity));

    shader->set_uniform("level_tex", 0);
    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint) level_tex->get_id()));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    const auto draw_sheet = [&](float z, const ColorRGB &color, float density, float index, bool full, bool lamp) {
        const Transform3d to_sheet = Geometry::translation_transform(Vec3d(0., 0., double(z)));
        shader->set_uniform("view_model_matrix", view_matrix * to_sheet);
        shader->set_uniform("volume_world_matrix", to_sheet);
        shader->set_uniform("sheet_color", color);
        shader->set_uniform("sheet_density", density);
        shader->set_uniform("sheet_index", index);
        shader->set_uniform("sheet_full", full ? 1 : 0);
        shader->set_uniform("sheet_lamp", lamp ? 1 : 0);
        m_chroma_sheet.render();
    };

    // No depth writes anywhere in the pass: the sheets are sorted already and must not occlude each
    // other, and anything drawn after this one has the rest of the scene to sort against. The depth
    // *test* stays on, so a plate standing behind another object is still behind it.
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glDepthMask(GL_FALSE));

    // ---- The light that went through: the lamp, then the stack multiplied into it ----
    //
    // A lithophane is not the stack composited over a background, it is the background seen through
    // the stack, so the sheets multiply the frame buffer instead of being laid over it. That is the
    // one place a per-channel transmittance can be applied with fixed-function blending - "over"
    // carries a single alpha and cannot tint what is behind the plate - and it is exactly what the
    // plastic does to the lamp's light.
    if (stack.lighting != ChromaLighting::FrontLit) {
        const float lamp_z = downward ? sheets.front().z - lh : sheets.front().z + lh;
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        shader->set_uniform("pass_mode", 0);
        shader->set_uniform("reflect_strength", 1.f);
        draw_sheet(lamp_z, ColorRGB{}, 0.f, 0.f, true, true);

        glsafe(::glBlendFunc(GL_ZERO, GL_SRC_COLOR));
        shader->set_uniform("pass_mode", 1);
        // Multiplication commutes, so this pass needs no order of its own.
        for (const Sheet &sheet : sheets)
            draw_sheet(sheet.z, sheet.color, sheet.density, sheet.index, sheet.full, false);
    }

    // ---- The light that came back out: the stack composited over whatever is behind it ----
    //
    // Lit from both sides, this goes on top of the transmitted light at the same weight
    // chroma_palette() blends the two palettes with, so the preview and the colours the solve was
    // chosen against stay the same picture.
    if (stack.lighting != ChromaLighting::BackLit) {
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        shader->set_uniform("pass_mode", 0);
        shader->set_uniform("reflect_strength", stack.lighting == ChromaLighting::FrontLit ?
                                                    1.f :
                                                    1.f - std::clamp(stack.lighting_balance, 0.f, 1.f));
        for (const Sheet &sheet : sheets)
            draw_sheet(sheet.z, sheet.color, sheet.density, sheet.index, sheet.full, false);
    }

    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDepthMask(GL_TRUE));
    shader->stop_using();
    return true;
}

TextureDisplacementLayer *GLGizmoTextureDisplacement::chroma_layer_in_slot(int slot)
{
    // By slot rather than by reference, because the callers are completion handlers: between queuing
    // a solve and its result landing the layer can have been removed, the object deselected or the
    // gizmo closed and reopened on something else.
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || slot < 0)
        return nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == slot && l.chroma_enabled)
            return &l;
    return nullptr;
}

// How long the stack has to hold still before the solve runs. Long enough that dragging a layer count
// through ten values solves once at the end, short enough that letting go feels like it answered.
static constexpr int64_t CHROMA_SETTLE_MS = 350;

// ---- ChromaStack ---------------------------------------------------------------------------------

std::vector<ChromaFilament> GLGizmoTextureDisplacement::chroma_filaments()
{
    const std::vector<ColorRGBA> colors = filament_palette();
    const PresetBundle          *bundle = wxGetApp().preset_bundle;

    std::vector<ChromaFilament> out;
    out.reserve(colors.size());
    for (size_t i = 0; i < colors.size(); ++i) {
        ChromaFilament f;
        f.filament = int(i);
        f.color    = to_rgb(colors[i]);
        // The colour comes from the project (it follows the sidebar), the TD from the filament's own
        // preset - it is a property of the material, not of this project.
        if (bundle != nullptr && i < bundle->filament_presets.size()) {
            const Preset *preset = bundle->filaments.find_preset(bundle->filament_presets[i], false);
            if (preset != nullptr) {
                f.name = preset->label(false);
                if (preset->config.has("filament_transmission_distance"))
                    f.td_mm = float(preset->config.opt_float("filament_transmission_distance", 0));

                // Nobody has said what this filament's transmission distance is, so ask the table.
                //
                // It is keyed by colour, because that is what a transmission distance is a property
                // of - one line of matte PLA runs from 0.1 to 4.1 across its colours - and a preset
                // is a whole line. So the preset cannot carry the number, but the preset and the
                // colour together are enough to find it.
                //
                // Only when the preset is still at the option's default: a number somebody typed, or
                // measured with the wedge, outranks a table every time.
                if (std::abs(f.td_mm - FILAMENT_TD_DEFAULT_MM) < 1e-4f) {
                    const std::string vendor = preset->config.has("filament_vendor") ?
                                                   preset->config.opt_string("filament_vendor", 0u) :
                                                   std::string();
                    if (const FilamentTd *found = filament_td_lookup(vendor, preset->name, f.color)) {
                        f.td_mm = found->td_mm;
                        // Where the number came from, for the card to show. The spool the table matched,
                        // and for an imported library the library too: a number out of somebody else's
                        // file and a number we measured and shipped are not the same kind of evidence,
                        // and the card would otherwise present them as one.
                        f.td_source = found->imported && !found->td_source.empty() ?
                                          found->name + " (" + found->td_source + ")" :
                                          found->name;
                    }
                }
            }
        }
        out.push_back(f);
    }
    return out;
}

float GLGizmoTextureDisplacement::print_layer_height()
{
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return 0.f;
    const DynamicPrintConfig &print = bundle->prints.get_edited_preset().config;
    return print.has("layer_height") ? float(print.opt_float("layer_height")) : 0.f;
}

GLGizmoTextureDisplacement::ChromaPatch GLGizmoTextureDisplacement::chroma_patch(const TextureDisplacementLayer &layer) const
{
    ChromaPatch      out;
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr || mo->instances.empty() || layer.slot < 0)
        return out;

    // The patch in the object's printed coordinates: a filament swap happens at a print Z, so the
    // stack has to be measured against the same frame the slicer will see, instance transform
    // included.
    const size_t      inst  = size_t(std::max(0, m_c->selection_info()->get_active_instance()));
    const Transform3d trafo = mo->instances[inst]->get_transformation().get_matrix() * mv->get_matrix();
    // The frame the bake projects the texture in, which is where tiling_scale and offset live. Not the
    // same as `trafo`: it drops the translation, so the texture rides with the model, and it is built
    // from the first instance, as the bake's own projection is.
    const Transform3d bake  = texture_displacement_bake_frame(texture_displacement_volume_to_world(*mv));
    const size_t      slot  = size_t(layer.slot);
    const uint64_t    stamp = mv->texture_displacement_facet(layer.slot).timestamp();
    if (m_chroma_patch_cache[slot].valid && m_chroma_patch_stamp[slot] == stamp &&
        m_chroma_patch_trafo[slot].isApprox(trafo) && m_chroma_patch_bake[slot].isApprox(bake))
        return m_chroma_patch_cache[slot];

    const indexed_triangle_set patch = extract_painted_patch(mv->mesh().its,
                                                             mv->texture_displacement_facet(layer.slot).get_data());
    if (patch.vertices.empty())
        return out;

    // The measurement itself is a pure function of the patch and the two frames, and lives in
    // libslic3r so the solve job can run it on a worker without reaching for the Model. This is only
    // the cache around it.
    const ChromaPatchMetrics metrics = chroma_measure_patch(patch, trafo, bake,
                                                            mo->instance_bounding_box(inst, false).min.z());
    if (!metrics.valid)
        return out;

    out.valid         = true;
    out.top_mm        = metrics.top_mm;
    out.spread_mm     = metrics.spread_mm;
    out.tilt_deg      = metrics.tilt_deg;
    out.tilt_mean_deg = metrics.tilt_mean_deg;
    out.planar_min    = metrics.planar_min;
    out.planar_max    = metrics.planar_max;

    m_chroma_patch_cache[slot] = out;
    m_chroma_patch_stamp[slot] = stamp;
    m_chroma_patch_trafo[slot] = trafo;
    m_chroma_patch_bake[slot]  = bake;
    return out;
}

ChromaSurface GLGizmoTextureDisplacement::chroma_surface(const TextureDisplacementLayer &layer, int width,
                                                         int height) const
{
    ChromaSurface      out;
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr || mo->instances.empty() || layer.slot < 0 || width <= 0 || height <= 0)
        return out;

    const indexed_triangle_set patch = extract_painted_patch(mv->mesh().its,
                                                             mv->texture_displacement_facet(layer.slot).get_data());
    if (patch.indices.empty())
        return out;

    // The same two frames chroma_patch() measures in, for the same reasons: the bake's for where the
    // picture lands, the instance's for how the surface sits on the plate.
    const size_t      inst  = size_t(std::max(0, m_c->selection_info()->get_active_instance()));
    const Transform3d trafo = mo->instances[inst]->get_transformation().get_matrix() * mv->get_matrix();
    const Transform3d bake  = texture_displacement_bake_frame(texture_displacement_volume_to_world(*mv));

    TextureDisplacementLayer probe;
    probe.image_data   = layer.chroma_source;
    const float aspect = layer_texture_aspect(probe);

    // As with chroma_patch(), the rasterisation itself is in libslic3r so a worker can run it; this
    // overload is only what still needs the live Model to find the patch and its frames.
    return chroma_build_surface(patch, trafo, bake, layer, aspect, width, height);
}

int GLGizmoTextureDisplacement::chroma_measured_base(const TextureDisplacementLayer &layer) const
{
    const ChromaPatch patch = chroma_patch(layer);
    if (!patch.valid || layer.chroma.layer_height_mm <= 0.f)
        return -1;
    return std::max(1, int(std::lround(patch.top_mm / layer.chroma.layer_height_mm)));
}

void GLGizmoTextureDisplacement::chroma_fit_picture(TextureDisplacementLayer &layer)
{
    const ChromaPatch patch = chroma_patch(layer);
    if (!patch.valid || !layer.chroma_source)
        return;
    TextureDisplacementLayer probe;
    probe.image_data   = layer.chroma_source;

    ChromaPatchMetrics metrics;
    metrics.valid      = true;
    metrics.planar_min = patch.planar_min;
    metrics.planar_max = patch.planar_max;
    chroma_fit_layer_to_patch(layer, metrics, layer_texture_aspect(probe));
}

// Turns a layer from one kind into the other, in place. Nothing is thrown away: a picture's stack is
// kept while it is a relief layer, so switching back costs nothing, and a relief layer's texture
// becomes the picture it is solved from.
void GLGizmoTextureDisplacement::chroma_convert_layer(TextureDisplacementLayer &layer, bool to_chroma)
{
    if (layer.chroma_enabled == to_chroma)
        return;
    ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;

    if (to_chroma)
        for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.chroma_enabled && l.slot != layer.slot) {
                show_error(nullptr, _u8L("This object already has a ChromaStack picture. An object can only have one: "
                                         "the filament swaps are the object's own height ranges, and there is one set "
                                         "of those."));
                return;
            }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                  to_chroma ? _u8L("Make this a ChromaStack picture") : _u8L("Make this a relief layer"),
                                  UndoRedo::SnapshotType::GizmoAction);
    layer.chroma_enabled = to_chroma;
    if (to_chroma) {
        // The texture it was displacing by becomes the picture it is solved from - which is right
        // whether that image is a photograph or a grayscale height map, since a height map is just a
        // picture with no colour in it.
        if (!layer.chroma_source)
            layer.chroma_source = layer.image_data;
        layer.color_enabled = false;
        layer.tile_enabled  = false;
        layer.invert        = false;
        layer.midlevel      = 0.f;
        layer.smoothing     = 0.f;
        layer.blend_mode    = TextureBlendMode::Add;
        chroma_fit_picture(layer);
        if (layer.chroma.bands.empty()) {
            const float layer_h  = print_layer_height();
            const int   measured = chroma_measured_base(layer);
            layer.chroma = chroma_default_stack(chroma_filaments(), layer_h > 0.f ? layer_h : 0.08f,
                                                measured > 0 ? measured : 4, CHROMA_DEFAULT_RELIEF_LAYERS);
        }
        chroma_touch(layer.slot);
    } else {
        // It stops printing in colour, so the swaps it wrote stop being wanted. Only the ones it wrote:
        // anything else on the object was never ours to remove.
        ModelObject *mo = m_c->selection_info()->model_object();
        if (mo != nullptr && !mo->layer_config_ranges.empty() && !chroma_swaps_blocked(layer)) {
            mo->layer_config_ranges.clear();
            layer.chroma.applied_ranges = 0;
            if (ObjectList *list = wxGetApp().obj_list(); list != nullptr) {
                const int obj_idx = m_parent.get_selection().get_object_idx();
                if (obj_idx >= 0)
                    list->rebuild_layer_ranges(obj_idx);
            }
        }
        m_chroma_settle_at[size_t(layer.slot)] = 0;
    }
    rebuild_preview();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::chroma_paint_face(ModelVolume &mv, int slot, size_t count)
{
    if (slot < 0 || count == 0)
        return;
    const indexed_triangle_set &its = mv.mesh().its;
    TriangleSelector            selector(mv.mesh());
    const size_t                n = std::min(count, its.indices.size());
    for (size_t i = 0; i < n; ++i)
        selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    // set() serializes the selector and stamps the volume, which is what every cache keyed on the
    // paint - the patch measurement, the overlays, undo/redo - watches.
    mv.texture_displacement_facet(slot).set(selector);
}

// Reads a closed outline out of an SVG, for a frame that is not one of the shapes a number can
// describe. The biggest contour in the file wins: an SVG of a frame is usually its silhouette plus
// whatever decoration was drawn inside it, and the silhouette is what the plate's edge is.
static bool chroma_outline_from_svg(const std::string &path, std::vector<Vec2f> &out)
{
    NSVGimage_ptr image = nsvgParseFromFile(path, "mm", 96.f);
    if (!image)
        return false;
    // The tolerance is in the parser's own squared units; 0.1 mm is what the SVG gizmo asks for and is
    // finer than anything a frame's edge needs.
    const ExPolygonsWithIds shapes = create_shape_with_ids(*image, NSVGLineParams(0.3));
    const ExPolygon        *best   = nullptr;
    double                  area   = 0.;
    for (const ExPolygonsWithId &with_id : shapes)
        for (const ExPolygon &expoly : with_id.expoly) {
            const double a = std::abs(expoly.contour.area());
            if (a > area) {
                area = a;
                best = &expoly;
            }
        }
    if (best == nullptr || best->contour.points.size() < 3)
        return false;

    out.clear();
    out.reserve(best->contour.points.size());
    for (const Point &p : best->contour.points)
        out.emplace_back(unscale<float>(p.x()), unscale<float>(p.y()));
    return true;
}

void GLGizmoTextureDisplacement::chroma_refresh_frame(TextureDisplacementLayer &layer)
{
    ChromaPlate &plate = layer.chroma.plate;
    if (plate.frame.frame_id.empty())
        return;
    const ChromaFrameDef *def = ChromaFrameLibrary::instance().find(plate.frame.frame_id);
    // The definition's file is gone - uninstalled, or a project opened on another machine. The plate
    // keeps the cross-section that was resolved when the frame was chosen, so the frame is intact and
    // only its name and its ability to be re-resolved are missing. Silently leaving it alone is the
    // right answer: the alternative is replacing the user's frame with a default one.
    if (def == nullptr)
        return;
    // Only the cross-section is re-resolved. The outline is a shape rather than a size - the mesh
    // grows it to whatever the frame's width is - so it does not depend on the numbers here, which is
    // also what lets a shape read from an SVG survive the width being changed.
    plate.frame.profile_points =
        chroma_frame_geometry(*def, plate.frame.width_mm, plate.frame.rise_mm, plate.width_mm, plate.height_mm,
                              plate.base_mm)
            .profile;
}

void GLGizmoTextureDisplacement::chroma_set_frame(int slot, const ChromaFramePick &pick)
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || slot < 0)
        return;
    TextureDisplacementLayer *layer = nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == slot && l.chroma_enabled)
            layer = &l;
    if (layer == nullptr || !layer->chroma.plate.generated)
        return;

    // The gallery's first cell. Taking the frame off is a choice made among the frames, which is where
    // a reader is already looking when they decide they do not want one.
    const ChromaFrameDef *def = pick.frame_id.empty() ? nullptr :
                                                        ChromaFrameLibrary::instance().find(pick.frame_id);
    if (def == nullptr && !pick.frame_id.empty()) {
        show_error(nullptr, _u8L("That frame could not be read."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Choose a ChromaStack frame"),
                                  UndoRedo::SnapshotType::GizmoAction);
    ChromaPlate &plate = layer->chroma.plate;
    if (def == nullptr) {
        plate.frame          = ChromaFrame{};
        plate.frame.width_mm = 0.f;
    } else {
        // The width and the rise the gallery came back with, not the definition's own: the gallery has
        // already decided whether this is a fresh pick that takes the moulding's proportions or a
        // dimension the user set and expects to keep, and deciding it again here would throw away a
        // number they had just typed.
        chroma_frame_to_plate(*def, pick.width_mm, pick.rise_mm, plate);
        plate.frame.frame_id = pick.frame_id;
        chroma_refresh_frame(*layer);
    }

    // After the frame, because chroma_frame_to_plate() writes the definition's own shape over both of
    // these: the outline is the plate's silhouette and the frame is the moulding swept around it, and
    // the one the user just read out of a file has to win.
    if (pick.svg_requested && !pick.svg_path.empty()) {
        std::vector<Vec2f> loop;
        if (chroma_outline_from_svg(pick.svg_path, loop)) {
            plate.frame.outline = std::move(loop);
            plate.frame.shape   = ChromaFrame::Shape::Outline;
        } else
            show_error(nullptr, _u8L("Could not read a closed outline from that SVG."));
    }

    m_chroma_plate_dirty[size_t(slot)] = true;
    chroma_touch(slot);
    m_parent.set_as_dirty();
}

bool GLGizmoTextureDisplacement::chroma_queue_plate(TextureDisplacementLayer &layer)
{
    if (!layer.chroma.plate.generated || layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return false;
    const size_t slot = size_t(layer.slot);
    if (m_bake_in_progress || m_prepare_in_progress)
        return false;
    // One in flight per slot. A second request while this one runs is simply dropped: the plate the
    // job is building is already out of date, and the settle timer will ask again.
    if (m_chroma_plate_running[slot])
        return false;

    ChromaPlateInput input;
    input.plate = layer.chroma.plate;

    const uint64_t generation = m_chroma_plate_generation->fetch_add(1) + 1;
    m_chroma_plate_running[slot] = true;
    // The old plate stays on screen and stays printable while this runs; the swap in the handler is
    // the only moment anything changes.
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<ChromaPlateJob>(
                          std::move(input), generation, m_chroma_plate_generation,
                          [this, slot](ChromaPlateResult result, uint64_t) {
                              m_chroma_plate_running[slot] = false;
                              chroma_apply_plate(int(slot), std::move(result));
                          }));
    return true;
}

void GLGizmoTextureDisplacement::chroma_apply_plate(int slot, ChromaPlateResult &&result)
{
    if (result.mesh.empty())
        return;
    TextureDisplacementLayer *layer  = chroma_layer_in_slot(slot);
    ModelVolume              *mv     = texture_volume();
    ModelObject              *mo     = m_c->selection_info()->model_object();
    Plater                   *plater = wxGetApp().plater();
    if (layer == nullptr || mv == nullptr || mo == nullptr || plater == nullptr)
        return;
    if (m_bake_in_progress || m_prepare_in_progress)
        return;

    // Nothing to save and restore: the triangle list is a different one. That is exactly why the
    // picture's face is built first - it can be found again by index whatever the frame turned into.
    //
    // No undo snapshot here, deliberately. This runs once per settle of the plate controls, and a
    // snapshot serialises the whole volume - including the source picture and the solved height map,
    // megabytes of PNG through a std::string copy (see TextureDisplacementLayer::save()). One per
    // keystroke in the Size field was both the slowest step in the rebuild and an undo stack nobody
    // could use. The snapshot belongs to the edit that set the numbers, not to the mesh catching up.
    mv->set_mesh(TriangleMesh(result.mesh.its));
    mv->set_new_unique_id();
    mv->calculate_convex_hull();
    chroma_paint_face(*mv, slot, result.mesh.picture_triangles);
    mo->invalidate_bounding_box();

    if (ObjectList *obj_list = wxGetApp().obj_list()) {
        const ModelObjectPtrs &objs = plater->model().objects;
        auto                   it   = std::find(objs.begin(), objs.end(), mo);
        if (it != objs.end())
            obj_list->update_info_items(size_t(it - objs.begin()));
    }
    plater->changed_object(*mo);
    update_from_model_object(false);

    // The volume was replaced, so the layer reference above may have been invalidated with it.
    layer = chroma_layer_in_slot(slot);
    if (layer == nullptr)
        return;

    // The picture has to be laid over the new face, and the swaps follow the new base height. Neither
    // is a solve: see chroma_solve_key() for why the plate cannot change what the solver picks.
    chroma_fit_picture(*layer);
    if (layer->chroma.base_auto)
        if (const int measured = chroma_measured_base(*layer); measured > 0)
            layer->chroma.base_layers = measured;
    chroma_apply_swaps(*layer, true);

    // Only if something really did change what the solve would answer - a tilted patch, or a stack
    // edit that arrived in the same settle.
    if (!layer->image_data || chroma_solve_key(*layer) != m_chroma_solve_key[size_t(slot)])
        chroma_solve_layer(*layer, true);
    else
        rebuild_preview(); // the mesh under the relief is new even when the relief is not

    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::create_picture_plate()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    const wxString wildcard = _L("Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp");
    wxFileDialog   dialog(nullptr, _L("Choose the picture to print in color"), wxEmptyString, wxEmptyString, wildcard,
                          wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    const std::string path = into_u8(dialog.GetPath());
    std::string       error;
    std::shared_ptr<std::vector<unsigned char>> bytes = load_texture_image_data(path, error);
    if (!bytes) {
        show_error(nullptr, error);
        return;
    }

    // The plate takes its proportions from the picture: a photograph squeezed into a square is not
    // the photograph. The longer side is what the default size means, so a panorama and a portrait
    // both come out the same size on the plate rather than one of them being tiny.
    TextureDisplacementLayer probe;
    probe.image_data   = bytes;
    const float aspect = layer_texture_aspect(probe);
    ChromaPlate plate;
    plate.generated = true;
    if (aspect >= 1.f) {
        plate.width_mm  = CHROMA_PLATE_DEFAULT_MM;
        plate.height_mm = CHROMA_PLATE_DEFAULT_MM / std::max(0.05f, aspect);
    } else {
        plate.height_mm = CHROMA_PLATE_DEFAULT_MM;
        plate.width_mm  = CHROMA_PLATE_DEFAULT_MM * std::max(0.05f, aspect);
    }

    const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
    if (mesh.empty()) {
        show_error(nullptr, _u8L("Could not build a plate for this picture."));
        return;
    }

    const std::string name = boost::filesystem::path(path).stem().string();
    Plater::TakeSnapshot snapshot(plater, _u8L("Add ChromaStack picture"));
    // The object, the instance, the placement on an empty cell and the selection, all of it.
    wxGetApp().obj_list()->load_mesh_object(TriangleMesh(mesh.its), from_u8(name));
    wxGetApp().mainframe->update_title();
    if (plater->model().objects.empty())
        return;
    ModelObject *mo = plater->model().objects.back();
    if (mo->volumes.empty())
        return;
    ModelVolume *mv = mo->volumes.front();

    // Painted after the mesh is final: the mask is indexed by triangle, so anything that replaces the
    // mesh afterwards drops it.
    chroma_paint_face(*mv, 0, mesh.picture_triangles);

    TextureDisplacementLayer layer;
    layer.slot           = 0;
    layer.chroma_enabled = true;
    layer.chroma_source  = bytes;
    layer.name           = name;
    layer.path           = path;
    layer.color_enabled  = false;
    layer.tile_enabled   = false;
    layer.invert         = false;
    layer.midlevel       = 0.f;
    layer.smoothing      = 0.f;
    layer.blend_mode     = TextureBlendMode::Add;

    const float layer_h = print_layer_height() > 0.f ? print_layer_height() : 0.08f;
    layer.chroma = chroma_default_stack(chroma_filaments(), layer_h,
                                        std::max(1, int(std::lround(double(plate.base_mm) / double(layer_h)))),
                                        CHROMA_DEFAULT_RELIEF_LAYERS);
    // After the stack, because seeding one replaces it wholesale.
    layer.chroma.plate      = plate;
    layer.chroma.base_auto  = true;
    mv->texture_displacement_layers.push_back(std::move(layer));
    mv->texture_displacement_options.active_slot = 0;

    // Open on what was just made. The gizmo reloads the volume as it opens, so the layer has to be
    // in place first, and the solve has to come after - it reads the paint through the selectors the
    // gizmo builds while opening.
    GLGizmosManager &manager = m_parent.get_gizmos_manager();
    if (manager.get_current_type() != GLGizmosManager::EType::TextureDisplacement)
        manager.open_gizmo(GLGizmosManager::EType::TextureDisplacement);
    plater->changed_object(*mo);

    if (TextureDisplacementLayer *opened = active_layer()) {
        chroma_fit_picture(*opened);
        chroma_solve_layer(*opened, true);
    }
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::chroma_pick_picture(TextureDisplacementLayer &layer)
{
    if (layer.slot < 0)
        return;
    // The gallery is a modal wx dialog and this is called from inside the ImGui pass, which would run a
    // second event loop under the frame being drawn. Deferred to the next idle, the way the texture
    // picker defers its own dialogs, and the layer is found again by slot on the other side: the panel,
    // the selection and the layer list all go on living in between.
    const int         slot    = layer.slot;
    const std::string current = layer.path;
    wxGetApp().CallAfter([this, slot, current]() {
        if (const std::optional<std::string> picked = pick_chroma_picture(nullptr, current); picked)
            chroma_set_picture(slot, *picked);
    });
}

void GLGizmoTextureDisplacement::chroma_set_picture(int slot, const std::string &path)
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr || slot < 0)
        return;
    TextureDisplacementLayer *found = nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == slot)
            found = &l;
    if (found == nullptr)
        return;
    TextureDisplacementLayer &layer = *found;

    std::string error;
    // The same loader the texture library uses, which keeps the image's colour - which is the whole
    // input to the solve, so a grayscale-only import would defeat the feature.
    std::shared_ptr<std::vector<unsigned char>> bytes = load_texture_image_data(path, error);
    if (!bytes) {
        show_error(nullptr, error);
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Set ChromaStack picture"), UndoRedo::SnapshotType::GizmoAction);
    layer.chroma_source  = std::move(bytes);
    layer.chroma_enabled = true;
    layer.name             = boost::filesystem::path(path).stem().string();
    layer.path             = path;
    // Colour by filament swap, never per triangle: the two mechanisms would fight over the same
    // surface. See TextureDisplacementLayer::chroma_enabled.
    layer.color_enabled = false;
    // One copy of the picture over the patch, not a repeating tile.
    layer.tile_enabled = false;
    layer.invert       = false;
    layer.midlevel     = 0.f;

    chroma_fit_picture(layer);

    if (layer.chroma.bands.empty()) {
        const float layer_h = print_layer_height();
        // The base is whatever the model already has under the painted face, in layers: the relief is
        // printed on top of it, so this is a measurement rather than a choice.
        const int measured = chroma_measured_base(layer);
        const int base     = measured > 0 ? measured : 4;
        layer.chroma = chroma_default_stack(chroma_filaments(), layer_h > 0.f ? layer_h : 0.08f, base,
                                            CHROMA_DEFAULT_RELIEF_LAYERS);
    }
    chroma_solve_layer(layer);
    // Deferred out of the ImGui pass, so nothing is repainting on its own account when this returns.
    m_parent.set_as_dirty();
}

// How fine the live solve works, as the longer side in texels. The height map it produces drives a
// mesh refined to a fraction of a millimetre (recommend_v2_resolution() puts the edge at the model's
// diagonal over 250), so a few hundred texels across already oversamples every vertex the relief has;
// solving a 12 MP photograph texel for texel on every edit bought nothing but the wait. A bake solves
// the picture properly - see chroma_solve_layer_blocking().
static constexpr int CHROMA_SOLVE_PREVIEW_PX = 1024;

// The finest resolution a bake has any reason to solve at, as the longer side in texels.
//
// The live path is capped at CHROMA_SOLVE_PREVIEW_PX so the panel keeps up, while the bake took the
// picture at whatever resolution it happened to have. For a phone photograph on a postcard-sized
// plate that is about twenty times what the print can express, and all of it is paid for twice: once
// in the match, once in the height map the bake then samples.
//
// What bounds the colour detail is the extrusion rather than the mesh. A region narrower than the
// nozzle cannot be printed in a colour of its own at all - the slicer drops it, which is the same
// reason a dithered single-texel column is a lie - so two texels per nozzle width is already past the
// point where another texel changes anything printable. The cap never goes below the live resolution,
// because a bake must not be coarser than the preview the user approved, and chroma_solve_input()
// only ever downsamples, so a picture finer than the cap is reduced and one coarser than it is left
// exactly as it is.
static int chroma_printable_solve_px(float picture_mm, float nozzle_mm)
{
    if (picture_mm <= 0.f || nozzle_mm <= 0.f)
        return 0; // nothing measured to go on, so the picture's own resolution stands
    return std::max(CHROMA_SOLVE_PREVIEW_PX, int(std::ceil(picture_mm / (0.5f * nozzle_mm))));
}

// What the printer can actually draw, which is what bounds both how finely a picture is worth solving
// and how small a dithered speck may be. Zeroes when there is no printer or print profile to ask,
// which every caller reads as "do not bother".
struct ChromaPrintScale
{
    float nozzle_mm     = 0.f;
    float line_width_mm = 0.f;
};

static ChromaPrintScale chroma_print_scale()
{
    ChromaPrintScale    out;
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;

    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    if (printer.has("nozzle_diameter"))
        out.nozzle_mm = float(printer.opt_float("nozzle_diameter", 0u));
    if (out.nozzle_mm <= 0.f)
        return out;

    // The outer wall's width, because the relief's steps are what the picture is made of and their
    // edges are outer walls; the general line width when that is on its default. Both are a width or a
    // percentage of the nozzle, hence get_abs_value().
    const DynamicPrintConfig &print = bundle->prints.get_edited_preset().config;
    for (const char *key : { "outer_wall_line_width", "line_width" }) {
        if (const auto *opt = print.option<ConfigOptionFloatOrPercent>(key); opt != nullptr) {
            const float w = float(opt->get_abs_value(double(out.nozzle_mm)));
            if (w > 0.f) {
                out.line_width_mm = w;
                break;
            }
        }
    }
    // A profile that says nothing useful: the nozzle is the honest lower bound on a printable feature.
    if (out.line_width_mm <= 0.f)
        out.line_width_mm = out.nozzle_mm;
    return out;
}

uint64_t GLGizmoTextureDisplacement::chroma_solve_key(const TextureDisplacementLayer &layer) const
{
    uint64_t   h   = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    // Rounded before hashing, so the last bits of a float that a slider re-derives identically do not
    // read as a change. A micrometre is finer than any of these quantities is meaningful to.
    const auto mix_f = [&mix](float v) { mix(uint64_t(int64_t(std::llround(double(v) * 1e6)))); };

    mix(reinterpret_cast<uintptr_t>(layer.chroma_source.get()));
    mix_f(layer.chroma.layer_height_mm);
    mix(layer.chroma.dither ? 1 : 0);
    // How close a match has to be, which decides which palette entry every pixel gets and so the whole
    // height map. Without it the settle below takes its "nothing the solver reads has changed" early
    // out and the slider moves nothing after the first solve.
    mix(uint64_t(uint32_t(layer.chroma.match_strictness)));
    // Despiking is a view-side toggle rather than part of the stack, so chroma_touch() cannot reach it
    // and the key has to read the member directly - but it goes into chroma_solve_input() and changes
    // the height map, so a key that leaves it out pins the solve to whatever it was first set to.
    mix(m_chroma_despike ? 1 : 0);

    // The lamp, which decides every colour in the palette when the light comes from behind.
    mix(uint64_t(int(layer.chroma.lighting)));
    mix_f(layer.chroma.led_color.r());
    mix_f(layer.chroma.led_color.g());
    mix_f(layer.chroma.led_color.b());
    mix_f(layer.chroma.led_intensity);
    mix_f(layer.chroma.lighting_balance);

    // The retouch edits, masks and all. A stroke changes the height map and nothing else about the
    // stack does, so without this an edit would be painted and never solved.
    const auto mix_mask = [&mix](const ChromaMask &mask) {
        mix(uint64_t(uint32_t(mask.width)));
        mix(uint64_t(uint32_t(mask.height)));
        for (const uint32_t run : mask.runs)
            mix(uint64_t(run));
    };
    for (const ChromaRetouchEdit &edit : layer.chroma.retouch.edits) {
        mix(uint64_t(int(edit.target)));
        mix(uint64_t(uint32_t(edit.value)));
        mix(edit.enabled ? 1 : 0);
        mix_mask(edit.region);
    }

    // The importance region changes which colour difference the solve reports, not which colour it
    // picks - but the report is what the card's frame and the error overlay are drawn from.
    mix_mask(layer.chroma.importance.region);
    mix_f(layer.chroma.importance.weight);

    // Face down the relief is complemented and the swaps come out in the other order, so the same
    // stack on the same picture is a different height map and a different set of ranges.
    mix(uint64_t(int(layer.chroma.plate.orientation)));

    for (const ChromaBand &band : layer.chroma.bands) {
        mix(uint64_t(band.layers));
        mix(uint64_t(band.filament.filament));
        mix_f(band.filament.td_mm);
        mix_f(band.filament.color.r());
        mix_f(band.filament.color.g());
        mix_f(band.filament.color.b());
    }

    // base_layers is deliberately absent front lit, and so is everything about the plate.
    // chroma_palette() makes every base layer the bottom band's own colour - acc starts there and each
    // base layer blends it with itself - so palette[base + i], which is the entire choice set the solver
    // picks from, comes out the same whatever the base is. The base moves where the swaps happen, not
    // what is solved.
    //
    // Back lit it is the opposite: the light comes through the whole column, the base included, so the
    // base is part of every colour in the palette and a layer of it added or removed changes all of
    // them. A hundred-layer base back lit is black, which is a real answer and has to be re-solved for.
    // Both lit for the same reason, in the share of the picture the lamp accounts for.
    if (layer.chroma.lighting != ChromaLighting::FrontLit)
        mix(uint64_t(uint32_t(layer.chroma.base_layers)));

    // The patch's geometry only enters through the tilt correction, which a level patch skips (see
    // chroma_run_solve()). So a flat face - which every generated plate has - contributes nothing here,
    // and resizing the plate or thickening its backing cannot invalidate a solve. A tilted one does
    // contribute, through both its angle and where the picture sits on it.
    if (const ChromaPatch patch = chroma_patch(layer); patch.valid && patch.tilt_deg > 0.5f) {
        mix_f(patch.tilt_deg);
        mix_f(layer.tiling_scale);
        mix_f(layer.rotation_deg);
        mix_f(layer.offset.x());
        mix_f(layer.offset.y());
    }
    return h == 0 ? 1 : h; // 0 means "nothing solved yet", so never hand it back as a real key
}

bool GLGizmoTextureDisplacement::chroma_solve_input(const TextureDisplacementLayer &layer, ChromaSolveInput &out,
                                                    int max_solve_px) const
{
    const ModelVolume *mv = texture_volume();
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mv == nullptr || mo == nullptr || mo->instances.empty() || layer.slot < 0)
        return false;

    const size_t inst = size_t(std::max(0, m_c->selection_info()->get_active_instance()));
    out.layer             = layer;
    out.base_mesh         = mv->mesh().its;
    out.facet_data        = mv->texture_displacement_facet(layer.slot).get_data();
    out.trafo             = mo->instances[inst]->get_transformation().get_matrix() * mv->get_matrix();
    out.bake              = texture_displacement_bake_frame(texture_displacement_volume_to_world(*mv));
    out.instance_bottom_z = mo->instance_bounding_box(inst, false).min.z();
    out.base_auto         = layer.chroma.base_auto;
    out.max_solve_px      = max_solve_px;

    // How big the picture is on the print, and how fine a line the printer draws. Together they are
    // what sizes a dither cell, and only this side knows either: see ChromaSolveInput::picture_mm.
    if (const ChromaPatch patch = chroma_patch(layer); patch.valid) {
        const Vec2f extent = patch.planar_max - patch.planar_min;
        out.picture_mm     = std::max(std::abs(extent.x()), std::abs(extent.y()));
    }
    out.line_width_mm = chroma_print_scale().line_width_mm;
    // Off unless the user asked for it, and at the value the solve recommends rather than a number of
    // this panel's own: how many layers make a spike is a question about the solve, not about the card.
    out.despike_layers = m_chroma_despike ? CHROMA_DESPIKE_LAYERS_RECOMMENDED : 0;
    return true;
}

void GLGizmoTextureDisplacement::chroma_apply_solve(int slot, ChromaSolveResult &&result)
{
    if (slot < 0 || size_t(slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    // Looked up fresh rather than held across the job: the layer can have been removed, the object
    // deselected or the gizmo reopened while this was computing.
    ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return;
    TextureDisplacementLayer *layer = nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == slot && l.chroma_enabled)
            layer = &l;
    if (layer == nullptr || !result.ok || !result.png)
        return;
    ChromaProfileSteps prof("apply_solve");

    if (result.base_layers > 0)
        layer->chroma.base_layers = result.base_layers;
    // A brand new allocation every time, never an edit in place: every texture cache downstream is
    // keyed on this pointer and validated through a weak_ptr. See decode_height_texture().
    layer->image_data = std::move(result.png);
    // The relief is exactly as tall as the layers above the base, and every step in the height map is
    // one of them - which is what makes the printed swaps line up with the heights. On a tilted patch
    // it is deeper than that, by as much as the steepest texel of it needed.
    layer->depth_mm   = result.depth_mm;
    layer->invert     = false;
    layer->midlevel   = 0.f;
    // Blurring a solved height map moves its steps off the layer boundaries they were solved for.
    layer->smoothing  = 0.f;

    m_chroma_preview[size_t(slot)] = upload_height_thumbnail(result.preview);
    prof.step("upload_card_thumb", std::to_string(result.preview.width) + "x" +
                                       std::to_string(result.preview.height));
    m_chroma_report[size_t(slot)]  = ChromaReport{ true, result.mean_delta_e, result.max_delta_e };
    m_chroma_palette_report[size_t(slot)] = result.palette;
    m_chroma_diag[size_t(slot)]           = std::move(result.diagnostics);

    // How far the stack landed from the picture, per pixel, for the overlay and the card. Uploaded as a
    // grey image, so the thumbnail path's luminance handling carries it and the shader reads it from the
    // red channel. Finer than a thumbnail on purpose: this is drawn over the model, where the question
    // it answers is which part of the picture is wrong.
    if (!result.delta.empty() && result.preview.width > 0 && result.preview.height > 0) {
        DecodedHeightTexture error;
        error.width                      = result.preview.width;
        error.height                     = result.preview.height;
        error.pixels                     = std::move(result.delta);
        m_chroma_error_tex[size_t(slot)] = upload_height_thumbnail(error, CHROMA_SOLVE_PREVIEW_PX);
    } else
        m_chroma_error_tex[size_t(slot)].reset();
    prof.step("upload_error_tex");

    // The same prediction again, at the solve's resolution instead of the card's, for the shader to
    // diffuse. Uploading it twice costs one more texture per layer and saves the alternative, which is
    // re-walking the stack at a dozen neighbouring points per fragment.
    m_chroma_scatter_tex[size_t(slot)] = upload_height_thumbnail(result.preview, CHROMA_SOLVE_PREVIEW_PX);
    prof.step("upload_scatter_tex");

    // The same solve's height map, as grey, for the layered view to cut its sheets with. The prediction
    // above carries both channels in one DecodedHeightTexture and the upload shows the colour one when
    // there is one, so the height has to be handed over as a texture of its own with no colour in it.
    // Copied rather than moved: the prediction is still wanted whole, by the card and by the scattering.
    if (result.preview.width > 0 && result.preview.height > 0 && !result.preview.pixels.empty()) {
        DecodedHeightTexture levels;
        levels.width                     = result.preview.width;
        levels.height                    = result.preview.height;
        levels.pixels                    = result.preview.pixels;
        m_chroma_level_tex[size_t(slot)] = upload_height_thumbnail(levels, CHROMA_SOLVE_PREVIEW_PX);
    } else
        m_chroma_level_tex[size_t(slot)].reset();
    prof.step("upload_level_tex");

    m_chroma_solve_key[size_t(slot)] = chroma_solve_key(*layer);
    prof.step("solve_key");

    if (m_chroma_apply_after_solve[size_t(slot)]) {
        m_chroma_apply_after_solve[size_t(slot)] = false;
        chroma_apply_swaps(*layer, true);
    }
    prof.step("apply_swaps");

    rebuild_preview();
    prof.step("rebuild_preview");
    m_parent.set_as_dirty();
    prof.total();
}

bool GLGizmoTextureDisplacement::chroma_solve_layer(TextureDisplacementLayer &layer, bool silent)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return false;
    const size_t slot = size_t(layer.slot);

    if (!layer.chroma_source || layer.chroma.bands.empty() || layer.chroma.relief_layers() <= 0) {
        m_chroma_report[slot]         = ChromaReport{};
        m_chroma_palette_report[slot] = ChromaPaletteReport{};
        m_chroma_diag[slot]           = ChromaDiagnostics{};
        m_chroma_preview[slot].reset();
        m_chroma_scatter_tex[slot].reset();
        m_chroma_level_tex[slot].reset();
        if (!silent)
            show_error(nullptr, _u8L("Pick a picture and at least two layers of filament before solving."));
        return false;
    }

    // One in flight per slot; anything asked for meanwhile collapses into a single follow-up issued
    // from the completion handler, exactly as queue_preview_job() does. The UI worker is one FIFO
    // thread, so without this a run of keystrokes would queue a solve each and run every one of them.
    if (m_chroma_solve_running[slot]) {
        m_chroma_solve_pending[slot] = true;
        return true;
    }

    ChromaSolveInput input;
    if (!chroma_solve_input(layer, input, CHROMA_SOLVE_PREVIEW_PX))
        return false;

    const uint64_t generation = m_chroma_generation->fetch_add(1) + 1;
    chroma_profile_mark("solve/queued", "gen " + std::to_string(generation));
    m_chroma_solve_running[slot] = true;
    // Nothing is cleared here: the card goes on showing the last prediction and the model the last
    // relief, which is what makes an edit feel immediate instead of blanking and coming back.
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<ChromaSolveJob>(
                          std::move(input), generation, m_chroma_generation,
                          [this, slot](ChromaSolveResult result, uint64_t result_generation) {
                              m_chroma_solve_running[slot] = false;
                              chroma_profile_mark("solve/landed",
                                                  std::string(result.ok ? "ok" : "empty") + " gen " +
                                                      std::to_string(result_generation));
                              if (result_generation != m_chroma_generation->load()) {
                                  // Superseded while computing - it aborted early and came back empty.
                                  // Whatever the newest state is, it still needs a run.
                                  m_chroma_solve_pending[slot] = true;
                              } else if (result.ok) {
                                  m_chroma_solve_coarse[slot] = true;
                                  chroma_apply_solve(int(slot), std::move(result));
                              }
                              if (m_chroma_solve_pending[slot]) {
                                  m_chroma_solve_pending[slot] = false;
                                  if (TextureDisplacementLayer *again = chroma_layer_in_slot(int(slot)))
                                      chroma_solve_layer(*again, true);
                              }
                              m_parent.set_as_dirty();
                          }));
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoTextureDisplacement::chroma_solve_layer_blocking(TextureDisplacementLayer &layer, ChromaSolve *solve_out)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return false;
    const size_t slot = size_t(layer.slot);
    if (!layer.chroma_source || layer.chroma.bands.empty() || layer.chroma.relief_layers() <= 0)
        return false;

    // Fine enough for what the nozzle can lay down, rather than however many megapixels the picture
    // happens to carry: see chroma_printable_solve_px(). The patch is what gives the picture a size in
    // millimetres - for a generated plate it is that plate's own picture face.
    float picture_mm = 0.f;
    if (const ChromaPatch patch = chroma_patch(layer); patch.valid) {
        const Vec2f extent = patch.planar_max - patch.planar_min;
        picture_mm         = std::max(std::abs(extent.x()), std::abs(extent.y()));
    }

    ChromaSolveInput input;
    if (!chroma_solve_input(layer, input, chroma_printable_solve_px(picture_mm, chroma_print_scale().nozzle_mm)))
        return false;
    // This is the pass whose height map is printed, so it is the one worth the better colour metric.
    input.high_quality = true;

    // The job's own body rather than a second copy of it: one description of what a solve is, whether
    // it runs on a worker or here. Nothing in it touches the Model - that is what made it a job in the
    // first place - so calling it from this thread is safe.
    ChromaSolveResult result = chroma_run_solve(input, {});
    if (!result.ok)
        return false;

    if (solve_out != nullptr) {
        solve_out->width  = result.preview.width;
        solve_out->height = result.preview.height;
        solve_out->levels = result.levels;
    }
    chroma_apply_solve(layer.slot, std::move(result));
    m_chroma_solve_coarse[slot] = false;
    return true;
}

// The plate turned into the parts a flat tile is printed from.
//
// A stack carries its picture in the height of its columns. That is why its plate tapers, and it is
// the right object for something looked at from one side: the terraces *are* the picture. It is the
// wrong object for a tile - a coaster, a bookmark, a keycap - which has to be there at every layer.
// Printed as it stands, every column short of the top is an overhang over nothing.
//
// A flat tile is the same picture made the other way round. The terraces stay exactly where they are,
// the space they leave empty is filled in (ChromaSplitPieces::fill), and the colour stops coming from
// the height of a column and starts coming from which part each layer is printed in: every layer is
// now the full footprint with several filament changes inside it. What that costs is swaps - dozens
// of them, where the plate needed one per band - which is why it is a separate thing to ask for and
// not what the plate does by default.
//
// The plate itself does not survive it, and that is the right trade rather than a loss: the pieces
// are the same geometry seen another way, and the snapshot this takes puts the plate back whole. An
// object full of parts cannot also be a plate with a picture painted on it; trying to be both is how
// a bake would have found a height map describing a mesh that no longer existed.
//
// Deferred out of the panel by CallAfter rather than done where the button is: this replaces the
// volume the gizmo is attached to and adds more beside it, and the panel is drawn from that volume
// while the click is being handled. By the time the event loop comes back round, nothing is reading
// it, which is why the slot is carried rather than the layer.
void GLGizmoTextureDisplacement::chroma_make_flat_tile(int slot)
{
    Plater      *plater = wxGetApp().plater();
    ModelObject *mo     = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    ModelVolume *mv     = texture_volume();
    if (plater == nullptr || mo == nullptr || mv == nullptr || slot < 0 ||
        size_t(slot) >= mv->texture_displacement_layers.size())
        return;

    TextureDisplacementLayer &layer = mv->texture_displacement_layers[size_t(slot)];
    if (!layer.chroma.plate.generated || !layer.chroma_source || layer.chroma.bands.empty() ||
        layer.chroma.relief_layers() <= 0) {
        show_error(nullptr, _u8L("A flat tile is made from a picture that brought its own plate and a stack "
                                 "with layers in it."));
        return;
    }

    // Solved here, at the resolution that gets printed, rather than read back from the preview: the
    // preview is deliberately solved coarse to keep the panel moving under a slider, and these are
    // the terraces the printer will lay down.
    wxBusyCursor wait;
    ChromaSolve  solve;
    if (!chroma_solve_layer_blocking(layer, &solve) || solve.empty()) {
        show_error(nullptr, _u8L("The picture could not be solved, so there is nothing to split."));
        return;
    }

    // Which side of the picture the fill goes on, which decides both the geometry and what the fill
    // has to be printed in. Behind it, the fill is the backing come forward to meet the picture where
    // the picture is thin: it prints in the filament the backing already prints in, and nothing stands
    // between the eye and the colours. In front of it, the fill is in the light's way before the light
    // reaches any colour, and then it has to be a filament the light gets through.
    const ChromaFlatMode mode = m_chroma_flat_mode == 0 ? ChromaFlatMode::ClearInFront :
                                m_chroma_flat_mode == 1 ? ChromaFlatMode::BackingBehind :
                                                          ChromaFlatMode::FlatStack;
    const bool         fill_clear = mode == ChromaFlatMode::ClearInFront;
    ChromaSplitOptions split_options;
    split_options.flat_mode = mode;

    ChromaSplitPieces pieces = chroma_split_bands(layer.chroma, solve, split_options);
    if (pieces.empty()) {
        show_error(nullptr, _u8L("The stack could not be split into parts."));
        return;
    }

    // Behind the picture there is nothing to choose: the fill is the backing, and the backing prints
    // in the bottom band's filament (ChromaStack::base_layers).
    //
    // In front of it, it has to be a filament the stack is not already using, since every one of those
    // was chosen for the colour it shows rather than the light it passes. The first unused slot is the
    // guess; with none free it falls back to the last one and the message below says to change it.
    // Guessing is right here and picking at random is not: a part handed filament 1 would come out in
    // whatever the base prints in, which is opaque, and the tile would print with its picture buried
    // under a solid face.
    int fill_filament = layer.chroma.bands.front().filament.filament;
    if (fill_clear) {
        fill_filament = -1;
        if (const PresetBundle *bundle = wxGetApp().preset_bundle; bundle != nullptr) {
            const int count = int(bundle->filament_presets.size());
            for (int f = 0; f < count && fill_filament < 0; ++f) {
                bool used = false;
                for (const ChromaBand &band : layer.chroma.bands)
                    used = used || band.filament.filament == f;
                if (!used)
                    fill_filament = f;
            }
            if (fill_filament < 0)
                fill_filament = std::max(0, count - 1);
        }
    }
    const std::string fill_name = fill_clear ? _u8L("Clear fill") : _u8L("Backing fill");

    struct Part
    {
        indexed_triangle_set its;
        std::string          name;
        int                  filament = 0;
    };
    std::vector<Part> parts;
    for (size_t i = 0; i < pieces.bands.size(); ++i)
        if (!pieces.bands[i].indices.empty())
            parts.push_back(Part{ std::move(pieces.bands[i]),
                                  Slic3r::format(_u8L("Color %1%"), int(i) + 1),
                                  layer.chroma.bands[i].filament.filament });
    if (!pieces.fill.indices.empty() && fill_filament >= 0)
        parts.push_back(Part{ std::move(pieces.fill), fill_name, fill_filament });
    if (parts.empty()) {
        show_error(nullptr, _u8L("The split produced no parts."));
        return;
    }

    // The pieces are built in the plate's own frame - centred on the origin, sitting on z = 0 - and
    // the volume's mesh need not still be in it: loading a mesh as an object centres it and
    // compensates with the volume's offset, and a bake replaces the mesh outright. Lining the two up
    // by the two things that cannot have changed - where the footprint is centred and where the
    // underside is - costs nothing when nothing moved, since the shift is then zero.
    BoundingBoxf3 part_box;
    for (const Part &part : parts)
        for (const Vec3f &v : part.its.vertices)
            part_box.merge(v.cast<double>());
    const BoundingBoxf3 mesh_box = mv->mesh().bounding_box();
    const Vec3f         shift(float(mesh_box.center().x() - part_box.center().x()),
                              float(mesh_box.center().y() - part_box.center().y()),
                              float(mesh_box.min.z() - part_box.min.z()));
    if (shift.squaredNorm() > 0.f)
        for (Part &part : parts)
            for (Vec3f &v : part.its.vertices)
                v += shift;

    Plater::TakeSnapshot snapshot(plater, _u8L("Flat ChromaStack tile"), UndoRedo::SnapshotType::GizmoAction);

    const Geometry::Transformation trafo = mv->get_transformation();
    const std::string              stem  = mv->name;
    const auto                     part_name = [&stem](const std::string &what) {
        return stem.empty() ? what : stem + " - " + what;
    };

    // The first part takes the textured volume's place rather than standing beside it. Deleting the
    // volume the gizmo is attached to would leave the panel, the selectors and the preview all
    // holding a volume that is gone; replacing its mesh is the same operation a bake already does to
    // it, and the paint and the layers go with the picture they described.
    for (int i = 0; i < TEXTURE_DISPLACEMENT_MAX_LAYERS; ++i)
        mv->texture_displacement_facet(i).reset();
    mv->texture_displacement_layers.clear();
    mv->set_mesh(TriangleMesh(std::move(parts[0].its)));
    mv->set_new_unique_id();
    mv->calculate_convex_hull();
    mv->name = part_name(parts[0].name);
    mv->config.set_key_value("extruder", new ConfigOptionInt(parts[0].filament + 1));

    for (size_t i = 1; i < parts.size(); ++i) {
        ModelVolume *part = mo->add_volume(TriangleMesh(std::move(parts[i].its)), ModelVolumeType::MODEL_PART,
                                           /* modify_to_center_geometry */ false);
        if (part == nullptr)
            continue;
        part->set_transformation(trafo);
        part->name = part_name(parts[i].name);
        part->config.set_key_value("extruder", new ConfigOptionInt(parts[i].filament + 1));
    }

    mo->invalidate_bounding_box();
    mo->ensure_on_bed();

    // The gizmo's own state is built from a volume that has just become a different mesh with no
    // texture layers on it, so it is rebuilt before anything draws from it again.
    update_from_model_object(false);
    plater->update();
    m_parent.reload_scene(true);
    m_parent.set_as_dirty();

    std::string note = Slic3r::format(_u8L("The picture is now %1% parts, one per filament, plus the fill that "
                                           "takes up the space it leaves empty."),
                                      parts.size());
    if (!pieces.fill.indices.empty())
        note += "\n\n" + (fill_clear ?
            Slic3r::format(_u8L("The part named \"%1%\" is that fill. It sits in front of the picture, so give it "
                                "a filament that passes light - it has been put on filament %2% only because the "
                                "stack was not using it."),
                           fill_name, fill_filament + 1) :
            Slic3r::format(_u8L("The part named \"%1%\" is that fill. It sits behind the picture, where the backing "
                                "is, and prints in the backing's own filament %2% - so the tile needs no filament "
                                "the stack was not already using."),
                           fill_name, fill_filament + 1));
    if (pieces.frame_omitted)
        note += "\n\n" + _u8L("The plate's frame was left out: a swept moulding is not a height map and cannot be "
                              "split this way. Set the frame's width to zero to print the tile without one.");
    MessageDialog(plater, from_u8(note), _L("ChromaStack"), wxICON_INFORMATION | wxOK).ShowModal();
}

// A fingerprint of an object's height ranges: the bands and the filament each prints in. What it is
// for is telling "the ranges this stack wrote" from "ranges somebody made", which is the whole of the
// automatic path's licence to overwrite them.
static uint64_t chroma_ranges_signature(const t_layer_config_ranges &ranges)
{
    uint64_t   h   = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const auto &[range, config] : ranges) {
        mix(uint64_t(std::llround(range.first * 1e6)));
        mix(uint64_t(std::llround(range.second * 1e6)));
        mix(uint64_t(config.has("extruder") ? config.opt_int("extruder") : 0));
    }
    return h == 0 ? 1 : h; // 0 means "nothing written", so never hand it back as a real signature
}

bool GLGizmoTextureDisplacement::chroma_swaps_applied(const TextureDisplacementLayer &layer) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return false;
    const std::vector<ChromaRange> ranges = chroma_ranges(layer.chroma, 0.);
    if (ranges.size() != mo->layer_config_ranges.size())
        return false;

    auto it = mo->layer_config_ranges.begin();
    for (const ChromaRange &r : ranges) {
        if (!is_approx(it->first.first, r.z_min) || !is_approx(it->first.second, r.z_max))
            return false;
        if (!it->second.has("extruder") || it->second.opt_int("extruder") != r.filament + 1)
            return false;
        ++it;
    }
    return true;
}

bool GLGizmoTextureDisplacement::chroma_swaps_blocked(const TextureDisplacementLayer &layer) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || mo->layer_config_ranges.empty())
        return false; // nothing there to take over
    if (chroma_swaps_applied(layer))
        return false; // already ours, and already right
    return chroma_ranges_signature(mo->layer_config_ranges) != layer.chroma.applied_ranges;
}

void GLGizmoTextureDisplacement::chroma_apply_swaps(TextureDisplacementLayer &layer, bool silent)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    const std::vector<ChromaRange> ranges = chroma_ranges(layer.chroma, 0.);
    if (ranges.empty() || chroma_swaps_applied(layer))
        return;

    // The ranges are the object's, not the layer's. Taking over ones this stack did not write is the
    // user's call, and the automatic path never asks: it leaves them and lets the card say so.
    if (chroma_swaps_blocked(layer)) {
        if (silent)
            return;
        MessageDialog dlg(wxGetApp().plater(),
                          _L("This object has height ranges that did not come from this picture. Replace them with "
                             "its filament swaps?"),
                          _L("ChromaStack"), wxICON_QUESTION | wxYES_NO);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }

    mo->layer_config_ranges.clear();
    for (const ChromaRange &r : ranges) {
        ModelConfig &config = mo->layer_config_ranges[{ r.z_min, r.z_max }];
        config.set_key_value("extruder", new ConfigOptionInt(r.filament + 1));
        // Every range carries its layer height: the solve is only valid at the height it was run at,
        // and layer_height_profile_from_ranges() reads the option without checking it is there.
        config.set_key_value("layer_height", new ConfigOptionFloat(double(layer.chroma.layer_height_mm)));
    }
    layer.chroma.applied_ranges = chroma_ranges_signature(mo->layer_config_ranges);

    if (ObjectList *list = wxGetApp().obj_list(); list != nullptr) {
        const int obj_idx = m_parent.get_selection().get_object_idx();
        if (obj_idx >= 0)
            list->rebuild_layer_ranges(obj_idx);
    }
}

void GLGizmoTextureDisplacement::chroma_touch(int slot)
{
    if (slot < 0 || size_t(slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    chroma_profile_mark("touch", "slot " + std::to_string(slot));
    // Measured from the last change, so a drag or a run of keystrokes settles once, at the end.
    m_chroma_settle_at[size_t(slot)] =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count() +
        CHROMA_SETTLE_MS;
}

void GLGizmoTextureDisplacement::chroma_settle(TextureDisplacementLayer &layer)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    const size_t slot = size_t(layer.slot);
    if (m_chroma_settle_at[slot] == 0)
        return;
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (now < m_chroma_settle_at[slot])
        return;
    // Not while a bake or a mesh preparation owns the volume: both replace the mesh under us.
    if (m_bake_in_progress || m_prepare_in_progress || ImGui::IsAnyItemActive())
        return;

    // Not while this slot's own work is still in flight either: the plate job is about to replace the
    // mesh a solve would measure against, and a second solve would only be thrown away.
    if (m_chroma_plate_running[slot] || m_chroma_solve_running[slot])
        return;

    m_chroma_settle_at[slot] = 0;
    chroma_profile_mark("settle", "slot " + std::to_string(slot));
    // The plate first: the solve reads the painted patch, and a rebuilt plate is a different patch.
    // The rest follows from its completion handler, which knows what the new mesh turned out to be.
    if (m_chroma_plate_dirty[slot]) {
        m_chroma_plate_dirty[slot] = false;
        if (chroma_queue_plate(layer))
            return;
    }

    // Nothing the solver reads has changed, so there is nothing to solve. This is what makes the plate
    // controls cheap: dragging Size or Backing re-fits the picture and rewrites the swap heights, both
    // of which are arithmetic, and leaves the height map exactly as it was. See chroma_solve_key().
    //
    // A layer with no height map yet is never skipped, however well its key matches - a key is only
    // evidence about a map that exists, and a slot can be reused by a layer that never solved.
    if (layer.image_data && m_chroma_solve_key[slot] != 0 && chroma_solve_key(layer) == m_chroma_solve_key[slot]) {
        chroma_profile_mark("settle/key-unchanged", "no solve needed");
        chroma_apply_swaps(layer, true);
        return;
    }

    // The swaps are written once the height map they describe actually exists, not before.
    m_chroma_apply_after_solve[slot] = true;
    if (!chroma_solve_layer(layer, true))
        m_chroma_apply_after_solve[slot] = false;
}

void GLGizmoTextureDisplacement::chroma_suggest(TextureDisplacementLayer &layer, int count)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    m_chroma_suggestion[size_t(layer.slot)].clear();
    if (!layer.chroma_source)
        return;

    TextureDisplacementLayer probe;
    probe.image_data                  = layer.chroma_source;
    const DecodedHeightTexture source = decode_height_texture(probe);
    if (source.empty())
        return;

    std::vector<uint8_t> grey_rgb;
    const uint8_t       *rgb = source.rgb.data();
    if (!source.has_color()) {
        grey_rgb.resize(source.pixels.size() * 3);
        for (size_t i = 0; i < source.pixels.size(); ++i)
            grey_rgb[i * 3] = grey_rgb[i * 3 + 1] = grey_rgb[i * 3 + 2] = source.pixels[i];
        rgb = grey_rgb.data();
    }

    const int relief = std::max(CHROMA_DEFAULT_RELIEF_LAYERS, layer.chroma.relief_layers());
    // Weighted for vividness, and by whatever the user has said matters: picking colours is exactly
    // where coverage alone goes wrong, because the background of a picture is most of its area and the
    // one saturated thing in it is usually what the picture is of.
    m_chroma_suggestion[size_t(layer.slot)] =
        chroma_suggest_colors(chroma_histogram(rgb, source.width, source.height, 192,
                                               CHROMA_VIVIDNESS_RECOMMENDED, layer.chroma.importance),
                              count, chroma_filaments(), layer.chroma.layer_height_mm, relief);
}

void GLGizmoTextureDisplacement::chroma_apply_suggestions(TextureDisplacementLayer &layer)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;

    const std::vector<ChromaSuggestion> &found = m_chroma_suggestion[size_t(layer.slot)];
    const std::vector<ChromaFilament>    owned = chroma_filaments();

    std::vector<ChromaBand> bands;
    for (const ChromaSuggestion &one : found) {
        // Nothing of the user's is anywhere near this colour, so there is nothing to print that band in.
        // Skipped rather than guessed at: a band filled with the least-bad filament in the drawer would
        // make the stack worse than the one with that colour simply left out.
        if (one.nearest < 0 || size_t(one.nearest) >= owned.size())
            continue;
        if (bands.size() >= CHROMA_MAX_BANDS)
            break;

        ChromaBand band;
        // The filament's own colour and transmission distance, not the suggested ones: the suggestion
        // says what to go looking for, and a prediction computed from a colour nobody owns would be a
        // prediction of a print that cannot happen.
        band.filament = owned[size_t(one.nearest)];
        band.layers   = std::clamp(one.layers, 1, CHROMA_MAX_RELIEF_LAYERS);

        // Two suggested colours can have the same filament of yours as their nearest, and a band printed
        // in the filament below it is not a swap at all - it is more of the band below. Merging them here
        // keeps the stack honest about how many filament changes it costs.
        if (!bands.empty() && bands.back().filament.filament == band.filament.filament)
            bands.back().layers = std::min(CHROMA_MAX_RELIEF_LAYERS, bands.back().layers + band.layers);
        else
            bands.push_back(band);
    }
    if (bands.empty())
        return;

    // Only the bands. Everything else about the stack belongs to the user and has nothing to do with
    // which filaments it prints in: the base and whether it follows the face, the dither, the plate and
    // its frame, and the fingerprint of the swaps already written all stay as they are.
    layer.chroma.bands = std::move(bands);
    chroma_touch(layer.slot);
}

void GLGizmoTextureDisplacement::chroma_mask_grid(const TextureDisplacementLayer &layer, int &width, int &height) const
{
    width = height = 0;
    if (!layer.chroma_source)
        return;
    TextureDisplacementLayer   probe;
    probe.image_data                  = layer.chroma_source;
    const DecodedHeightTexture source = decode_height_texture(probe);
    if (source.empty())
        return;

    // The picture's own grid, capped the way the live solve is. A mask is sampled nearest-neighbour, so
    // one painted here holds for the coarse solve and for the full resolution bake alike; what the cap
    // buys is that a stroke on a 12 MP photograph is a megabyte of runs rather than forty.
    const int scale = std::max(1, (std::max(source.width, source.height) + CHROMA_SOLVE_PREVIEW_PX - 1) /
                                      CHROMA_SOLVE_PREVIEW_PX);
    width  = std::max(1, source.width / scale);
    height = std::max(1, source.height / scale);
}

bool GLGizmoTextureDisplacement::chroma_stamp_at(const Vec3f &hit, const TextureDisplacementLayer &layer)
{
    if (m_chroma_mask.empty() || m_chroma_mask_w <= 0 || m_chroma_mask_h <= 0)
        return false;

    const ModelVolume *mv = texture_volume();
    if (mv == nullptr)
        return false;

    // Where that point lands in the picture, through the mapping chroma_build_surface() and the bake
    // both use: the picture is pinned to the XY plane, so its place on the plate does not move as the
    // surface under it tilts.
    const float aspect = layer_texture_aspect(layer);
    const Vec3d baked  = texture_displacement_bake_frame(texture_displacement_volume_to_world(*mv)) * hit.cast<double>();
    const Vec2f uv     = apply_uv_transform(Vec2f(float(baked.x()), float(-baked.y())), layer, aspect);
    const float cx     = uv.x() * float(m_chroma_mask_w);
    const float cy     = uv.y() * float(m_chroma_mask_h);

    // The brush is set in millimetres on the print, because that is what the user is looking at; a
    // texel is whatever fraction of the picture's width that is.
    float picture_mm = 0.f;
    if (const ChromaPatch patch = chroma_patch(layer); patch.valid) {
        const Vec2f extent = patch.planar_max - patch.planar_min;
        picture_mm         = std::max(std::abs(extent.x()), std::abs(extent.y()));
    }
    const float mm_per_texel = picture_mm > 0.f ? picture_mm / float(std::max(m_chroma_mask_w, m_chroma_mask_h)) : 0.f;
    const float radius_px    = mm_per_texel > 0.f ? std::max(1.f, m_chroma_retouch_radius_mm / mm_per_texel) : 8.f;

    const int x0 = std::max(0, int(std::floor(cx - radius_px)));
    const int x1 = std::min(m_chroma_mask_w - 1, int(std::ceil(cx + radius_px)));
    const int y0 = std::max(0, int(std::floor(cy - radius_px)));
    const int y1 = std::min(m_chroma_mask_h - 1, int(std::ceil(cy + radius_px)));
    if (x1 < x0 || y1 < y0)
        return false; // the ray hit the model somewhere the picture does not reach

    const float r2 = radius_px * radius_px;
    bool        any = false;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x) + 0.5f - cx;
            const float dy = float(y) + 0.5f - cy;
            if (dx * dx + dy * dy > r2)
                continue;
            m_chroma_mask[size_t(y) * size_t(m_chroma_mask_w) + size_t(x)] = 1;
            any                                                            = true;
        }
    return any;
}

void GLGizmoTextureDisplacement::chroma_commit_stroke(TextureDisplacementLayer &layer)
{
    m_chroma_retouch_painting = false;
    if (m_chroma_mask.empty())
        return;

    ChromaMask region = chroma_mask_encode(m_chroma_mask.data(), m_chroma_mask_w, m_chroma_mask_h);
    m_chroma_mask.clear();
    m_chroma_mask_w = m_chroma_mask_h = 0;
    // A stroke that covered nothing - a drag that never touched the picture - is not an edit. Storing
    // it would leave a row in the list that does nothing and cannot be told from one that does.
    if (region.empty())
        return;

    // A discrete action by the user, unlike the automatic re-solve, so it is worth an undo step of its
    // own: a stroke is the unit a user would expect Ctrl+Z to take back.
    Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                  m_chroma_retouch_kind == 2 ? _u8L("Mark what the ChromaStack picture is about") :
                                                               _u8L("Fix ChromaStack colors"),
                                  UndoRedo::SnapshotType::GizmoAction);

    // Marking what matters is not an edit to the picture: it says which of the picture's colours the
    // filament search and the colour suggestions should serve. One region per stack, so a new stroke
    // replaces the last rather than stacking up.
    if (m_chroma_retouch_kind == 2) {
        layer.chroma.importance.region = std::move(region);
        layer.chroma.importance.weight = std::max(1.f, m_chroma_importance_weight);
        chroma_touch(layer.slot);
        return;
    }

    ChromaRetouchEdit edit;
    edit.region = std::move(region);
    if (m_chroma_retouch_kind == 0) {
        edit.target = ChromaRetouchEdit::Target::Delta;
        edit.value  = m_chroma_retouch_delta;
    } else {
        edit.target = ChromaRetouchEdit::Target::Band;
        edit.value  = m_chroma_retouch_band;
    }
    layer.chroma.retouch.edits.push_back(std::move(edit));
    chroma_touch(layer.slot);
}

void GLGizmoTextureDisplacement::render_chroma_brush()
{
    if (!m_chroma_brush_valid)
        return;
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelVolume *mv = texture_volume();
    GLShaderProgram   *shader = wxGetApp().get_shader("flat");
    if (mo == nullptr || mv == nullptr || shader == nullptr || mo->instances.empty())
        return;

    // A unit ring in the XY plane, as line segments: a line loop is not among the primitive types this
    // renderer offers, and 48 segments is past the point where a circle this size looks like a polygon.
    if (!m_chroma_brush_glmodel.is_initialized()) {
        constexpr int     segments = 48;
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(segments);
        init_data.reserve_indices(segments * 2);
        for (int i = 0; i < segments; ++i) {
            const float a = 2.f * float(M_PI) * float(i) / float(segments);
            init_data.add_vertex(Vec3f(std::cos(a), std::sin(a), 0.f));
        }
        for (int i = 0; i < segments; ++i)
            init_data.add_line(unsigned(i), unsigned((i + 1) % segments));
        m_chroma_brush_glmodel.init_from(std::move(init_data));
    }

    const Selection  &selection = m_parent.get_selection();
    const Transform3d trafo = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() *
                              mv->get_matrix();

    // The ring lies in the surface's own plane, so it reads as drawn on the print rather than hovering
    // over it. Any two directions perpendicular to the normal will do; the one chosen avoids being
    // parallel to it.
    const Vec3f n    = m_chroma_brush_normal.normalized();
    const Vec3f seed = std::abs(n.z()) < 0.9f ? Vec3f::UnitZ() : Vec3f::UnitX();
    const Vec3f t    = seed.cross(n).normalized();
    const Vec3f b    = n.cross(t);

    // The brush is set in printed millimetres, and this transform ends in the instance's own frame, so
    // the radius is divided by the scaling that frame applies. Approximate where the volume carries a
    // non-uniform scale of its own, which for a plate or a plaque it does not.
    const double scale  = std::max(1e-6, mo->instances[selection.get_instance_idx()]
                                            ->get_scaling_factor()
                                            .cwiseAbs()
                                            .maxCoeff());
    const double radius = double(m_chroma_retouch_radius_mm) / scale;

    Transform3d ring     = Transform3d::Identity();
    ring.linear().col(0) = t.cast<double>();
    ring.linear().col(1) = b.cast<double>();
    ring.linear().col(2) = n.cast<double>();
    ring.translation()   = m_chroma_brush_point.cast<double>();

    const Camera &camera = wxGetApp().plater()->get_camera();
    glsafe(::glDisable(GL_DEPTH_TEST)); // the ring is on the surface, so it must not fight it for depth
    glsafe(::glEnable(GL_BLEND));
    shader->start_using();
    shader->set_uniform("view_model_matrix",
                        camera.get_view_matrix() * trafo * ring *
                            Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d(radius, radius, radius)));
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Filled while a stroke is being laid down, hollow while only hovering: the difference between the
    // brush being armed and the brush being used is worth seeing without looking at the mouse.
    ColorRGBA color = m_chroma_retouch_painting ? ColorRGBA::ORANGE() : ColorRGBA::WHITE();
    color.a(m_chroma_retouch_painting ? 0.95f : 0.65f);
    m_chroma_brush_glmodel.set_color(color);
    m_chroma_brush_glmodel.render();
    shader->stop_using();
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

bool GLGizmoTextureDisplacement::on_mouse_chroma_retouch(const wxMouseEvent &mouse_event)
{
    // Ctrl orbits the camera while the brush is active, exactly as it does while painting: without it
    // the mode would swallow every drag and the model could not be turned to see what is being fixed.
    if (mouse_event.CmdDown())
        return false;

    TextureDisplacementLayer *layer = active_layer();
    if (layer == nullptr || !layer->chroma_enabled || !layer->chroma_source)
        return false;

    // Where the pointer is on the model, for the ring and for the stroke alike: one raycast per event,
    // on hover as well as while painting, which is what the painter base class does for its own brush.
    const ModelObject *mo  = m_c->selection_info()->model_object();
    const ModelVolume *mv  = texture_volume();
    const int          idx = texture_volume_raycaster_index();
    const bool         was = m_chroma_brush_valid;
    m_chroma_brush_valid   = false;
    if (mo != nullptr && mv != nullptr && idx >= 0 && !mo->instances.empty()) {
        const Selection  &selection = m_parent.get_selection();
        const Transform3d trafo = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() *
                                  mv->get_matrix();
        Vec3f hit = Vec3f::Zero(), normal = Vec3f::Zero();
        if (m_c->raycaster()->raycasters()[size_t(idx)]->unproject_on_mesh(
                Vec2d(mouse_event.GetX(), mouse_event.GetY()), trafo, wxGetApp().plater()->get_camera(), hit, normal,
                m_c->object_clipper()->get_clipping_plane())) {
            m_chroma_brush_point  = hit;
            m_chroma_brush_normal = normal;
            m_chroma_brush_valid  = true;
        }
    }
    // The ring appearing or disappearing is a change on screen like any other.
    if (was != m_chroma_brush_valid || m_chroma_brush_valid)
        m_parent.set_as_dirty();

    if (mouse_event.LeftDown()) {
        if (!m_chroma_brush_valid)
            return false; // a click off the model is the canvas's, not a stroke of nothing
        chroma_mask_grid(*layer, m_chroma_mask_w, m_chroma_mask_h);
        if (m_chroma_mask_w <= 0 || m_chroma_mask_h <= 0)
            return false;
        m_chroma_mask.assign(size_t(m_chroma_mask_w) * size_t(m_chroma_mask_h), 0);
        m_chroma_retouch_painting = true;
        chroma_stamp_at(m_chroma_brush_point, *layer);
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.Dragging() && m_chroma_retouch_painting) {
        // Stamped per event rather than interpolated between them: at the rate wx delivers motion a
        // disc per event already overlaps, and a dropped event leaves a gap the user simply paints over.
        // A drag that leaves the model keeps the stroke open, so coming back continues the same edit.
        if (m_chroma_brush_valid)
            chroma_stamp_at(m_chroma_brush_point, *layer);
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.LeftUp() && m_chroma_retouch_painting) {
        chroma_commit_stroke(*layer);
        m_parent.set_as_dirty();
        return true;
    }
    // Right button, wheel and everything else belongs to the canvas: zoom and pan keep working.
    return false;
}

void GLGizmoTextureDisplacement::chroma_pick_colors(TextureDisplacementLayer &layer, int count)
{
    if (!layer.chroma_source) {
        chroma_pick_picture(layer);
        if (!layer.chroma_source)
            return;
    }
    TextureDisplacementLayer probe;
    probe.image_data                  = layer.chroma_source;
    const DecodedHeightTexture source = decode_height_texture(probe);
    if (source.empty())
        return;

    // The same grey-is-a-colour expansion the solve does, so a black and white picture can be picked
    // for too.
    std::vector<uint8_t> grey_rgb;
    const uint8_t       *rgb = source.rgb.data();
    if (!source.has_color()) {
        grey_rgb.resize(source.pixels.size() * 3);
        for (size_t i = 0; i < source.pixels.size(); ++i)
            grey_rgb[i * 3] = grey_rgb[i * 3 + 1] = grey_rgb[i * 3 + 2] = source.pixels[i];
        rgb = grey_rgb.data();
    }

    const int relief = std::max(CHROMA_DEFAULT_RELIEF_LAYERS, layer.chroma.relief_layers());
    // Same weighting as the suggestion: this is the other half of the same question, and a picker that
    // served the background while ignoring the subject is what the weighting exists to stop.
    ChromaStack picked = chroma_auto_stack(chroma_histogram(rgb, source.width, source.height, 192,
                                                            CHROMA_VIVIDNESS_RECOMMENDED, layer.chroma.importance),
                                           chroma_filaments(), count, layer.chroma.layer_height_mm,
                                           layer.chroma.base_layers, relief);
    if (picked.bands.empty())
        return;
    // Spending layers on colours the one below already showed is never what was asked for.
    chroma_trim_bands(picked);
    picked.base_auto      = layer.chroma.base_auto;
    picked.dither         = layer.chroma.dither;
    picked.applied_ranges = layer.chroma.applied_ranges;
    layer.chroma          = std::move(picked);
    m_chroma_pick_count[size_t(layer.slot)] = count;
    chroma_touch(layer.slot);
}

std::vector<std::string> GLGizmoTextureDisplacement::chroma_problems(const TextureDisplacementLayer &layer) const
{
    std::vector<std::string> out;
    if (!layer.chroma_enabled)
        return out;

    if (chroma_swaps_blocked(layer))
        out.push_back(_u8L("This object has height ranges that did not come from this picture, so the filament swaps "
                           "have not been written and it would print in one color. Click here to replace them."));

    // Printed face down, the picture's own surface is the first layer. A brim is drawn around that
    // layer and a raft is printed under it: either one lands on the face the whole mode exists to keep
    // smooth, and it is found out by peeling it off a finished print.
    if (layer.chroma.plate.generated && layer.chroma.plate.orientation == ChromaOrientation::FaceDown) {
        // Face down the whole stack prints above the picture, so the plate has to be at least as thick
        // as the stack is tall. Switching the mode on sets it to that and nothing holds it there
        // afterwards: one more layer in one band is enough to cut the deepest column out through the
        // back, and the preview cannot show it because the hole is in the part of the plate the relief
        // was supposed to stop short of.
        const float needed = layer.chroma.total_mm();
        if (needed > layer.chroma.plate.base_mm + 1e-4f)
            out.push_back(Slic3r::format(_u8L("This plate prints picture down and is %1$.1f mm thick, but the stack "
                                              "above the picture is %2$.1f mm. The relief is cut up into the plate "
                                              "from underneath, so the deepest part of the picture opens out through "
                                              "the back. Make the plate thicker."),
                                         layer.chroma.plate.base_mm, needed));
        if (const PresetBundle *bundle = wxGetApp().preset_bundle; bundle != nullptr) {
            const DynamicPrintConfig &print = bundle->prints.get_edited_preset().config;
            const bool brim = print.has("brim_type") && print.opt_enum<BrimType>("brim_type") != btNoBrim &&
                              (!print.has("brim_width") || print.opt_float("brim_width") > 0.);
            const bool raft = print.has("raft_layers") && print.opt_int("raft_layers") > 0;
            if (brim)
                out.push_back(_u8L("This plate prints picture down, and the print profile has a brim. The brim is "
                                   "drawn around the first layer, which here is the picture's own face - turn it off, "
                                   "or it prints onto the picture."));
            if (raft)
                out.push_back(_u8L("This plate prints picture down, and the print profile has a raft. The raft is "
                                   "printed under the first layer, which here is the picture's own face - turn it "
                                   "off, or the picture comes off the raft rather than off the build plate."));
        }
    }

    // ---- The relief never reached the surface ----
    //
    // The colours are read from each fragment's own height off the surface the overlay is drawn on, so
    // they need the relief to be in that surface. Without it every point of the picture sits at the
    // same height and gets the same answer - the top of the stack, everywhere - and the plate comes out
    // one flat colour, which looks exactly like a correct solve of a very dark picture. It is the single
    // most confusing thing this feature can do, and four quite different faults end in it.
    //
    // So each is tested for rather than the likeliest one being named: a message that asserts a cause it
    // did not check sends the reader to the wrong control, and they then have no way to tell that the
    // advice was wrong rather than the feature broken. The order is the order they mask each other in.
    if (m_show_chroma_colors) {
        const ModelVolume *mv = texture_volume();
        if (layer.empty())
            // Deliberately first, and deliberately not behind a test for a non-empty layer: a layer with
            // no height map is the one case where there is nothing to displace with at all, and gating
            // the whole list on having one is what would hide it.
            out.push_back(_u8L("This layer has no solved height map, so there is no relief to read the colors "
                               "off and the plate comes out one color. Solve the picture again."));
        else if (m_use_shaded_preview)
            out.push_back(_u8L("The View is set to Fast, which shades the relief on instead of building it, so the "
                               "colors are read off a flat plate and come out as one color. Switch the View to "
                               "Normal to see them."));
        else if (mv == nullptr || !mv->is_texture_displacement_painted())
            out.push_back(_u8L("No part of this object is marked as the picture's face, so there is nothing for the "
                               "relief to be built on and the colors are read off a flat plate. Paint the face the "
                               "picture goes on, or rebuild the plate."));
        else if (m_preview_its.indices.empty())
            out.push_back(_u8L("The relief has not been built yet, so the colors are read off a flat plate for now. "
                               "If it stays this way, the preview is not finishing."));
        else if (!m_preview_displaced)
            out.push_back(_u8L("The relief came back flat: the surface was built, but nothing moved. Either the solve "
                               "has no height map for this layer yet, or the picture is not landing on the painted "
                               "face - the 'Where the picture lands' debug view says which."));
    }

    const ChromaPatch patch = chroma_patch(layer);
    if (patch.valid && patch.tilt_deg > CHROMA_MAX_TILT_DEG)
        out.push_back(Slic3r::format(_u8L("Part of the painted area is steeper than %1$.0f degrees - it reaches "
                                          "%2$.0f. The relief there cannot be made deep enough to reach the heights "
                                          "its colors were solved for, so those parts print a color of their own."),
                                     CHROMA_MAX_TILT_DEG, patch.tilt_deg));

    // The error that tilting cannot fix: a swap is one height across the whole plate, so a surface
    // that rises across itself carries its own colors with it. Reported in bands rather than in
    // millimetres, because a band is what a reader of the picture would see change.
    if (patch.valid && layer.chroma.layer_height_mm > 0.f) {
        const int drift = int(patch.spread_mm / layer.chroma.layer_height_mm + 0.5f);
        if (drift > 1)
            out.push_back(Slic3r::format(_u8L("The painted area rises %1$.1f mm across itself, which is %2% layers. "
                                              "A filament swap happens at one height across the whole plate, so the "
                                              "picture's colors shift by that much from its lowest point to its "
                                              "highest."),
                                         patch.spread_mm, drift));
    }

    if (layer.chroma.relief_layers() > CHROMA_MAX_RELIEF_LAYERS)
        out.push_back(Slic3r::format(_u8L("The picture cannot be more than %1% layers tall. Give the bands fewer "
                                          "layers, or raise the layer height."),
                                     CHROMA_MAX_RELIEF_LAYERS));
    return out;
}

// Whether a band still describes the filament the project actually has in that slot.
//
// A band stores its filament's index together with the name, colour and transmission distance it was
// solved against, because a stack outlives the project's filament list: a preset can be swapped out
// from under a saved project, and then what the stack stored is the only record of what the colours
// were computed from. The cost of storing it is that the two can come to disagree in silence, and
// every colour the card predicts is then computed for a filament that is no longer there.
//
// So it is reported rather than corrected. Adopting the project's filament re-solves the picture and
// may change every colour in it, which is the user's call and not the panel's.
struct ChromaBandDrift
{
    bool missing = false; // the slot this band names is not loaded at all
    bool color   = false;
    bool td      = false;
    bool name    = false;

    bool any() const { return missing || color || td || name; }
};

static ChromaBandDrift chroma_band_drift(const ChromaFilament &stored, const std::vector<ChromaFilament> &loaded)
{
    ChromaBandDrift out;
    if (stored.filament < 0 || size_t(stored.filament) >= loaded.size()) {
        out.missing = true;
        return out;
    }
    const ChromaFilament &now = loaded[size_t(stored.filament)];
    // One 8-bit step of tolerance: a colour that went out as a hex string and came back is not an edit.
    const float tol = 2.f / 255.f;
    out.color       = std::abs(now.color.r() - stored.color.r()) > tol ||
                std::abs(now.color.g() - stored.color.g()) > tol ||
                std::abs(now.color.b() - stored.color.b()) > tol;
    // A twentieth of a millimetre of TD moves no colour the eye can find, and the field itself only
    // shows one decimal.
    out.td = std::abs(now.td_mm - stored.td_mm) > 0.05f;
    // Only when both sides have a name to compare: a stack saved before names were stored has none,
    // and an unnamed band is not evidence that anything changed.
    out.name = !stored.name.empty() && !now.name.empty() && stored.name != now.name;
    return out;
}

// Amber for "these filaments cannot reach this picture", green for "they do". The only two states the
// card states in colour rather than in words, and they are file-local because the card and the
// readiness line below have to agree about them.
static const ImVec4 CHROMA_COL_WARN = ImVec4(0.94f, 0.65f, 0.24f, 1.f);
static const ImVec4 CHROMA_COL_GOOD = ImVec4(0.39f, 0.69f, 0.51f, 1.f);

GLGizmoTextureDisplacement::ChromaReadiness
GLGizmoTextureDisplacement::chroma_readiness(const TextureDisplacementLayer &layer) const
{
    ChromaReadiness out;
    out.color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

    if (!layer.chroma_source) {
        out.text = _u8L("Choose a picture to print.");
        return out;
    }
    if (layer.chroma.bands.size() < 2) {
        out.text = _u8L("Pick at least two filaments.");
        return out;
    }

    const size_t slot = size_t(layer.slot);
    if (m_chroma_settle_at[slot] != 0 || m_chroma_solve_running[slot] || m_chroma_plate_running[slot]) {
        out.color = ImGuiWrapper::COL_ORCA;
        out.text  = _u8L("Working out the heights.");
        return out;
    }

    // Past this point nothing stops a bake. A problem is still worth the top line - it is the worst
    // thing the card knows - but a brim that would print across the picture is a setting to go and
    // change, not a reason the panel should refuse to build the mesh.
    out.ready = true;

    // Deliberately not the worst problem. A problem does not stop a bake, and spelling one out here
    // made this line four lines long - a paragraph where the card wants a state. The problems are on
    // the warning mark beside it, all of them, where a reader who wants them can read them and a
    // reader who does not is not made to. See render_chroma_card().

    const ChromaReport &report = m_chroma_report[slot];
    if (!report.valid) {
        out.text = _u8L("Ready to bake.");
        return out;
    }
    if (report.mean > 15.) {
        out.color = CHROMA_COL_WARN;
        out.text  = _u8L("Your filaments cannot reach these colors.");
        return out;
    }
    out.color = CHROMA_COL_GOOD;
    out.text  = report.mean <= 5. ? _u8L("Good match. Ready to bake.") : _u8L("Close enough. Ready to bake.");
    return out;
}

void GLGizmoTextureDisplacement::render_chroma_card(TextureDisplacementLayer &layer, float card_pad,
                                                    float content_w)
{
    ChromaProfileScope prof("chroma_card_frame", {}, 8.0);
    const ImGuiStyle &style   = ImGui::GetStyle();
    const float       frame_h = ImGui::GetFrameHeight();
    const float       gap_s   = std::round(m_imgui->scaled(0.4f));
    const size_t      slot    = size_t(layer.slot);
    ImDrawList       *dl      = ImGui::GetWindowDrawList();

    // The current window's draw list, not the panel's captured one: a swatch is also drawn from inside
    // the popups below, and the panel's list is clipped to the panel and drawn under them.
    const auto swatch = [&](const ColorRGB &c, float size) {
        ImDrawList  *wdl = ImGui::GetWindowDrawList();
        const ImVec2 p   = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, frame_h));
        const float y = std::round(p.y + 0.5f * (frame_h - size));
        wdl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + size, y + size),
                           IM_COL32(int(c.r() * 255.f), int(c.g() * 255.f), int(c.b() * 255.f), 255), style.FrameRounding);
        wdl->AddRect(ImVec2(p.x, y), ImVec2(p.x + size, y + size), ImGui::GetColorU32(ImGuiCol_Separator), style.FrameRounding);
    };
    const ImVec4 &col_warn = CHROMA_COL_WARN;
    const ImVec4 &col_good = CHROMA_COL_GOOD;

    // A row's label, clamped to one width so every field in the card starts at the same x. Three
    // weights and no more: a heading, a setting row like this one, and a derived line that is never
    // interactive.
    // A section that closes, with its summary in the same bar so folding it costs no information.
    // Returns whether it is open.
    const auto closable = [&](const char *id, const wxString &title, const std::string &summary, bool &open) {
        const float  x0 = ImGui::GetCursorPosX();
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const float  w  = std::max(m_imgui->scaled(2.f),
                                   ImGui::GetContentRegionMax().x - x0 - std::max(card_pad, 1.f));
        if (ImGui::InvisibleButton(id, ImVec2(w, frame_h)))
            open = !open;
        const bool  hot  = ImGui::IsItemHovered();
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : 0.9f);
        // The disclosure triangle, pointing down when open. Drawn rather than taken from a font so it
        // keeps its size when the panel is scaled.
        const float a  = std::round(ImGui::GetFontSize() * 0.3f);
        const float cx = p.x + a + 2.f, cy = p.y + 0.5f * frame_h;
        if (open)
            dl->AddTriangleFilled(ImVec2(cx - a, cy - 0.5f * a), ImVec2(cx + a, cy - 0.5f * a),
                                  ImVec2(cx, cy + 0.7f * a), text);
        else
            dl->AddTriangleFilled(ImVec2(cx - 0.5f * a, cy - a), ImVec2(cx + 0.7f * a, cy),
                                  ImVec2(cx - 0.5f * a, cy + a), text);
        const std::string name = into_u8(title);
        dl->AddText(ImVec2(cx + a + 4.f, cy - 0.5f * ImGui::GetFontSize()), text, name.c_str());
        if (!summary.empty())
            dl->AddText(ImVec2(p.x + w - ImGui::CalcTextSize(summary.c_str()).x, cy - 0.5f * ImGui::GetFontSize()),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), summary.c_str());
        ImGui::SetCursorPosX(x0);
        return open;
    };
    // The same control for a choice whose options are words rather than numbers. `current` is the
    // index of the chosen one.
    const auto segmented_text = [&](const char *id, const std::vector<std::string> &labels, int &current,
                                    float total_w) {
        bool        changed = false;
        const float each    = std::max(m_imgui->scaled(1.2f), std::floor(total_w / float(labels.size())));
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.f, ImGui::GetStyle().ItemSpacing.y));
        for (size_t i = 0; i < labels.size(); ++i) {
            const bool on = int(i) == current;
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGuiWrapper::COL_ORCA);
            ImGui::PushID(int(i));
            if (ImGui::Button(labels[i].c_str(), ImVec2(each, frame_h))) {
                current = int(i);
                changed = true;
            }
            ImGui::PopID();
            if (on)
                ImGui::PopStyleColor();
            if (i + 1 < labels.size())
                ImGui::SameLine(0.f, 1.f);
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    };
    // A row of mutually exclusive choices drawn as one control. Replaces a slider for a count nobody
    // drags: picking 4 out of five options is one click, where a slider is a drag and a read-back.
    const auto segmented = [&](const char *id, const std::vector<int> &values, int &current, float total_w) {
        bool        changed = false;
        const float each    = std::max(m_imgui->scaled(1.2f), std::floor(total_w / float(values.size())));
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.f, ImGui::GetStyle().ItemSpacing.y));
        for (size_t i = 0; i < values.size(); ++i) {
            const bool on = values[i] == current;
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGuiWrapper::COL_ORCA);
            ImGui::PushID(int(i));
            if (ImGui::Button(std::to_string(values[i]).c_str(), ImVec2(each, frame_h))) {
                current = values[i];
                changed = true;
            }
            ImGui::PopID();
            if (on)
                ImGui::PopStyleColor();
            if (i + 1 < values.size())
                ImGui::SameLine(0.f, 1.f);
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    };

    // ---- The height a layer is actually printed at ----
    //
    // The stack takes it from the print profile when it is made, and the profile can be changed
    // afterwards by anything: another preset, another printer, a nozzle. A stack left at the height it
    // was born with puts its swaps at Z values no layer boundary lands on, and the slicer rounds every
    // one of them to whatever layer it falls in - which is how a picture comes out of the slicer as a
    // slab of one colour with the other three crowded into the top millimetre. The card's own
    // prediction knows nothing about it, because the card is drawing the stack and the stack is
    // consistent with itself.
    if (const float printed = print_layer_height();
        printed > 0.f && std::abs(printed - layer.chroma.layer_height_mm) > 1e-4f) {
        layer.chroma.layer_height_mm = printed;
        chroma_touch(layer.slot);
    }

    // ---- Nothing chosen yet ----
    if (!layer.chroma_source) {
        if (m_imgui->button(_L("Choose a picture")))
            chroma_pick_picture(layer);
        hover_tip(_u8L("The color picture to reproduce. The height map is computed from it, and recomputed whenever the "
                 "stack changes."));
        return;
    }

    // Sized from the width the card is allowed, never from the width it has left. The two thumbnails
    // are square, so a size taken from the live region ties the panel's height to its width: a
    // scrollbar appears, the region narrows, the thumbnails shrink, the panel gets shorter, the
    // scrollbar goes, and it does it all again next frame. The panel flickered between two widths.
    const float thumb = std::max(m_imgui->scaled(2.f), std::round(0.5f * (content_w - gap_s)));
    if (m_chroma_source_key[slot] != layer.chroma_source.get()) {
        TextureDisplacementLayer probe;
        probe.image_data            = layer.chroma_source;
        m_chroma_source_thumb[slot] = upload_height_thumbnail(decode_height_texture(probe));
        m_chroma_source_key[slot]   = layer.chroma_source.get();
    }
    // One preview: the image, a frame that carries the match, a chip naming it in figures, and a
    // caption. Clicking it opens the pair at the size of the canvas, because judging a face against a
    // print is not a thing a 190 px square can settle.
    const auto pane = [&](GLTexture *tex, const std::string &caption, const std::string &tip_text,
                          const ImVec4 *ring, const std::string &chip, const ImVec4 *chip_col) {
        ImGui::BeginGroup();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (tex != nullptr)
            ImGui::Image((ImTextureID) (intptr_t) tex->get_id(), ImVec2(thumb, thumb));
        else {
            ImGui::Dummy(ImVec2(thumb, thumb));
            dl->AddRect(p, ImVec2(p.x + thumb, p.y + thumb), ImGui::GetColorU32(ImGuiCol_Separator));
        }
        // Over the image rather than beside it: the picture is the control, and a separate button for
        // "look closer" would be a seventh thing in a card that already has too many.
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##zoom", ImVec2(thumb, thumb)) && tex != nullptr)
            m_chroma_zoom_slot = layer.slot;
        const bool hot = ImGui::IsItemHovered();
        // How good the match is, as the colour of the prediction's own frame. It is a property of that
        // picture, so it belongs on it rather than on a line of its own saying so in words.
        if (ring != nullptr)
            dl->AddRect(ImVec2(p.x - 1.f, p.y - 1.f), ImVec2(p.x + thumb + 1.f, p.y + thumb + 1.f),
                        ImGui::GetColorU32(*ring), 0.f, 0, 2.f);
        if (!chip.empty() && chip_col != nullptr) {
            const ImVec2 ts  = ImGui::CalcTextSize(chip.c_str());
            const float  pad = std::round(m_imgui->scaled(0.2f));
            const ImVec2 c0(p.x + pad, p.y + thumb - ts.y - 3.f * pad);
            const ImVec2 c1(c0.x + ts.x + 2.f * pad, c0.y + ts.y + 2.f * pad);
            dl->AddRectFilled(c0, c1, ImGui::GetColorU32(*chip_col), style.FrameRounding);
            dl->AddText(ImVec2(c0.x + pad, c0.y + pad), IM_COL32(20, 20, 20, 255), chip.c_str());
        }
        if (tex != nullptr && hot) {
            // A magnifier, only while the pointer is on the image: an affordance that is always there
            // is one more mark on a card full of them.
            const float r  = std::round(m_imgui->scaled(0.3f));
            const ImVec2 c(p.x + thumb - 2.5f * r, p.y + 2.5f * r);
            dl->AddCircleFilled(c, r * 1.9f, IM_COL32(0, 0, 0, 120));
            dl->AddCircle(ImVec2(c.x - 0.15f * r, c.y - 0.15f * r), r * 0.8f, IM_COL32(255, 255, 255, 230), 0, 1.4f);
            dl->AddLine(ImVec2(c.x + 0.4f * r, c.y + 0.4f * r), ImVec2(c.x + r, c.y + r),
                        IM_COL32(255, 255, 255, 230), 1.4f);
        }
        if (hot)
            panel_tooltip(tip_text + "\n\n" + _u8L("Click to see both at the size of the canvas."));
        ImGui::TextDisabled("%s", caption.c_str());
        ImGui::EndGroup();
    };

    const ChromaReport &report  = m_chroma_report[slot];
    // Waiting for the controls to settle, or actually computing. The ring and the tooltip mean the
    // same thing to the user either way: what is on screen is the previous answer, and a better one
    // is on its way.
    const bool          solving = m_chroma_settle_at[slot] != 0 || m_chroma_solve_running[slot] ||
                                  m_chroma_plate_running[slot];
    const ImVec4        ring    = solving       ? ImGuiWrapper::COL_ORCA :
                                  !report.valid ? ImGui::GetStyleColorVec4(ImGuiCol_Separator) :
                                  report.mean > 15.  ? col_warn :
                                  report.mean <= 5.  ? col_good :
                                                       ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    pane(m_chroma_source_thumb[slot].get(), _u8L("Your picture"),
         _u8L("The picture being reproduced. Click the layer's name to choose another."), nullptr, {}, nullptr);
    ImGui::SameLine(0.f, gap_s);
    // While the error view is on, the second pane shows the error rather than the prediction, so the
    // card and the model are never saying two different things about the same solve.
    const bool error_pane = m_show_chroma_error && m_chroma_error_tex[slot];
    // The match in figures, on the prediction itself. The frame's colour already says which band it is
    // in; the number is what tells one near miss from another.
    // The chip always carries something, including when the answer is "still working" and when the
    // match is neither good nor bad. An indicator that disappears in the middle of its own range is an
    // indicator nobody learns to read: the reader cannot tell a middling match from a stale preview,
    // from a solve that has not run, or from a chip they have misremembered.
    const ImVec4  chip_plain = ImVec4(0.84f, 0.84f, 0.84f, 1.f);
    std::string   chip;
    const ImVec4 *chip_col = nullptr;
    if (solving) {
        chip     = _u8L("solving");
        chip_col = &chip_plain;
    } else if (report.valid) {
        // TRN A color difference, as shown on the ChromaStack prediction: "3.4 off" means the print
        // lands 3.4 units of color difference away from the picture on average.
        chip     = Slic3r::format(_u8L("%1$.1f off"), report.mean);
        chip_col = report.mean > 15. ? &col_warn : report.mean <= 5. ? &col_good : &chip_plain;
    }
    pane(error_pane ? m_chroma_error_tex[slot].get() : m_chroma_preview[slot].get(),
         error_pane ? _u8L("Where it is wrong") : _u8L("What prints"),
         error_pane   ? _u8L("How far this stack lands from the picture, pixel by pixel: dark where it "
                             "matches, bright where the filaments cannot reach it. Turn Error off in the "
                             "View row to see the predicted print again.") :
         solving       ? _u8L("Working out the heights for this stack.") :
         !report.valid ? _u8L("What this stack is predicted to print.") :
                         Slic3r::format(_u8L("What this stack is predicted to print, %1$.1f from the picture on "
                                             "average and %2$.1f at worst, as a color difference.\n\n"
                                             "Under 5 is a good match. Over 15 means these filaments cannot reach the "
                                             "picture's colors and more layers will not help: change a filament, or "
                                             "ask for a different number of colors."),
                                        report.mean, report.max),
         &ring, chip, chip_col);

    // ---- Whether it is ready, what can be done about it, and the one correction that is not a
    //      change to the whole picture ----
    //
    // One sentence, said once. This line is the card's answer to "can I print this yet"; Bake's
    // disabled tooltip is the same sentence from the same function, and the worst problem is the same
    // sentence again, which is why there is no separate bar of problems above the previews any more.
    // Two copies of one state are two things that can come to disagree, and a reader who meets the
    // same sentence twice in two weights goes looking for the difference between them.
    //
    // Under it, a row of actions that is always there, in the same place, whatever the state. Fix an
    // area corrects what the previews above show; Color analysis answers the question the line itself
    // asks. Neither appears or disappears, because a control that appears has moved everything below
    // it while the reader was looking somewhere else.
    {
        const std::vector<std::string> problems = chroma_problems(layer);
        const ChromaReadiness          state    = chroma_readiness(layer);
        const float                    x0       = ImGui::GetCursorPosX();

        ImGui::AlignTextToFramePadding();
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  r = std::max(2.5f, std::round(frame_h * 0.11f));
            ImGui::Dummy(ImVec2(2.f * r + gap_s, frame_h));
            dl->AddCircleFilled(ImVec2(p.x + r, p.y + 0.5f * frame_h), r, ImGui::GetColorU32(state.color));
        }
        ImGui::SameLine(0.f, gap_s);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, state.color);
        ImGui::TextUnformatted(state.text.c_str());
        ImGui::PopStyleColor();

        // Everything else that is wrong, as one mark. A problem is a paragraph and there can be
        // several of them; printed on the card they were the tallest thing on it and they pushed the
        // picture they are about off the top of the panel. The mark is the whole of their presence
        // here, and the tooltip is all of them in full.
        if (!problems.empty()) {
            ImGui::SameLine(0.f, gap_s);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  a = std::round(frame_h * 0.30f);
            ImGui::InvisibleButton("##hf_problems", ImVec2(2.f * a + gap_s, frame_h));
            const float cx = p.x + a, cy = p.y + 0.5f * frame_h;
            const ImU32 col = ImGui::GetColorU32(col_warn);
            dl->AddTriangleFilled(ImVec2(cx, cy - a), ImVec2(cx + a, cy + 0.75f * a),
                                  ImVec2(cx - a, cy + 0.75f * a), col);
            // The bar of the exclamation mark, knocked out of the triangle rather than drawn over it,
            // so it reads at the size the panel actually gets on a high-DPI screen.
            const ImU32 hole = ImGui::GetColorU32(ImGuiCol_WindowBg);
            dl->AddRectFilled(ImVec2(cx - 0.11f * a, cy - 0.35f * a), ImVec2(cx + 0.11f * a, cy + 0.2f * a), hole);
            dl->AddRectFilled(ImVec2(cx - 0.11f * a, cy + 0.38f * a), ImVec2(cx + 0.11f * a, cy + 0.6f * a), hole);
            if (ImGui::IsItemHovered()) {
                std::string all;
                for (const std::string &one : problems)
                    all += (all.empty() ? "" : "\n\n") + one;
                panel_tooltip(all);
            }
        }

        // ---- the action row ----
        ImGui::SetCursorPosX(x0);
        const bool        was_on = m_chroma_retouch_mode;
        const std::string fix    = was_on ? _u8L("Done fixing") : _u8L("Fix an area");
        if (m_imgui->button(from_u8(fix))) {
            m_chroma_retouch_mode = !was_on;
            if (!m_chroma_retouch_mode && m_chroma_retouch_painting)
                chroma_commit_stroke(layer);
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            panel_tooltip(was_on ?
                              _u8L("Stops the brush, so dragging turns the model again.") :
                              _u8L("Paint over the print to correct it: the area you paint moves to the color you "
                                   "choose here, and the rest of the picture is left exactly as the solve decided. "
                                   "Turn on the error view, under Views, first - it shows you where correcting is "
                                   "worth the trouble.\n\nHold Ctrl to turn the model while the brush is on."));

        // The report, as the answer to the line above rather than as a button of its own further down
        // the card: the findings are the why behind the sentence. Disabled rather than absent when the
        // solve found nothing to say, so the row keeps its shape.
        const ChromaDiagnostics &diag     = m_chroma_diag[slot];
        const bool               analysed = !diag.empty();
        ImGui::SameLine(0.f, gap_s);
        m_imgui->disabled_begin(!analysed);
        if (ImGui::Button(_u8L("Color analysis").c_str()) && analysed)
            m_chroma_diag_slot = layer.slot;
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered())
            panel_tooltip(analysed ?
                              _u8L("The picture's colors and the stack's own reach, drawn in one chart, with what is "
                                   "wrong with this choice of filaments written out underneath. The error view says "
                                   "where a solve failed; this says why.") :
                              _u8L("The picture's colors and the stack's own reach in one chart. It opens once a "
                                   "solve has something to say about this choice of filaments."));

        // What else is wrong, or what has been corrected. On the right of the same row, because it is
        // a count and not an action.
        if (const int fixes = int(layer.chroma.retouch.edits.size()); fixes > 0) {
            const std::string tail = Slic3r::format(into_u8(_L_PLURAL("%1% correction", "%1% corrections", fixes)),
                                                    fixes);
            const float       w    = ImGui::CalcTextSize(tail.c_str()).x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          ImGui::GetContentRegionMax().x - std::max(card_pad, 1.f) - w));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", tail.c_str());
        }

        // The one action a problem can carry, and the only thing in this block that is not always
        // offered: swaps that did not come from this picture are the one thing here the card itself
        // can put right. It takes a row of its own rather than a fourth place on the row above, which
        // at this width is where a control goes to be clipped.
        if (chroma_swaps_blocked(layer)) {
            ImGui::SetCursorPosX(x0);
            if (m_imgui->button(_L("Replace them")))
                chroma_apply_swaps(layer);
            if (ImGui::IsItemHovered())
                panel_tooltip(_u8L("Writes this picture's own filament swaps over the height ranges the object "
                                   "already had."));
        }
    }

    // ---- Fixing what the solve got wrong ----
    //
    // Everything else on this card changes the whole picture to put one part of it right. This does
    // not: a stroke over the print says what that area should have been, and the solve honours it
    // there and nowhere else. It sits with the previews because it is a correction of what they show.
    {
        if (m_chroma_retouch_mode) {
            const float x0    = ImGui::GetCursorPosX();
            const float row_w = std::max(m_imgui->scaled(2.f),
                                         ImGui::GetContentRegionMax().x - x0 - std::max(card_pad, 1.f));
            // Three words rather than a combo. Three is the whole choice, so a combo hides two of them
            // behind a click and saves no room doing it - and it was the combo that changed the kind
            // and the width of the control beside it, which is what made the brush slider move.
            if (segmented_text("##hf_fix_kind", { _u8L("Layers"), _u8L("Filament"), _u8L("Matters") },
                               m_chroma_retouch_kind, row_w))
                m_parent.set_as_dirty();
            if (ImGui::IsItemHovered())
                panel_tooltip(_u8L("What the brush does to what it covers. Moving by layers nudges that area up or "
                                   "down the stack, which is the gentler correction and keeps the shading. Using a "
                                   "filament sets the area to the top of one band, which is what a flat area of one "
                                   "color wants. Marking what matters changes no height: it tells the solve which "
                                   "part of the picture to choose the filaments for."));

            // Then two rows, and always these two. The control on the Amount row changes kind with the
            // mode, but neither its place nor its width does, so nothing under it moves when the mode
            // is changed.
            const float label_w = m_imgui->scaled(4.2f);
            const float field_w = std::max(m_imgui->scaled(3.f), row_w - label_w - frame_h - 2.f * gap_s);

            ImGui::SetCursorPosX(x0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("Amount").c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
            ImGui::SetNextItemWidth(field_w);
            if (m_chroma_retouch_kind == 0) {
                if (ImGui::InputInt("##hf_fix_delta", &m_chroma_retouch_delta, 1, 1))
                    m_chroma_retouch_delta = std::clamp(m_chroma_retouch_delta, -CHROMA_MAX_RELIEF_LAYERS,
                                                        CHROMA_MAX_RELIEF_LAYERS);
                if (ImGui::IsItemHovered())
                    panel_tooltip(_u8L("How many printed layers to move the area, up for a later color and down for "
                                       "an earlier one. One layer is usually the whole correction."));
            } else if (m_chroma_retouch_kind == 1) {
                const int   bands      = int(layer.chroma.bands.size());
                std::string band_label = _u8L("the base");
                if (m_chroma_retouch_band >= 0 && m_chroma_retouch_band < bands)
                    band_label = Slic3r::format(_u8L("band %1%"), m_chroma_retouch_band + 1);
                push_panel_combo_style(m_parent.get_scale());
                if (ImGui::BeginCombo("##hf_fix_band", band_label.c_str())) {
                    if (ImGui::Selectable(_u8L("the base").c_str(), m_chroma_retouch_band < 0))
                        m_chroma_retouch_band = -1;
                    for (int bi = 0; bi < bands; ++bi) {
                        ImGui::PushID(bi);
                        const bool picked = ImGui::Selectable("", m_chroma_retouch_band == bi, 0, ImVec2(0.f, frame_h));
                        ImGui::SameLine(0.f, 0.f);
                        swatch(layer.chroma.bands[size_t(bi)].filament.color, std::round(frame_h * 0.7f));
                        ImGui::SameLine(0.f, gap_s);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(Slic3r::format(_u8L("band %1%"), bi + 1).c_str());
                        if (picked)
                            m_chroma_retouch_band = bi;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                pop_panel_combo_style();
                if (ImGui::IsItemHovered())
                    panel_tooltip(_u8L("Which band the painted area is taken up to. The base is the picture at its "
                                       "thinnest, showing the backing alone."));
            } else {
                if (typed_slider("##hf_fix_weight", field_w, &m_chroma_importance_weight, 1.f, 10.f, "%.0fx", false,
                                 _L("How much more the marked area counts than its size says it should. The "
                                    "filaments are chosen for the whole picture, and the background of a "
                                    "photograph is most of its area - so without this the one thing the picture "
                                    "is of gets outvoted by the wall behind it.")))
                    m_chroma_importance_weight = std::max(1.f, m_chroma_importance_weight);
            }

            // On the row in every mode, so the row is the same shape in all three. There is only one
            // thing on this card that can be forgotten, and it is the marked area.
            const bool can_clear = m_chroma_retouch_kind == 2 && !layer.chroma.importance.region.empty();
            ImGui::SameLine(0.f, gap_s);
            m_imgui->disabled_begin(!can_clear);
            if (m_imgui->button("x##hf_fix_imp_clear") && can_clear) {
                layer.chroma.importance = ChromaImportance{};
                chroma_touch(layer.slot);
            }
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered())
                panel_tooltip(can_clear ? _u8L("Forgets the marked area, so the whole picture counts equally again.") :
                                          _u8L("Forgets the marked area. Nothing is marked yet."));

            ImGui::SetCursorPosX(x0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("Brush").c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
            if (typed_slider("##hf_fix_radius", field_w, &m_chroma_retouch_radius_mm, 0.5f, 25.f, "%.1f mm", false,
                             _L("How wide the brush is, measured on the print rather than on the screen, so it "
                                "covers the same amount of the picture however far you have zoomed in.")))
                m_parent.set_as_dirty();
        }

        // The edits themselves, newest last, each one a stroke. Shown only when there are any: a card
        // with nothing wrong on it says nothing.
        std::vector<ChromaRetouchEdit> &edits = layer.chroma.retouch.edits;
        int                             drop  = -1;
        for (size_t ei = 0; ei < edits.size(); ++ei) {
            ChromaRetouchEdit &edit = edits[ei];
            ImGui::PushID(int(1000 + ei));
            if (m_imgui->bbl_checkbox(wxString("##hf_fix_on"), edit.enabled))
                chroma_touch(layer.slot);
            if (ImGui::IsItemHovered())
                panel_tooltip(_u8L("Turns this correction off without losing it, so you can see what it is doing."));
            ImGui::SameLine(0.f, gap_s);
            ImGui::AlignTextToFramePadding();
            std::string said;
            if (edit.target == ChromaRetouchEdit::Target::Delta) {
                const int moved = std::abs(edit.value);
                said = edit.value >= 0 ?
                           Slic3r::format(into_u8(_L_PLURAL("Fix %1%: %2% layer up", "Fix %1%: %2% layers up", moved)),
                                          int(ei + 1), moved) :
                           Slic3r::format(into_u8(_L_PLURAL("Fix %1%: %2% layer down", "Fix %1%: %2% layers down",
                                                            moved)),
                                          int(ei + 1), moved);
            } else if (edit.value < 0)
                said = Slic3r::format(_u8L("Fix %1%: the base"), int(ei + 1));
            else
                said = Slic3r::format(_u8L("Fix %1%: band %2%"), int(ei + 1), edit.value + 1);
            ImGui::TextDisabled("%s", said.c_str());
            ImGui::SameLine(0.f, gap_s);
            if (m_imgui->button("x##hf_fix_del"))
                drop = int(ei);
            if (ImGui::IsItemHovered())
                panel_tooltip(_u8L("Removes this correction."));
            ImGui::PopID();
        }
        if (drop >= 0) {
            edits.erase(edits.begin() + drop);
            chroma_touch(layer.slot);
        }
    }

    // ---- The colours the picture is printed in ----
    //
    // The ramp first, because it is the whole stack's own readout and answers "is this worth printing"
    // before any one band is worth looking at; then the bands, top of the stack first, the way the
    // print is looked at; then the two ways of choosing them again from scratch.
    ImGui::Dummy(ImVec2(1.f, gap_s));
    const std::vector<ChromaFilament> loaded   = chroma_filaments();
    const std::vector<ColorRGB>       palette  = chroma_palette(layer.chroma);
    const int                         distinct = chroma_distinct_levels(layer.chroma);
    const int                         relief   = layer.chroma.relief_layers();
    const bool                        has_ramp = int(palette.size()) > layer.chroma.base_layers;

    // Closable like the rest of them, and open to begin with. The summary carries the filament count,
    // which is the one thing about the stack worth reading while the section is folded. It carries
    // only that: a summary assembled out of two translated fragments is a sentence no translator can
    // put in the order their language wants, and how many colours the stack can print is already
    // written out under the ramp.
    const bool colors_open = closable("##hf_sec_colors", _L("Colors"),
                                      Slic3r::format(into_u8(_L_PLURAL("%1% filament", "%1% filaments",
                                                                       int(layer.chroma.bands.size()))),
                                                     int(layer.chroma.bands.size())),
                                      m_chroma_open_colors[slot]);
    if (colors_open) {
        if (has_ramp) {
            const float  w     = std::max(m_imgui->scaled(2.f),
                                          ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f));
            const float  h     = frame_h;
            const ImVec2 p     = ImGui::GetCursorScreenPos();
            const int    first = std::max(0, layer.chroma.base_layers);
            const int    count = int(palette.size()) - first;
            ImGui::Dummy(ImVec2(w, h));
            for (int i = 0; i < count; ++i) {
                const ColorRGB &c  = palette[size_t(first + i)];
                const float     x0 = p.x + w * float(i) / float(count);
                const float     x1 = p.x + w * float(i + 1) / float(count);
                dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h),
                                  IM_COL32(int(c.r() * 255.f), int(c.g() * 255.f), int(c.b() * 255.f), 255));
            }
            // A tick where each band ends, so the ramp also says which filament is doing the work over any
            // part of it. Light on dark and dark on light, since the ramp is both.
            int at = 0;
            for (size_t bi = 0; bi + 1 < layer.chroma.bands.size(); ++bi) {
                at += layer.chroma.bands[bi].layers;
                if (at <= 0 || at >= count)
                    continue;
                const ColorRGB &c   = palette[size_t(first + at)];
                const float     lum = 0.2126f * c.r() + 0.7152f * c.g() + 0.0722f * c.b();
                const float     x   = std::round(p.x + w * float(at) / float(count));
                dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h),
                            lum > 0.5f ? IM_COL32(0, 0, 0, 140) : IM_COL32(255, 255, 255, 140));
            }
            dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Separator));
            if (ImGui::IsItemHovered())
                panel_tooltip(Slic3r::format(_u8L("Every color this stack can print, from the base at the left to "
                                                  "the top at the right.\n\n"
                                                  "%1% of its %2% picture layers change the color; the rest show "
                                                  "what the layer below already showed. Trim removes them."),
                                             distinct, relief),
                              { { "chroma_layers.gif" } });
            // No caption under it. What it says - how many colours the stack can really print - is
            // worth knowing once and is in the ramp's own tooltip above; printed under every picture
            // it was three lines of the card spent on a number that does not move while anything is
            // being adjusted.
        }

        // ---- One row per filament, and everything that filament has on it ----
        //
        // It used to be two rows and a drawer: a line of figures nobody could click, and behind a
        // chevron the transmission distance, the tool that measures it, Remove, and the band's own
        // place in the print order behind two buttons. Four things one click away each, to save a row
        // that did not need saving. They are all on the row now, and the order is the handle: the list
        // is the stack, so dragging a row along it is the one gesture that already means what it says.
        {
            int remove    = -1;
            int drag_from = -1, drag_to = -1;
            for (size_t bi = 0; bi < layer.chroma.bands.size(); ++bi) {
                ChromaBand &band = layer.chroma.bands[bi];
                ImGui::PushID(int(bi));
                const float swatch_px = std::round(frame_h * 0.8f);
                const float handle_w  = std::round(frame_h * 0.7f);

                // ---- the handle ----
                //
                // The row's place in the list is the band's place in the stack, so the gesture that
                // changes it is dragging the row. One step at a time, with the drag delta reset at each
                // swap: a slow drag walks the band along and a fast one cannot overshoot the list.
                {
                    const ImVec2 hp = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##hf_drag", ImVec2(handle_w, frame_h));
                    if (ImGui::IsItemActive()) {
                        const float step = frame_h + style.ItemSpacing.y;
                        const float dy   = ImGui::GetMouseDragDelta(0).y;
                        if (dy > 0.5f * step && bi + 1 < layer.chroma.bands.size()) {
                            drag_from = int(bi);
                            drag_to   = int(bi) + 1;
                            ImGui::ResetMouseDragDelta();
                        } else if (dy < -0.5f * step && bi > 0) {
                            drag_from = int(bi);
                            drag_to   = int(bi) - 1;
                            ImGui::ResetMouseDragDelta();
                        }
                    }
                    const bool  hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
                    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, hot ? 0.9f : 0.35f);
                    const float r   = std::max(1.f, std::round(frame_h * 0.055f));
                    const float cx = hp.x + 0.5f * handle_w, cy = hp.y + 0.5f * frame_h;
                    for (int ry = -1; ry <= 1; ++ry)
                        for (int rx = 0; rx < 2; ++rx)
                            dl->AddCircleFilled(ImVec2(cx + (rx == 0 ? -1.7f : 1.7f) * r, cy + float(ry) * 3.2f * r),
                                                r, col);
                    if (ImGui::IsItemHovered())
                        panel_tooltip(_u8L("Drag to move this filament along the stack. The order is the print order: "
                                           "the first row is printed first, against the backing, and the last is the "
                                           "one the light meets first."));
                    ImGui::SameLine(0.f, gap_s);
                }

                // The one thing about a stack that can go wrong while nobody is touching the card: the
                // project's filament in this slot is no longer the one the band was solved against. Said
                // on the swatch, because the colour is what is wrong, and clicking it is the only thing
                // that can be done about it from here.
                const ChromaBandDrift drift = chroma_band_drift(band.filament, loaded);
                const ImVec2          at    = ImGui::GetCursorPos();
                if (drift.any() && ImGui::InvisibleButton("##hf_drift", ImVec2(swatch_px, frame_h)) && !drift.missing) {
                    band.filament = loaded[size_t(band.filament.filament)];
                    chroma_touch(layer.slot);
                }
                if (drift.any()) {
                    if (ImGui::IsItemHovered()) {
                        const int   slot_no = band.filament.filament + 1;
                        std::string said;
                        if (drift.missing)
                            said = Slic3r::format(_u8L("This band prints in filament %1%, which this project does not "
                                                       "have loaded. Pick one it does have: a picture cannot be solved "
                                                       "against a filament that is not there."),
                                                  slot_no);
                        else if (drift.name)
                            said = Slic3r::format(_u8L("Filament %1% is now %2%, not the %3% this band was solved "
                                                       "against. Click to take what the project has now and solve the "
                                                       "picture again."),
                                                  slot_no, loaded[size_t(band.filament.filament)].name,
                                                  band.filament.name);
                        else if (drift.color)
                            said = Slic3r::format(_u8L("Filament %1% has changed color since this band was solved. Click "
                                                       "to take the color the project has now and solve the picture "
                                                       "again."),
                                                  slot_no);
                        else
                            said = Slic3r::format(_u8L("Filament %1% now has a transmission distance of %2$.1f, not the "
                                                       "%3$.1f this band was solved against. Click to take it and solve "
                                                       "the picture again."),
                                                  slot_no, loaded[size_t(band.filament.filament)].td_mm,
                                                  band.filament.td_mm);
                        panel_tooltip(said);
                    }
                    ImGui::SetCursorPos(at);
                }
                swatch(band.filament.color, swatch_px);
                {
                    // The band's number inside its own swatch rather than beside it: the position in the
                    // list already says the order, so a column of digits was a column of nothing.
                    const ImVec2 mn  = ImGui::GetItemRectMin();
                    const float  top = std::round(mn.y + 0.5f * (frame_h - swatch_px));
                    const ColorRGB &c = band.filament.color;
                    const float lum   = 0.2126f * c.r() + 0.7152f * c.g() + 0.0722f * c.b();
                    const std::string no = std::to_string(bi + 1);
                    const ImVec2      ts = ImGui::CalcTextSize(no.c_str());
                    dl->AddText(ImVec2(mn.x + 0.5f * (swatch_px - ts.x), top + 0.5f * (swatch_px - ts.y)),
                                lum > 0.5f ? IM_COL32(20, 20, 20, 230) : IM_COL32(240, 240, 240, 230), no.c_str());
                    if (drift.any()) {
                        const float r = std::max(2.5f, std::round(swatch_px * 0.2f));
                        dl->AddCircleFilled(ImVec2(mn.x + swatch_px - r, top + r), r, ImGui::GetColorU32(col_warn));
                    }
                }
                ImGui::SameLine(0.f, gap_s);
                // ---- the filament ----
                const std::string current = band.filament.name.empty() ?
                                                Slic3r::format(_u8L("Filament %1%"), band.filament.filament + 1) :
                                                Slic3r::format("%1%  %2%", band.filament.filament + 1, band.filament.name);
                const float layers_w  = m_imgui->scaled(3.9f);
                const float td_w      = m_imgui->scaled(3.3f);
                const float icon_w    = std::round(frame_h * 0.85f);
                const float tail      = layers_w + td_w + 2.f * icon_w + 4.f * gap_s + std::max(card_pad, 1.f);
                ImGui::SetNextItemWidth(-tail);
                push_panel_combo_style(m_parent.get_scale());
                if (ImGui::BeginCombo("##hf_filament", current.c_str())) {
                    for (const ChromaFilament &f : loaded) {
                        const std::string item = f.name.empty() ? Slic3r::format(_u8L("Filament %1%"), f.filament + 1) :
                                                                  Slic3r::format("%1%  %2%", f.filament + 1, f.name);
                        if (ImGui::Selectable(item.c_str(), f.filament == band.filament.filament)) {
                            band.filament = f;
                            chroma_touch(layer.slot);
                        }
                    }
                    ImGui::EndCombo();
                }
                pop_panel_combo_style();
                hover_tip(_u8L("Which of the loaded filaments this band prints in. Its color and its transmission distance come "
                         "from that filament's preset."));

                // ---- how many layers of it ----
                //
                // The unit lives in the field's own format, which is the only place a reader is looking
                // when they want to know what the number counts.
                ImGui::SameLine(0.f, gap_s);
                ImGui::SetNextItemWidth(layers_w);
                if (ImGui::InputScalar("##hf_layers", ImGuiDataType_S32, &band.layers, nullptr, nullptr,
                                       _u8L("%d layers").c_str())) {
                    band.layers = std::clamp(band.layers, 1, CHROMA_MAX_RELIEF_LAYERS);
                    chroma_touch(layer.slot);
                }
                hover_tip(_u8L("How many printed layers this filament gets, counting up the stack."));

                // ---- how deep light gets into it ----
                ImGui::SameLine(0.f, gap_s);
                ImGui::SetNextItemWidth(td_w);
                float td = band.filament.td_mm;
                if (ImGui::InputFloat("##hf_td", &td, 0.f, 0.f, "%.1f")) {
                    const float v = std::clamp(td, 0.05f, 100.f);
                    for (ChromaBand &b : layer.chroma.bands)
                        if (b.filament.filament == band.filament.filament)
                            b.filament.td_mm = v;
                    chroma_touch(layer.slot);
                }
                {
                    // The reading of the number, where the number is. It was a line of its own under
                    // every band, which is what made the list two storeys tall.
                    const float cover = chroma_layers_to_opaque(band.filament.td_mm, layer.chroma.layer_height_mm);
                    std::string said  = Slic3r::format(_u8L("Transmission distance, in millimetres: how deep light gets "
                                                            "into this filament before it is spent. Every color on this "
                                                            "card is computed from it.\n\n"
                                                            "At %1$.1f mm it covers what is under it in %2$.0f layers."),
                                                       band.filament.td_mm, cover);
                    if (bi == 0)
                        said += " " + _u8L("This is also the filament the backing prints in.");
                    if (!band.filament.td_source.empty())
                        said += "\n\n" + Slic3r::format(_u8L("Read from a table of measured filaments, matched to %1%. "
                                                              "A spool you measured yourself beats any table."),
                                                         band.filament.td_source);
                    if (drift.any())
                        said += "\n\n" + _u8L("This is not the filament it was solved with. Click the swatch to take "
                                               "the one the project has now.");
                    hover_tip(said, { { "chroma_td.gif" } });
                }

                // ---- measure it, rather than guess it ----
                //
                // No filament preset ships the number and nothing but a print can tell you it, so the
                // tool that prints the staircase is on the row beside the field it fills in. The icon
                // is that staircase.
                ImGui::SameLine(0.f, gap_s);
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##hf_measure", ImVec2(icon_w, frame_h))) {
                        m_chroma_wedge_step = 0;
                        if (m_chroma_wedge_substrate < 0 || m_chroma_wedge_substrate == band.filament.filament) {
                            // The darkest other filament shows through a pale staircase best.
                            float darkest = 2.f;
                            for (const ChromaFilament &f : loaded)
                                if (f.filament != band.filament.filament) {
                                    const float lum = 0.2126f * f.color.r() + 0.7152f * f.color.g() +
                                                      0.0722f * f.color.b();
                                    if (lum < darkest) {
                                        darkest                  = lum;
                                        m_chroma_wedge_substrate = f.filament;
                                    }
                                }
                        }
                        ImGui::OpenPopup("hf_measure_pop");
                    }
                    const ImU32 col  = ImGui::GetColorU32(ImGuiCol_Text, ImGui::IsItemHovered() ? 1.f : 0.6f);
                    const float step = std::round(icon_w * 0.26f);
                    const float x0   = p.x + 0.5f * (icon_w - 3.f * step);
                    const float y1   = p.y + 0.5f * frame_h + 1.5f * step;
                    for (int k = 0; k < 3; ++k)
                        dl->AddRectFilled(ImVec2(x0 + float(k) * step, y1 - float(k + 1) * step),
                                          ImVec2(x0 + float(k + 1) * step - 1.f, y1), col);
                    if (ImGui::IsItemHovered())
                        panel_tooltip(_u8L("Measure this filament's transmission distance by printing a step wedge: a "
                                           "staircase of it over a filament it has to hide. Nothing but a print can tell "
                                           "you this number, and every color on this card is computed from it."),
                                      { { "chroma_td.gif" } });
                }
        if (PanelPopup popup{"hf_measure_pop"}) {
                        ImGui::TextDisabled("%s", _u8L("Measure transmission distance").c_str());
                        ImGui::Dummy(ImVec2(1.f, gap_s));

                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(_u8L("Over").c_str());
                        ImGui::SameLine(0.f, gap_s);
                        const ChromaFilament *sub = nullptr;
                        for (const ChromaFilament &f : loaded)
                            if (f.filament == m_chroma_wedge_substrate)
                                sub = &f;
                        ImGui::SetNextItemWidth(m_imgui->scaled(8.f));
                        push_panel_combo_style(m_parent.get_scale());
                        if (ImGui::BeginCombo("##hf_sub", sub == nullptr ? _u8L("pick one").c_str() :
                                                                           (sub->name.empty() ?
                                                                                Slic3r::format(_u8L("Filament %1%"), sub->filament + 1) :
                                                                                Slic3r::format("%1%  %2%", sub->filament + 1, sub->name))
                                                                               .c_str())) {
                            for (const ChromaFilament &f : loaded) {
                                if (f.filament == band.filament.filament)
                                    continue;
                                const std::string item = f.name.empty() ? Slic3r::format(_u8L("Filament %1%"), f.filament + 1) :
                                                                          Slic3r::format("%1%  %2%", f.filament + 1, f.name);
                                if (ImGui::Selectable(item.c_str(), f.filament == m_chroma_wedge_substrate))
                                    m_chroma_wedge_substrate = f.filament;
                            }
                            ImGui::EndCombo();
                        }
                        pop_panel_combo_style();
                        if (ImGui::IsItemHovered())
                            panel_tooltip(_u8L("The filament the staircase is printed over. A dark one under a pale staircase "
                                               "is the easiest to see stop showing through."));

                        ImGui::SameLine(0.f, 2.f * gap_s);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(_u8L("Steps").c_str());
                        ImGui::SameLine(0.f, gap_s);
                        ImGui::SetNextItemWidth(m_imgui->scaled(3.4f));
                        if (ImGui::InputInt("##hf_steps", &m_chroma_wedge_steps, 0, 0))
                            m_chroma_wedge_steps = std::clamp(m_chroma_wedge_steps, 4, 60);
                        if (ImGui::IsItemHovered())
                            panel_tooltip(_u8L("How many thicknesses the staircase offers, one layer apart. Sixteen covers "
                                               "every filament that is any use for this at a fine layer height."));

                        m_imgui->disabled_begin(m_chroma_wedge_substrate < 0);
                        if (m_imgui->button(_L("Create test plate"))) {
                            ChromaWedge wedge;
                            wedge.steps           = m_chroma_wedge_steps;
                            wedge.layer_height_mm = layer.chroma.layer_height_mm;
                            // After this frame, not during it. The test plate replaces the project, which frees
                            // the volume that `layer` and `band` point into, and the rest of the panel then
                            // draws from freed memory.
                            const int substrate = m_chroma_wedge_substrate;
                            const int test      = band.filament.filament;
                            ImGui::CloseCurrentPopup();
                            wxGetApp().CallAfter([wedge, substrate, test]() {
                                if (Plater *plater = wxGetApp().plater(); plater != nullptr)
                                    plater->calib_transmission_distance(wedge, substrate, test);
                            });
                        }
                        m_imgui->disabled_end();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            panel_tooltip(_u8L("Replaces what is on the plate with the wedge, ready to slice. Print it, then "
                                               "come back here with the step you read off it."));

                        ImGui::Dummy(ImVec2(1.f, gap_s));
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(_u8L("Step").c_str());
                        ImGui::SameLine(0.f, gap_s);
                        ImGui::SetNextItemWidth(m_imgui->scaled(3.4f));
                        if (ImGui::InputInt("##hf_read", &m_chroma_wedge_step, 0, 0))
                            m_chroma_wedge_step = std::clamp(m_chroma_wedge_step, 0, 200);
                        if (ImGui::IsItemHovered())
                            panel_tooltip(_u8L("The first step at which the filament underneath stopped showing through. "
                                               "Every fifth step is widened so it can be counted in fives."));
                        const float measured_td = chroma_td_from_step(m_chroma_wedge_step, layer.chroma.layer_height_mm);
                        ImGui::SameLine(0.f, 2.f * gap_s);
                        ImGui::AlignTextToFramePadding();
                        if (measured_td > 0.f)
                            ImGui::Text("%s", Slic3r::format(_u8L("TD %1$.1f"), measured_td).c_str());
                        else
                            ImGui::TextDisabled("%s", _u8L("TD -").c_str());

                        ImGui::SameLine(0.f, 2.f * gap_s);
                        m_imgui->disabled_begin(measured_td <= 0.f);
                        if (m_imgui->button(_L("Use it"))) {
                            for (ChromaBand &b : layer.chroma.bands)
                                if (b.filament.filament == band.filament.filament)
                                    b.filament.td_mm = measured_td;
                            chroma_touch(layer.slot);
                            ImGui::CloseCurrentPopup();
                        }
                        m_imgui->disabled_end();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            panel_tooltip(_u8L("Puts the measurement into this stack. Put it into the filament's own preset "
                                               "too, under Transmission distance, so every later picture starts from it."));

                        // The other way to get a transmission distance: somebody else measured it. A library is a
                        // file of filaments with a number each, and the people who do this have large ones; it
                        // belongs here because this is where the card talks about where the number comes from.
                        ImGui::Dummy(ImVec2(1.f, gap_s));
                        ImGui::Separator();
                        ImGui::Dummy(ImVec2(1.f, gap_s));
                        ImGui::TextDisabled("%s", _u8L("Or use someone else's measurements").c_str());
                        ImGui::Dummy(ImVec2(1.f, gap_s));
                        if (m_imgui->button(_L("Import a library"))) {
                            wxFileDialog pick(nullptr, _L("Choose a filament library to import"), wxEmptyString, wxEmptyString,
                                              _L("Filament libraries (*.json;*.csv;*.tsv)|*.json;*.csv;*.tsv"),
                                              wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                            if (pick.ShowModal() == wxID_OK) {
                                const FilamentTdImport result = filament_td_import(into_u8(pick.GetPath()));
                                // Without this the import would only take effect on the next run: the table is
                                // read once and cached for the process.
                                filament_td_reset_cache();

                                std::string said;
                                if (result.unreadable)
                                    said = _u8L("That file could not be read as a filament library. A library is a JSON or "
                                                "CSV file listing filaments with a transmission distance for each of them.");
                                else {
                                    said = Slic3r::format(into_u8(_L_PLURAL("Took %1% measurement.", "Took %1% measurements.",
                                                                            int(result.taken))),
                                                          int(result.taken));
                                    if (result.skipped() > 0)
                                        said += "\n\n" +
                                                Slic3r::format(into_u8(_L_PLURAL("%1% row had nothing usable in it and was "
                                                                                 "skipped.",
                                                                                 "%1% rows had nothing usable in them and "
                                                                                 "were skipped.",
                                                                                 int(result.skipped()))),
                                                               int(result.skipped()));
                                    if (result.taken > 0)
                                        said += "\n\n" + _u8L("A filament takes its transmission distance up when you choose "
                                                              "it for a band. For a band you have already chosen, the mark on "
                                                              "its color swatch is how you take the new number.");
                                }
                                MessageDialog(nullptr, from_u8(said), _L("Import filament library"),
                                              wxOK | wxICON_INFORMATION)
                                    .ShowModal();
                                m_parent.set_as_dirty();
                            }
                        }
                        if (ImGui::IsItemHovered())
                            panel_tooltip(_u8L("Reads a file of filaments with a transmission distance each, and keeps its "
                                               "measurements beside the ones that ship with OrcaSlicer. A number you set on a "
                                               "preset yourself still beats both, and a number measured for one color is "
                                               "never used for another."));
                    }

                // ---- take it out of the stack ----
                ImGui::SameLine(0.f, gap_s);
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    m_imgui->disabled_begin(layer.chroma.bands.size() <= 1);
                    if (ImGui::InvisibleButton("##hf_remove", ImVec2(icon_w, frame_h)) &&
                        layer.chroma.bands.size() > 1)
                        remove = int(bi);
                    m_imgui->disabled_end();
                    const bool  hot = ImGui::IsItemHovered() && layer.chroma.bands.size() > 1;
                    const ImU32 col = hot ? IM_COL32(224, 89, 75, 255) :
                                            ImGui::GetColorU32(ImGuiCol_Text, layer.chroma.bands.size() > 1 ? 0.55f : 0.25f);
                    const float a   = std::round(icon_w * 0.26f);
                    const float cx = p.x + 0.5f * icon_w, cy = p.y + 0.5f * frame_h;
                    dl->AddLine(ImVec2(cx - a, cy - a), ImVec2(cx + a, cy + a), col, 1.6f);
                    dl->AddLine(ImVec2(cx - a, cy + a), ImVec2(cx + a, cy - a), col, 1.6f);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        panel_tooltip(layer.chroma.bands.size() > 1 ?
                                          _u8L("Takes this filament out of the stack.") :
                                          _u8L("A stack prints in at least one filament."));
                }

                ImGui::PopID();
            }

            if (drag_from >= 0 && drag_to >= 0 && drag_to < int(layer.chroma.bands.size())) {
                std::swap(layer.chroma.bands[size_t(drag_from)], layer.chroma.bands[size_t(drag_to)]);
                chroma_touch(layer.slot);
            }
            if (remove >= 0) {
                layer.chroma.bands.erase(layer.chroma.bands.begin() + remove);
                chroma_touch(layer.slot);
            }
        }

        // ---- Choosing the whole stack again, rather than one band of it ----
        {
            ImGui::Dummy(ImVec2(1.f, gap_s));
            if (m_chroma_pick_count[slot] <= 0)
                m_chroma_pick_count[slot] = std::max(2, int(layer.chroma.bands.size()));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("How many").c_str());
            ImGui::SameLine(0.f, gap_s);
            // Five choices and one click, instead of a slider nobody drags to pick four.
            std::vector<int> counts;
            for (int n : { 2, 3, 4, 6, 8 })
                if (n <= int(CHROMA_MAX_BANDS))
                    counts.push_back(n);
            segmented("##hf_count", counts, m_chroma_pick_count[slot], m_imgui->scaled(7.f));
            hover_tip(_u8L("How many filaments the picture is printed in. Fewer is fewer changes to stand over; more is a "
                     "better match, never a worse one."),
                { { "chroma_colors.gif" } });

            // Their own row, each half of it. On the row above they shared it with the count, and the
            // second of them was drawn past the right edge of the panel: a button nobody can reach.
            const float half = std::max(m_imgui->scaled(4.f),
                                        0.5f * (ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f) - gap_s));
            if (ImGui::Button(_u8L("Pick from mine").c_str(), ImVec2(half, frame_h)))
                chroma_pick_colors(layer, m_chroma_pick_count[slot]);
            hover_tip(_u8L("Builds the stack from that many of your loaded filaments: the ones that come closest to this "
                     "picture, in the order that reaches the most colors."),
                { { "chroma_colors.gif" }, { "chroma_order.gif" } });

            ImGui::SameLine(0.f, gap_s);
            if (ImGui::Button(_u8L("Suggest colors").c_str(), ImVec2(half, frame_h))) {
                chroma_suggest(layer, m_chroma_pick_count[slot]);
                ImGui::OpenPopup("hf_suggest");
            }
            hover_tip(_u8L("Works out the colors this picture would most like to be printed in, whether or not you own "
                     "them - so you know what to go and get.\n\n"
                     "A transmission distance matters as much as the color: the same red at TD 1 and at TD 6 are "
                     "different filaments for this. Click a color in the list to put your nearest into that band."),
                { { "chroma_colors.gif" } });

            // Beside the panel rather than over it, and at a width of its own. Over it, the list
            // covered the stack it is about: the use of a suggestion is reading it against the bands
            // underneath, and a window that hides them makes the reader close it to look.
            {
                const float  sug_w = m_imgui->scaled(21.5f);
                const ImVec2 panel = ImGui::GetWindowPos();
                ImGui::SetNextWindowPos(ImVec2(std::max(0.f, panel.x - sug_w - m_imgui->scaled(0.5f)),
                                               ImGui::GetCursorScreenPos().y),
                                        ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(sug_w, 0.f));
            }
            if (PanelPopup popup{"hf_suggest"}) {
                const std::vector<ChromaSuggestion> &found = m_chroma_suggestion[slot];
                if (found.empty())
                    ImGui::TextDisabled("%s", _u8L("Nothing to suggest for this picture.").c_str());
                else {
                    // Hoisted out of the row loop: every entry needs it, and it reads the filament presets.
                    const std::vector<ChromaFilament> owned = chroma_filaments();
                    size_t                            reachable = 0;
                    for (const ChromaSuggestion &one : found)
                        if (one.nearest >= 0 && size_t(one.nearest) < owned.size())
                            ++reachable;

                    // ---- one row per colour, in four columns that line up ----
                    //
                    // It was a row of text running to whatever width the longest entry wanted, which is
                    // what made the window wider than the canvas. Every column has a width now, the row
                    // itself is the control rather than the swatch alone, and what you own sits at the
                    // right where it can be compared down the list instead of read sentence by sentence.
                    const float sug_px  = std::round(frame_h * 0.8f);
                    const float hex_w   = m_imgui->scaled(4.6f);
                    const float td_w    = m_imgui->scaled(3.2f);
                    const float lay_w   = m_imgui->scaled(4.0f);
                    const float mine_w  = m_imgui->scaled(4.4f);

                    ImGui::TextDisabled("%s", _u8L("Colors to look for, bottom of the stack first").c_str());
                    {
                        // The heading of the column on the right, where it is needed: the swatch under it
                        // is a different filament from the one the row is about.
                        const float x = ImGui::GetCursorPosX();
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                                      ImGui::GetContentRegionMax().x - mine_w - sug_px - gap_s));
                        ImGui::TextDisabled("%s", _u8L("yours").c_str());
                        ImGui::SetCursorPosX(x);
                    }
                    ImGui::Dummy(ImVec2(1.f, gap_s));
                    for (size_t i = 0; i < found.size(); ++i) {
                        const ChromaSuggestion &one = found[i];
                        ImGui::PushID(int(i));
                        const bool   can_use = one.nearest >= 0 && size_t(one.nearest) < owned.size();
                        const ImVec2 at      = ImGui::GetCursorPos();
                        // The whole row, so the thing that is clicked is the thing that is read. A
                        // suggestion with nothing of yours near it is still listed - it is what the
                        // list is for - but there is nothing to apply, so it does not highlight.
                        if (can_use) {
                            if (ImGui::Selectable("##sg_use", false, 0,
                                                  ImVec2(ImGui::GetContentRegionAvail().x, frame_h))) {
                                ChromaBand band;
                                band.filament = owned[size_t(one.nearest)];
                                band.layers   = std::clamp(one.layers, 1, CHROMA_MAX_RELIEF_LAYERS);
                                if (i < layer.chroma.bands.size())
                                    layer.chroma.bands[i] = band;
                                else if (layer.chroma.bands.size() < CHROMA_MAX_BANDS)
                                    layer.chroma.bands.push_back(band);
                                chroma_touch(layer.slot);
                            }
                            if (ImGui::IsItemHovered())
                                panel_tooltip(Slic3r::format(_u8L("Puts your filament %1% into band %2% of the stack, "
                                                                  "with the %3% layers this suggestion asks for. Its "
                                                                  "own color and transmission distance are used, not "
                                                                  "the suggested ones, so what the card then predicts "
                                                                  "is what you can actually print."),
                                                             one.nearest + 1, i + 1, one.layers));
                            ImGui::SetCursorPos(at);
                        }
                        swatch(one.color, sug_px);
                        ImGui::SameLine(0.f, gap_s);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%s", Slic3r::format("#%1$02X%2$02X%3$02X", int(one.color.r() * 255.f + 0.5f),
                                                         int(one.color.g() * 255.f + 0.5f),
                                                         int(one.color.b() * 255.f + 0.5f))
                                              .c_str());
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(at.x + sug_px + gap_s + hex_w);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", Slic3r::format(_u8L("TD %1$.0f"), one.td_mm).c_str());
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(at.x + sug_px + gap_s + hex_w + td_w);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", Slic3r::format(into_u8(_L_PLURAL("%1% layer", "%1% layers",
                                                                                   one.layers)),
                                                                 one.layers)
                                                      .c_str());

                        // What you own that is nearest it, as a colour beside a number: two swatches a
                        // few pixels apart is the comparison this list exists to offer.
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                                      ImGui::GetContentRegionMax().x - mine_w - sug_px - gap_s));
                        if (!can_use) {
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("%s", _u8L("none loaded").c_str());
                        } else {
                            swatch(owned[size_t(one.nearest)].color, sug_px);
                            ImGui::SameLine(0.f, gap_s);
                            ImGui::AlignTextToFramePadding();
                            const std::string near_text = Slic3r::format(_u8L("%1% (%2$.0f off)"), one.nearest + 1,
                                                                         one.nearest_de);
                            if (one.nearest_de > 10.f)
                                m_imgui->warning_text(near_text.c_str());
                            else
                                ImGui::TextDisabled("%s", near_text.c_str());
                        }
                        ImGui::PopID();
                    }
                    ImGui::Dummy(ImVec2(1.f, gap_s));
                    // Two ways of taking the list, and they answer different questions. One builds the
                    // stack you can print today out of the nearest things you own. The other builds the
                    // stack the picture asked for, colour and transmission distance both, so the card
                    // predicts what those filaments would do if you had them - which is the only way to
                    // find out whether they are worth buying before buying them.
                    const float sug_half = std::max(m_imgui->scaled(4.f),
                                                    0.5f * (ImGui::GetContentRegionAvail().x - gap_s));
                    if (ImGui::Button(_u8L("Use suggested").c_str(), ImVec2(sug_half, frame_h))) {
                        std::vector<ChromaBand> built;
                        for (const ChromaSuggestion &one : found) {
                            ChromaBand band;
                            // The slot stays a slot the printer has: a band has to be printed by
                            // something. What is taken from the suggestion is the colour and the depth
                            // light gets into it, which is all the prediction reads.
                            band.filament = (one.nearest >= 0 && size_t(one.nearest) < owned.size()) ?
                                                owned[size_t(one.nearest)] :
                                                (layer.chroma.bands.empty() ? ChromaFilament{} :
                                                                              layer.chroma.bands.front().filament);
                            band.filament.color     = one.color;
                            band.filament.td_mm     = one.td_mm;
                            band.filament.td_source = _u8L("suggested");
                            band.layers             = std::clamp(one.layers, 1, CHROMA_MAX_RELIEF_LAYERS);
                            built.push_back(band);
                            if (built.size() >= CHROMA_MAX_BANDS)
                                break;
                        }
                        if (!built.empty()) {
                            layer.chroma.bands = built;
                            chroma_touch(layer.slot);
                        }
                    }
                    if (ImGui::IsItemHovered())
                        panel_tooltip(_u8L("Builds the stack out of these colors themselves, with the transmission "
                                           "distance each of them asks for, whether or not you own anything like "
                                           "them. The card then predicts the print you would get if you did - which "
                                           "is how to find out whether a spool is worth buying before buying it.\n\n"
                                           "The filament slots stay the ones your printer has, because a band has to "
                                           "be printed by something."));

                    ImGui::SameLine(0.f, gap_s);
                    m_imgui->disabled_begin(reachable == 0);
                    if (ImGui::Button(_u8L("Use my closest").c_str(), ImVec2(sug_half, frame_h)))
                        chroma_apply_suggestions(layer);
                    m_imgui->disabled_end();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        panel_tooltip(reachable == 0 ?
                                          _u8L("None of these colors is anything you have loaded, so there is no stack to "
                                               "build from them yet. Load something closer, or take the list shopping.") :
                                          _u8L("Builds the stack from the filament of yours nearest each suggestion, in "
                                               "this order and with these layer counts. It is the printable version of "
                                               "this list, and the card's prediction updates to match it."));

                }
            }
        }

        // ---- The two edits to the stack as a whole ----
        if (has_ramp) {
            const float half = std::max(m_imgui->scaled(4.f),
                                        0.5f * (ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f) - gap_s));
            m_imgui->disabled_begin(distinct >= relief);
            if (ImGui::Button(_u8L("Trim wasted layers").c_str(), ImVec2(half, frame_h))) {
                chroma_trim_bands(layer.chroma);
                chroma_touch(layer.slot);
            }
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                panel_tooltip(Slic3r::format(_u8L("Shortens each band to its last layer that still changes the "
                                                  "color. %1% of %2% picture layers do."),
                                             distinct, relief),
                              { { "chroma_trim.png" } });
            ImGui::SameLine(0.f, gap_s);
            m_imgui->disabled_begin(loaded.empty() || layer.chroma.bands.size() >= CHROMA_MAX_BANDS);
            if (ImGui::Button(_u8L("Add a filament").c_str(), ImVec2(half, frame_h)))
                ImGui::OpenPopup("hf_add");
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                panel_tooltip(_u8L("Adds another filament change on top of the stack."), { { "chroma_order.gif" } });
    if (PanelPopup popup{"hf_add"}) {
                // The whole row is the item, with the swatch and the name drawn over it rather than
                // beside it: an item sized to its own label highlights behind the word only, which reads
                // as nothing happening rather than as the row being picked. An empty label of width zero
                // is the way to say that - it carries no size of its own and fills the row.
                for (const ChromaFilament &f : loaded) {
                    ImGui::PushID(f.filament);
                    const bool picked = ImGui::Selectable("", false, 0, ImVec2(0.f, frame_h));
                    ImGui::SameLine(0.f, 0.f);
                    swatch(f.color, std::round(frame_h * 0.7f));
                    ImGui::SameLine(0.f, gap_s);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted((f.name.empty() ? Slic3r::format(_u8L("Filament %1%"), f.filament + 1) :
                                                             Slic3r::format("%1%  %2%", f.filament + 1, f.name))
                                               .c_str());
                    if (picked) {
                        ChromaBand band;
                        band.filament = f;
                        band.layers   = 4;
                        layer.chroma.bands.push_back(band);
                        chroma_touch(layer.slot);
                    }
                    ImGui::PopID();
                }
            }
        }
    }

    // ---- The plate that comes out of the printer ----
    //
    // Every row in here is a dimension of the physical thing, which is why which way up it prints is
    // among them and why the frame's own two numbers are not: those belong beside the drawing of the
    // moulding, in the gallery where a frame is chosen by looking at it.
    ImGui::Dummy(ImVec2(1.f, gap_s));
    if (layer.chroma.plate.generated) {
        const ChromaPlate &plate = layer.chroma.plate;
        const std::string  size  = Slic3r::format(_u8L("%1$.0f x %2$.0f x %3$.1f mm"), plate.width_mm, plate.height_mm,
                                                  plate.base_mm + layer.chroma.depth_mm());
        if (closable("##hf_sec_plate", _L("The plate"), size, m_chroma_open_plate[slot])) {
            ImGui::Dummy(ImVec2(1.f, gap_s));
            render_chroma_plate(layer, card_pad, has_ramp);
        }
    } else {
        // Not a section. For a picture painted onto somebody else's model the whole of it is one row -
        // how much of the model is under the picture - and a fold that hides one row is a click that
        // buys nothing and a heading that says nothing.
        render_chroma_plate(layer, card_pad, has_ramp);
    }

    // ---- Chosen once, and then not again ----
    ImGui::Dummy(ImVec2(1.f, gap_s));
    {
        const std::string lit = layer.chroma.lighting == ChromaLighting::BackLit ? _u8L("lit from behind") :
                                layer.chroma.lighting == ChromaLighting::BothLit ?
                                                                                   _u8L("lit from both sides") :
                                                                                   _u8L("lit from the front");
        if (closable("##hf_sec_light", _L("Light and detail"), lit, m_chroma_open_light[slot])) {
            ImGui::Dummy(ImVec2(1.f, gap_s));
            render_chroma_light(layer);
        }
    }

    // ---- How it is drawn, as opposed to what is printed ----
    //
    // Nothing in here goes into the project or changes the mesh. The summary names whichever overlay is
    // on, because that is the one thing about this section worth knowing while it is folded: an overlay
    // left on is the usual reason a model looks wrong.
    ImGui::Dummy(ImVec2(1.f, gap_s));
    {
        const std::string on = m_show_chroma_error  ? _u8L("the error map") :
                               m_show_chroma_layers ? _u8L("printed layers") :
                               m_show_chroma_colors ? _u8L("printed colors") :
                               m_chroma_debug != 0  ? _u8L("a debug view") :
                                                      _u8L("none on");
        if (closable("##hf_sec_views", _L("Views"), on, m_chroma_open_views[slot])) {
            ImGui::Dummy(ImVec2(1.f, gap_s));
            render_chroma_views(layer);
        }
    }
}

// The plate's own dimensions. Every row is a measurement of the thing that comes off the bed, which is
// why the base it measures for itself and which way up it prints are both here: a base is layers of
// plastic under the picture, and which way up is a fact about how the slab is laid on the bed.
void GLGizmoTextureDisplacement::render_chroma_plate(TextureDisplacementLayer &layer, float card_pad,
                                                     bool has_ramp)
{
    const float frame_h = ImGui::GetFrameHeight();
    const float gap_s   = std::round(m_imgui->scaled(0.4f));
    ImDrawList *dl      = ImGui::GetWindowDrawList();

    // One width for every field in the section. They start at one x already; starting at one x and
    // ending at four different ones is what made the column look like no column at all.
    const float field_w = m_imgui->scaled(4.6f);

    // What is under the picture, in layers. Two different questions wear the same name here, and the
    // one being asked depends on whose plate it is.
    //
    // A plate the picture made for itself knows its own backing: Solid backing, in millimetres, is a
    // dimension of that plate, and the stack's base has to be the same thickness counted in layers or
    // the swaps are written at heights the plate does not have. That is not a measurement and there is
    // nothing to measure it against, so the row is not offered at all - Solid backing below says it,
    // in the units a plate is described in.
    //
    // A picture painted onto somebody else's model has no such number: the base is everything of the
    // model below the painted face, which is a measurement, and the padlock is for the rare picture
    // that is not meant to start at that face.
    if (layer.chroma.plate.generated) {
        const double lh   = std::max(0.01, double(layer.chroma.layer_height_mm));
        const int    want = std::max(0, int(std::lround(double(layer.chroma.plate.base_mm) / lh)));
        if (layer.chroma.base_layers != want) {
            layer.chroma.base_layers = want;
            layer.chroma.base_auto   = false;
            chroma_touch(layer.slot);
        }
    } else {
        const int measured = chroma_measured_base(layer);
        if (layer.chroma.base_auto && measured > 0 && measured != layer.chroma.base_layers) {
            layer.chroma.base_layers = measured;
            chroma_touch(layer.slot);
        }
        slider_label(_L("Layers below it"));
        int base = layer.chroma.base_layers;
        m_imgui->disabled_begin(layer.chroma.base_auto);
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputInt("##hf_base", &base, 0, 0) && !layer.chroma.base_auto) {
            layer.chroma.base_layers = std::clamp(base, 0, 20000);
            chroma_touch(layer.slot);
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            panel_tooltip(Slic3r::format(_u8L("The solid backing under the picture: %1$.2f mm, in layers.\n\n"
                                              "It is a measurement of the model - everything below the painted face - "
                                              "and the colors are computed assuming no light gets through it. The "
                                              "padlock stops it following the model, for a picture that is not meant "
                                              "to start at the painted face."),
                                         double(layer.chroma.base_layers) * double(layer.chroma.layer_height_mm)));
        // Following the model or not, as a padlock rather than a pair of buttons and a sentence.
        ImGui::SameLine(0.f, gap_s);
        {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##hf_base_auto", ImVec2(frame_h, frame_h))) {
            layer.chroma.base_auto = !layer.chroma.base_auto;
            chroma_touch(layer.slot);
        }
        const bool  hot  = ImGui::IsItemHovered();
        const float w    = std::round(frame_h * 0.38f);
        const float hgt  = std::round(frame_h * 0.30f);
        const float cx   = p.x + 0.5f * frame_h;
        const float body = p.y + 0.5f * frame_h;
        ImVec4 tint = layer.chroma.base_auto ? ImGuiWrapper::COL_ORCA :
                                               ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        tint.w      = hot ? 1.f : 0.9f;
        const ImU32 col = ImGui::GetColorU32(tint);
        dl->AddRectFilled(ImVec2(cx - 0.5f * w, body), ImVec2(cx + 0.5f * w, body + hgt), col, 1.f);
        // The shackle: closed while it follows the model, open once it has been set by hand.
        const float r  = 0.32f * w;
        const float pi = 3.14159265f;
        dl->PathArcTo(ImVec2(layer.chroma.base_auto ? cx : cx + 0.22f * w, body - r * 0.2f), r, pi, pi * 2.f, 12);
        dl->PathStroke(col, 0, 1.4f);
        if (hot)
            panel_tooltip(layer.chroma.base_auto ?
                                 _u8L("The base follows the painted face. Click to set it by hand.") :
                                 _u8L("The base is set by hand. Click to make it follow the painted face again."));
        }
    }

    // ---- The plate, for a picture that made one for itself ----
    //
    // Only here: a picture painted onto a model has a plate already, and it is the model.
    if (layer.chroma.plate.generated) {
        ChromaPlate &plate = layer.chroma.plate;
        const size_t slot  = size_t(layer.slot);
        const auto   redraw = [&]() {
            // The frame's cross-section is resolved against the picture's size and the backing under
            // it, so any of these rows can invalidate it. Re-resolving costs a polyline and saves the
            // moulding quietly drifting out of proportion as the plate is resized.
            chroma_refresh_frame(layer);
            m_chroma_plate_dirty[slot] = true;
            chroma_touch(layer.slot);
        };

        // One number for the size, because the other one is the picture's to decide: a photograph
        // stretched to a shape that is not its own is not the photograph any more.
        const bool  wide    = plate.width_mm >= plate.height_mm;
        const float aspect  = wide ? plate.width_mm / std::max(0.1f, plate.height_mm) :
                                     plate.height_mm / std::max(0.1f, plate.width_mm);
        float       longest = std::max(plate.width_mm, plate.height_mm);
        slider_label(_L("Picture is"));
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputFloat("##hf_plate_size", &longest, 0.f, 0.f, "%.0f mm")) {
            longest = std::clamp(longest, 10.f, 1000.f);
            plate.width_mm  = wide ? longest : longest / aspect;
            plate.height_mm = wide ? longest / aspect : longest;
            redraw();
        }
        ImGui::SameLine(0.f, gap_s);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Slic3r::format(_u8L("wide, so %1$.0f x %2$.0f"), plate.width_mm,
                                                 plate.height_mm)
                                      .c_str());
        hover_tip(_u8L("The longer side of the picture on the plate. The other side follows the picture's own "
                 "proportions, and the frame is added outside both."));

        slider_label(_L("Solid backing"));
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputFloat("##hf_plate_base", &plate.base_mm, 0.f, 0.f, "%.1f mm")) {
            plate.base_mm = std::clamp(plate.base_mm, 0.2f, 20.f);
            redraw();
        }
        hover_tip(_u8L("How solid the plate is under the picture. The colors are computed assuming no light gets "
                 "through it, so this has to be thick enough to be opaque in the first filament - about a "
                 "tenth of its transmission distance."),
            { { "chroma_td.gif" } });

        // ---- The frame ----
        //
        // One mark at the end of this row, and it is the frame itself: the cross-section the plate is
        // wearing, drawn small. A frame is something seen, so the thing that opens the gallery is a
        // picture of what you already have rather than a word and a list of words - and the gallery is
        // where it is chosen anyway, because that is the only place the width, the rise, your own
        // mouldings and cutting the plate to an outline all are.
        ChromaFrame &frame = plate.frame;
        {
            const float  fw = std::round(frame_h * 1.5f);
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                          ImGui::GetContentRegionMax().x - std::max(card_pad, 1.f) - fw));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##hf_frame_pick", ImVec2(fw, frame_h))) {
                // Modal window, so not from inside the ImGui pass: see chroma_pick_picture().
                const int         slot_id = layer.slot;
                const std::string current = frame.frame_id;
                const float       width   = frame.width_mm;
                const float       rise    = frame.rise_mm;
                wxGetApp().CallAfter([this, slot_id, current, width, rise]() {
                    const ChromaFramePick pick = pick_chroma_frame(nullptr, current, width, rise);
                    if (pick.chosen)
                        chroma_set_frame(slot_id, pick);
                });
            }
            const bool  hot  = ImGui::IsItemHovered();
            const ImU32 line = ImGui::GetColorU32(hot ? ImGuiWrapper::COL_ORCA :
                                                        ImGui::GetStyleColorVec4(ImGuiCol_Text));
            const float pad  = std::round(frame_h * 0.18f);
            const float base_y = p0.y + frame_h - pad;
            dl->AddLine(ImVec2(p0.x, base_y), ImVec2(p0.x + fw, base_y), ImGui::GetColorU32(ImGuiCol_Separator));

            const std::vector<Vec2f> profile = frame.frame_id.empty() ? std::vector<Vec2f>() :
                                                                        chroma_frame_profile(frame, plate.base_mm);
            if (profile.size() >= 2) {
                float top = plate.base_mm;
                for (const Vec2f &v : profile)
                    top = std::max(top, v.y());
                const float span = std::max(1e-3f, top - plate.base_mm);
                // The picture's surface on the left and the plate's outer edge on the right, which is
                // the way the plate is looked at from above with the moulding running round it.
                const auto at = [&](const Vec2f &v) {
                    return ImVec2(p0.x + fw * (v.x() / std::max(1e-3f, frame.width_mm)),
                                  base_y - (frame_h - 2.f * pad) * (v.y() - plate.base_mm) / span);
                };
                for (size_t i = 0; i + 1 < profile.size(); ++i)
                    dl->AddLine(at(profile[i]), at(profile[i + 1]), line, std::max(1.4f, std::round(frame_h * 0.07f)));
            }
            if (hot) {
                const ChromaFrameDef *def = frame.frame_id.empty() ?
                                                nullptr :
                                                ChromaFrameLibrary::instance().find(frame.frame_id);
                std::string said;
                if (def != nullptr && frame.width_mm > 0.f)
                    said = Slic3r::format(_u8L("Frame: %1%, %2$.1f mm wide."), into_u8(def->name), frame.width_mm);
                else if (def != nullptr)
                    said = Slic3r::format(_u8L("Frame: %1%."), into_u8(def->name));
                else if (!frame.frame_id.empty())
                    // The file it came from is gone, but the plate still has the cross-section it
                    // resolved, so the frame itself is intact and only its name is missing.
                    said = Slic3r::format(_u8L("Frame: %1%, which this installation no longer has a file for."),
                                          frame.frame_id);
                else
                    said = _u8L("No frame: the picture runs to the edge of the plate.");
                panel_tooltip(said + "\n\n" +
                              _u8L("Click to choose one from the gallery, where its width and how far it stands "
                                   "above the picture are set too, along with adding your own and cutting the "
                                   "plate to an SVG outline."));
            }
        }

        // ---- Which way up it prints ----
        //
        // Face down is the one way to get a picture with no layer lines in it at all: the picture's own
        // face is printed against the build plate, so it comes out as smooth as that plate is.
        slider_label(_L("Which way up"));
        {
            const bool  down    = plate.orientation == ChromaOrientation::FaceDown;
            ImGui::SetNextItemWidth(m_imgui->scaled(7.f));
            push_panel_combo_style(m_parent.get_scale());
            if (ImGui::BeginCombo("##hf_orient", down ? _u8L("Picture down").c_str() : _u8L("Picture up").c_str())) {
                if (ImGui::Selectable(_u8L("Picture up").c_str(), !down) && down) {
                    plate.orientation = ChromaOrientation::FaceUp;
                    layer.chroma.base_auto = true;
                    redraw();
                }
                if (ImGui::Selectable(_u8L("Picture down").c_str(), down) && !down) {
                    plate.orientation = ChromaOrientation::FaceDown;
                    // The picture is at the bottom now, so there is no face above it to measure a base
                    // from, and the backing has to be at least as thick as the whole stack is tall -
                    // everything the picture is made of prints above it.
                    layer.chroma.base_auto = false;
                    plate.base_mm          = std::max(plate.base_mm, layer.chroma.total_mm());
                    redraw();
                }
                ImGui::EndCombo();
            }
            pop_panel_combo_style();
            hover_tip(_u8L("Which way up the plate prints. Picture up is the ordinary way, and the picture carries "
                     "the layer lines of its own terraces. Picture down prints the picture's face against the "
                     "build plate, which comes out as smooth as the plate is - and turns the relief inside "
                     "out, so the backing has to be thick enough to hold the whole stack above it.\n\n"
                     "Turn brim and raft off for this: both would print across the picture's face."));

            // Face down the picture is on the plate's underside, so the 3D view - which looks down at
            // the bed like every other view in the slicer - shows the opaque backing and nothing else.
            // That is the honest picture of what sits on the bed, and it is useless for judging a
            // photograph, so the one thing missing is a way to get round the other side.
            if (down) {
                ImGui::SameLine(0.f, m_imgui->scaled(0.4f));
                const bool under = !wxGetApp().plater()->get_camera().is_looking_downward();
                if (m_imgui->button(under ? _L("From above") : _L("From below"))) {
                    wxGetApp().plater()->select_view(under ? "top" : "bottom");
                    redraw();
                }
                if (ImGui::IsItemHovered())
                    panel_tooltip(under ?
                                      _u8L("Back to looking down at the bed.") :
                                      _u8L("Looks at the plate from underneath, which is the side the picture "
                                           "is printed on. From above, a plate that prints picture down shows "
                                           "only its backing, because that is what is facing up on the bed."));
            }
        }

    }
    // ---- What a bake of this plate makes ----
    //
    // Not a button. There was one here called Make it a flat tile, beside a Bake that made the other
    // thing, and two buttons for "turn this into what gets printed" is one too many: a reader has to
    // know the difference before they can choose, and the difference is what this row is for. Bake
    // makes whichever of these is chosen.
    if (layer.chroma.plate.generated && has_ramp) {
        ImGui::Dummy(ImVec2(0.f, gap_s));
        // Which of the three flat tiles it builds. They all end up the same slab; what differs is
        // where the bands sit inside it, and that decides what the fill prints in and the order the
        // light crosses the colours in.
        slider_label(_L("Flat tile"));
        {
            const auto name_of = [](int m) {
                switch (m) {
                case 0: return _u8L("Clear in front");
                case 1: return _u8L("Backing behind");
                default: return _u8L("FlatStack");
                }
            };
            ImGui::SetNextItemWidth(-std::max(card_pad, 1.f));
            push_panel_combo_style(m_parent.get_scale());
            if (ImGui::BeginCombo("##hf_flat_mode", name_of(m_chroma_flat_mode).c_str())) {
                for (int m = 2; m >= 0; --m)
                    if (ImGui::Selectable(name_of(m).c_str(), m == m_chroma_flat_mode))
                        m_chroma_flat_mode = m;
                ImGui::EndCombo();
            }
            pop_panel_combo_style();
            hover_tip(_u8L("FlatStack slides each column's own sequence up against the flat face, so the light crosses "
                     "the same filaments in the same order it would on the plate, and the colors are the ones the "
                     "solve chose. The cost is the printing: a layer no longer holds one band, so the printer "
                     "swaps filament inside almost every layer.\n\n"
                     "Backing behind keeps every band at one height and lets the backing come forward to meet the "
                     "picture where the picture is thin. Two filaments a layer, no clear filament, and a column "
                     "holds the same filaments as FlatStack - crossed the other way round, which absorption "
                     "barely notices.\n\n"
                     "Clear in front keeps the bands where a swapped print puts them and fills the space on the "
                     "eye's side. It needs a filament that passes light, and that filament's own haze is in the "
                     "picture, thickest where the picture is thinnest."));
        }
    }
}

// Where the light is, and how the rounding error is spread. Both are chosen once for a picture and
// neither is touched on a normal pass.
void GLGizmoTextureDisplacement::render_chroma_light(TextureDisplacementLayer &layer)
{
    const float gap_s = std::round(m_imgui->scaled(0.4f));


    // ---- Which side the light comes from ----
    //
    // Front lit is a filament painting: light falls on the print and comes back through the layers it
    // got into. Back lit is a lithophane: the light comes through the whole column, so thicker means
    // darker and the base is part of every colour rather than an opaque backing. It is a different
    // print of the same picture, which is why it is a mode here and not a view. Both at once is the
    // third, and is where most back-lit prints end up: a lamp behind and a room in front, the two
    // adding rather than one replacing the other.
    slider_label(_L("Light comes from"));
    {
        const ChromaLighting mode    = layer.chroma.lighting;
        const bool           back    = mode == ChromaLighting::BackLit;
        const bool           both    = mode == ChromaLighting::BothLit;
        const float          combo_w = m_imgui->scaled(7.f);
        // One place the three names are written, so the closed combo and its list cannot drift apart.
        const auto name_of = [](ChromaLighting m) {
            switch (m) {
            case ChromaLighting::BackLit: return _u8L("Behind");
            case ChromaLighting::BothLit: return _u8L("Behind and in front");
            default: return _u8L("The front");
            }
        };
        const auto pick = [&](ChromaLighting m) {
            if (ImGui::Selectable(name_of(m).c_str(), mode == m) && mode != m) {
                layer.chroma.lighting = m;
                chroma_touch(layer.slot);
            }
        };
        ImGui::SetNextItemWidth(combo_w);
        push_panel_combo_style(m_parent.get_scale());
        if (ImGui::BeginCombo("##hf_lighting", name_of(mode).c_str())) {
            pick(ChromaLighting::FrontLit);
            pick(ChromaLighting::BackLit);
            pick(ChromaLighting::BothLit);
            ImGui::EndCombo();
        }
        pop_panel_combo_style();
        hover_tip(_u8L("Where the light is when the print is looked at. In front is a filament painting. Behind is "
                 "a lithophane: the light comes through the whole stack, so thicker is darker, the darkest "
                 "filaments belong at the bottom, and the backing has to be a layer or two rather than solid "
                 "- a picture on a thick base is black from behind.\n\n"
                 "Behind and in front is both at once, which is what a lit plate in a lit room really is. "
                 "A stack chosen for the lamp alone washes out as soon as the room light lands on it, so "
                 "this is the one to solve against for anything that will not hang in the dark."));

        // How much of the picture the lamp accounts for. Both lit only: in the other two modes it would
        // be a second way to say what the mode has already said.
        if (both) {
            slider_label(_L("Balance"));
            // Shown as a percentage because that is how a mix reads, and kept as a fraction because
            // that is what the palette multiplies by.
            float percent = layer.chroma.lighting_balance * 100.f;
            if (typed_slider("##hf_balance", m_imgui->scaled(7.f) - panel_edit_icon_room(), &percent, 0.f, 100.f,
                             "%.0f%%", false,
                             _L("How much of what the eye gets came through the plate rather than off its face. None of "
                                "it is the front-lit print exactly, all of it is the back-lit one, and the middle is a "
                                "plate on a window sill. Set it by eye against the preview and the room the print will "
                                "hang in - there is no number to measure here, only the balance you can see."))) {
                layer.chroma.lighting_balance = std::clamp(percent / 100.f, 0.f, 1.f);
                chroma_touch(layer.slot);
            }
        }

        if (back || both) {
            slider_label(_L("Lamp"));
            ImVec4 led(layer.chroma.led_color.r(), layer.chroma.led_color.g(), layer.chroma.led_color.b(), 1.f);
            if (ImGui::ColorEdit3("##hf_led", &led.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                layer.chroma.led_color = ColorRGB(led.x, led.y, led.z);
                chroma_touch(layer.slot);
            }
            hover_tip(_u8L("The colour of the light behind the print. A warm white lamp and a cold white one show the "
                     "same filaments as different colours, and this is where that is accounted for."));

            ImGui::SameLine(0.f, m_imgui->scaled(0.6f));
            float lumens = layer.chroma.led_intensity;
            if (typed_slider("##hf_led_i", m_imgui->scaled(6.f) - panel_edit_icon_room(), &lumens, 0.2f, 8.f, "%.1fx",
                             false,
                             _L("How bright that light is, as a multiple of a plain white lamp. A deep stack passes very "
                                "little, so more than one is normal here and is what a bright panel behind the print "
                                "actually does."))) {
                layer.chroma.led_intensity = std::max(0.f, lumens);
                chroma_touch(layer.slot);
            }
        }
    }

    // ---- How close a match has to be ----
    //
    // The match takes the nearest colour the stack can print, however far away that is, so a picture
    // with a colour nobody owns gets the least-wrong filament smeared over it and the result reads
    // muddy rather than wrong. A cap refuses the stand-in instead and falls back to lightness alone,
    // which prints that region as a tonal relief of itself: honest about what the drawer cannot do,
    // and the mean error goes UP as this goes up, because the solve stops pretending.
    ImGui::Dummy(ImVec2(0.f, gap_s));
    slider_label(_L("Match strictness"));
    {
        int strictness = std::clamp(layer.chroma.match_strictness, 0, 100);
        if (typed_slider_int("##hf_strictness", m_imgui->scaled(9.f) - panel_edit_icon_room(), &strictness, 0, 100,
                             "%d%%", false,
                             from_u8(Slic3r::format(
                                 _u8L("How far off a color is allowed to be before the stack refuses to stand in for "
                                      "it. All the way down, any printable color may stand in for any color in the "
                                      "picture, which is what every version before this did.\n\n"
                                      "Raising it makes the reported error worse and the print better: the areas it "
                                      "refuses are the ones the filaments cannot reach, and printing those in tone "
                                      "alone looks like shading rather than like the wrong color. %1$d%% is the "
                                      "setting to try first."),
                                 CHROMA_MATCH_STRICTNESS_RECOMMENDED)))) {
            layer.chroma.match_strictness = strictness;
            chroma_touch(layer.slot);
        }
        // The number in the units the rest of the card talks about, since a percentage of a scale
        // nobody has seen means nothing on its own.
        ImGui::PushTextWrapPos(0.f);
        if (strictness <= 0)
            ImGui::TextDisabled("%s", _u8L("Any printable color may stand in for any color in the picture.").c_str());
        else
            ImGui::TextDisabled("%s", Slic3r::format(_u8L("Refuses a stand-in further off than %1$.0f, on the same "
                                                          "scale the error overlay uses."),
                                                     chroma_match_cap_de(strictness))
                                          .c_str());
        ImGui::PopTextWrapPos();
    }

    if (m_imgui->bbl_checkbox(_L("Dither to break up banding"), layer.chroma.dither))
        chroma_touch(layer.slot);
    hover_tip(Slic3r::format(_u8L("Scatters the rounding error into neighbouring pixels instead of rounding each one on its "
                            "own, trading the bands a photograph shows across a sky for fine stipple. Leave it off "
                            "for flat-colored artwork.\n\n"
                            "Solved for %1$.3f mm layers, from the print profile."),
                       layer.chroma.layer_height_mm),
        { { "chroma_dither.gif" } });

    // Why that box is doing nothing on this picture. The solve already acts on it - flat artwork is
    // never dithered and never resampled with blending - and a control that silently stops mattering
    // is worse than one that says why, because the user's next move is to turn it on harder.
    if (layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS) {
        const ChromaPaletteReport &pal = m_chroma_palette_report[size_t(layer.slot)];
        if (pal.palette_exact) {
            ImGui::PushTextWrapPos(0.f);
            const wxString line = pal.more ?
                                      _L_PLURAL("This is flat artwork: more than %1$d color.",
                                                "This is flat artwork: more than %1$d colors.", pal.colors) :
                                      _L_PLURAL("This is flat artwork: %1$d color.",
                                                "This is flat artwork: %1$d colors.", pal.colors);
            ImGui::TextDisabled("%s", (Slic3r::format(into_u8(line), pal.colors) + " " +
                                       _u8L("Dithering is off and the picture is resampled without blending, "
                                            "so its edges stay hard."))
                                          .c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // A one-texel column is a retraction, a string and a blob on any machine, and nothing on the plate
    // is one texel wide anyway. Off by default, because it changes the solved height map and a solve
    // that quietly differs from the one the user approved is not worth the strings it saves.
    if (m_imgui->bbl_checkbox(_L("Pull down lone spikes"), m_chroma_despike))
        chroma_touch(layer.slot);
    hover_tip(Slic3r::format(_u8L("Pulls a single texel standing %1$d or more layers above everything around it down "
                            "to its neighbours. Those columns cost a retraction each and print as strings and "
                            "blobs, and they come from the dither rather than from the picture.\n\n"
                            "A step several texels wide is left alone, so hard edges in flat artwork survive."),
                       CHROMA_DESPIKE_LAYERS_RECOMMENDED));

}


// The views of this picture: what is drawn over the model, and the two tools for taking a solve apart.
//
// They used to be in two places, and neither was this one. Printed colors, Printed layers and Where it
// is wrong were checkboxes in the panel's own View row, where they did not fit: that row is full before
// they are added, and the third of them was drawn past the right edge of the panel, so for a while
// Printed layers was a control nobody could reach. The argument for putting them there was that they
// are views rather than settings, which is true and is not the point - the code only draws them when
// the active layer is a picture, and Printed layers only for a plate the picture made for itself. They
// are one layer's views, already conditional on that layer, and a layer's own views belong in its card,
// where each gets a full row that nothing can clip.
//
// Soften, See through, the layer slider and the debug views came the other way, out of Light and
// detail. Every one of them draws the preview differently and writes nothing into the project, while
// everything left in that section is a dimension of the print. Two of them said so in their own
// comments.
void GLGizmoTextureDisplacement::render_chroma_views(TextureDisplacementLayer &layer)
{
    const float gap_s = std::round(m_imgui->scaled(0.4f));


    // ---- What is drawn over the model ----
    //
    // Three icons in a row rather than three labelled checkboxes. They are the three ways of looking
    // at one picture and only one of them is usually on, which is a toolbar and not a list of
    // settings; the labels live in the tooltips, where a label is read once rather than every time
    // the section is opened.
    ImDrawList *dl  = ImGui::GetWindowDrawList();
    const float ico = std::round(ImGui::GetFrameHeight() * 1.5f);
    const auto  view_toggle = [&](const char *id, int kind, bool &on, const std::string &said) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton(id, ImVec2(ico, ico)))
            on = !on;
        const bool hot = ImGui::IsItemHovered();
        dl->AddRectFilled(p, ImVec2(p.x + ico, p.y + ico),
                          on  ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) :
                          hot ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) :
                                ImGui::GetColorU32(ImGuiCol_Button),
                          ImGui::GetStyle().FrameRounding);
        const ImU32 fg = on ? IM_COL32(255, 255, 255, 235) : ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : 0.7f);
        const float c  = std::round(ico * 0.5f);
        const float q  = std::round(ico * 0.17f);
        if (kind == 0) {
            // The printed colours: four squares of one picture's worth of them.
            for (int i = 0; i < 4; ++i) {
                const float ox = (i % 2 == 0 ? -q - 1.f : 1.f), oy = (i < 2 ? -q - 1.f : 1.f);
                dl->AddRectFilled(ImVec2(p.x + c + ox, p.y + c + oy), ImVec2(p.x + c + ox + q, p.y + c + oy + q),
                                  fg, 1.f);
            }
        } else if (kind == 1) {
            // The printed layers: sheets seen edge on.
            for (int i = -1; i <= 1; ++i)
                dl->AddRectFilled(ImVec2(p.x + c - 1.6f * q, p.y + c + float(i) * 1.5f * q - 1.f),
                                  ImVec2(p.x + c + 1.6f * q, p.y + c + float(i) * 1.5f * q + 1.f), fg);
        } else {
            // Where it is wrong: a mark over the one place it is.
            dl->AddCircle(ImVec2(p.x + c, p.y + c), 1.6f * q, fg, 0, 1.6f);
            dl->AddCircleFilled(ImVec2(p.x + c, p.y + c), 0.6f * q, fg);
        }
        if (hot)
            panel_tooltip(said);
    };

    bool colors_on = m_show_chroma_colors;
    view_toggle("##v_colors", 0, colors_on,
                _u8L("Printed colors: the model painted with the colors this stack will print, every surface "
                     "taking the color of the printed layer it sits at. Seen from above, that is the picture.\n\n"
                     "It stays after a bake, and it is the only place these colors exist outside the sliced "
                     "preview - they are blends of translucent filaments, not filaments, so the 3D scene cannot "
                     "paint them."));
    if (colors_on != m_show_chroma_colors) {
        m_show_chroma_colors = colors_on;
        m_chroma_color_key   = 0;
        m_parent.set_as_dirty();
    }

    // Only for a plate the picture made for itself: a printed layer is a horizontal slice of the
    // world, and that is a sheet over the picture only while the picture is a flat rectangle on the bed.
    if (layer.chroma.plate.generated) {
        ImGui::SameLine(0.f, gap_s);
        bool layers_on = m_show_chroma_layers;
        view_toggle("##v_layers", 1, layers_on,
                    _u8L("Printed layers: the print drawn as the stack of translucent sheets it is, one for each "
                         "printed layer, at its own height, in its own filament.\n\n"
                         "Nothing here is lit. A sheet is its filament at the alpha one printed layer of it stops, "
                         "and the picture is what two dozen of them add up to - which is also why the thin places "
                         "let the scene behind the plate through."));
        if (layers_on != m_show_chroma_layers) {
            m_show_chroma_layers = layers_on;
            m_parent.set_as_dirty();
        }
    }

    ImGui::SameLine(0.f, gap_s);
    bool error_on = m_show_chroma_error;
    view_toggle("##v_error", 2, error_on,
                _u8L("Where it is wrong: the model painted with how far each part of the picture is from what the "
                     "stack can print. Grey where it matches, amber where it is close, red where the filaments "
                     "cannot reach the picture at all.\n\n"
                     "This is where to look before changing anything."));
    if (error_on != m_show_chroma_error) {
        m_show_chroma_error = error_on;
        m_parent.set_as_dirty();
    }

    // The one thing about the sheets worth a switch, and only while they are drawn.
    if (m_show_chroma_layers && layer.chroma.plate.generated) {
        ImGui::SameLine(0.f, 2.f * gap_s);
        if (m_imgui->bbl_checkbox(_L("Backing"), m_chroma_layers_backing))
            m_parent.set_as_dirty();
        hover_tip(_u8L("Draws the solid backing behind the sheets, the way the print has it. At any real thickness it is "
                 "opaque, so with it on the picture sits against a wall and nothing of the scene behind the plate "
                 "comes through."));
    }

    ImGui::Dummy(ImVec2(0.f, gap_s));

    // ---- The half of this that is for taking a solve apart ----
    //
    // Folded, and folded by default. None of it is wrong to have and none of it is anything to reach
    // for while a picture is being set up: it is here for the afternoon when the preview looks wrong
    // and the question is which of three things went wrong.
    {
        const float  x0 = ImGui::GetCursorPosX();
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const float  fh = ImGui::GetFrameHeight();
        if (ImGui::InvisibleButton("##chroma_dbg_head",
                                   ImVec2(std::max(m_imgui->scaled(2.f), ImGui::GetContentRegionMax().x - x0), fh)))
            m_chroma_open_debug = !m_chroma_open_debug;
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, ImGui::IsItemHovered() ? 0.95f : 0.65f);
        const float a   = std::round(ImGui::GetFontSize() * 0.3f);
        const float cx = p.x + a + 2.f, cy = p.y + 0.5f * fh;
        if (m_chroma_open_debug)
            dl->AddTriangleFilled(ImVec2(cx - a, cy - 0.5f * a), ImVec2(cx + a, cy - 0.5f * a),
                                  ImVec2(cx, cy + 0.7f * a), col);
        else
            dl->AddTriangleFilled(ImVec2(cx - 0.5f * a, cy - a), ImVec2(cx + 0.7f * a, cy),
                                  ImVec2(cx - 0.5f * a, cy + a), col);
        const std::string name = _u8L("Taking a solve apart");
        dl->AddText(ImVec2(cx + a + 4.f, cy - 0.5f * ImGui::GetFontSize()), col, name.c_str());
        ImGui::SetCursorPosX(x0);
    }
    if (!m_chroma_open_debug)
        return;

    ImGui::Dummy(ImVec2(0.f, gap_s));
    // A view rather than a setting, but it belongs with the light: what it switches is whether the
    // preview accounts for light going into the plastic at one point and coming out at another. On by
    // default, because the soft picture is the one the printer makes and the hard one promises detail
    // the material cannot hold. Worth turning off to see the solve's own steps.
    if (m_imgui->bbl_checkbox(_L("Soften it the way the plastic does"), m_show_chroma_scatter))
        m_parent.set_as_dirty();
    hover_tip(_u8L("Filament scatters light sideways, so a print is softer than the stack it was solved from and "
             "detail finer than about a nozzle width never survives the plastic. With this on, the preview "
             "shows that softening; with it off, it shows the solve's own terraces, which no print has.\n\n"
             "Lit from behind it is much stronger, because the light crosses the whole plate rather than "
             "the top filament only - which is why a lithophane loses fine detail."));

    // Also a view, and the other half of the same question: the softening above is the light that comes
    // back out of the plastic, this is the light that goes straight through it and out the far side.
    //
    // Off by default, and not only because a new view should be. It is the one option here that changes
    // how the plate is drawn rather than what it is drawn as: the opaque surface has to be left out of
    // the frame for the translucency to show at all (see on_render), and with it goes the paint
    // highlight. The place to say that is here rather than in a comment nobody reading the panel can see.
    {
        const bool back_lit = layer.chroma.lighting != ChromaLighting::FrontLit;
        m_imgui->disabled_begin(back_lit);
        if (m_imgui->bbl_checkbox(_L("See through the thin places"), m_chroma_see_through))
            m_parent.set_as_dirty();
        m_imgui->disabled_end();
        if (back_lit)
            hover_tip(_u8L("Only with the light in front. Lit from behind, the plate stands in front of its own "
                     "lamp and what you see is already the light that came through it, so there is nothing "
                     "behind it left to show."));
        else
            hover_tip(_u8L("Draws the plate as the translucent thing it is: the stack's own transmittance becomes "
                     "how solid each pixel is, so whatever stands behind the print shows through wherever "
                     "the filament does not stop the light.\n\n"
                     "A finished plate is nearly opaque, and that is not a shortcoming of the preview - it "
                     "is why a print reads as a picture rather than a stained-glass window. Expect to see "
                     "this at the thin places only: the first layers of a clear filament, a plate that is "
                     "almost all relief, or the stack part way up as the layer slider walks it.\n\n"
                     "While it is on, a brush stroke shows as the green tint instead of the solid "
                     "highlight, which would otherwise cover the print it is meant to be seen through."));
    }

    // ---- The layer slider ----
    //
    // Which swap bought what. A finished stack does not say: every band is under every band above it,
    // and the one that contributed nothing looks exactly like the one that carried the picture. Walking
    // the limit up shows the print as it stood at each layer, which is the same thing the user would
    // have seen standing over the printer, and it is the fastest way to find a band paying for a swap
    // and changing nothing.
    {
        const int total = std::max(1, layer.chroma.total_layers());
        // At the top it is the finished print, which is also the resting state, so the slider is left
        // there rather than being a mode that has to be switched off.
        int shown = (m_chroma_layer_limit > 0 && m_chroma_layer_limit < total) ? m_chroma_layer_limit : total;
        ImGui::Dummy(ImVec2(0.f, gap_s));
        slider_label(_L("Show up to layer"));
        // The height in millimetres rides along in the slider's own text: a layer number means nothing
        // on a stack whose layer height the user set somewhere else.
        const std::string fmt = (shown >= total) ?
                                    Slic3r::format(_u8L("%1$d of %2$d - all of it"), shown, total) :
                                    Slic3r::format(_u8L("%1$d of %2$d - %3$.2f mm"), shown, total,
                                                   float(shown) * layer.chroma.layer_height_mm);
        // The one slider in the gizmo that is not a typed_slider_int(): that one puts the bounds in the
        // tooltip by running the format over them, and this format takes three arguments rather than the
        // one a bound would supply.
        ImGui::SetNextItemWidth(m_imgui->scaled(9.f));
        if (ImGui::SliderInt("##hf_layer_limit", &shown, 1, total, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
            m_chroma_layer_limit = (shown >= total) ? 0 : shown;
            m_parent.set_as_dirty();
        }
        hover_tip(_u8L("Draws the print as it stood at that layer, so the stack can be walked up one band at a "
                 "time. Everything already finished below the line keeps its color; everything above it "
                 "shows the color the stack had reached by then.\n\n"
                 "This is how to find a band that is paying for a filament swap and changing nothing: stop "
                 "just below it, then just above it, and see whether the picture moved."));
    }

    // ---- The debug views ----
    //
    // A preview that looks wrong cannot say why: a stack that never got its relief, a picture that
    // landed somewhere else on the model, and a walk that came out dark all end in the same flat
    // wash. Each of these draws one of the quantities the shader works from instead of the colour it
    // predicts, which is the only way to tell those three apart by looking.
    ImGui::Dummy(ImVec2(0.f, gap_s));
    slider_label(_L("Debug view"));
    {
        // The order is the order to try them in, which is also the order they rule things out in, and
        // the index is what the shader switches on - see CHROMA_DBG_* in chromastack.fs.
        const auto name_of = [](int v) {
            switch (v) {
            case ChromaDebugHeight: return _u8L("Height in the stack");
            case ChromaDebugBand: return _u8L("Which band");
            case ChromaDebugFacing: return _u8L("Terrace or wall");
            case ChromaDebugUV: return _u8L("Where the picture lands");
            case ChromaDebugSolve: return _u8L("What the solve predicted");
            case ChromaDebugAlbedo: return _u8L("Colour with no light on it");
            default: return _u8L("Off");
            }
        };
        ImGui::SetNextItemWidth(m_imgui->scaled(9.f));
        push_panel_combo_style(m_parent.get_scale());
        if (ImGui::BeginCombo("##hf_debug", name_of(m_chroma_debug).c_str())) {
            for (int v = 0; v < int(ChromaDebugCount); ++v)
                if (ImGui::Selectable(name_of(v).c_str(), v == m_chroma_debug) && v != m_chroma_debug) {
                    m_chroma_debug = v;
                    m_parent.set_as_dirty();
                }
            ImGui::EndCombo();
        }
        pop_panel_combo_style();
        hover_tip(_u8L("Draws one of the things the shader works from instead of the colour it predicts. Off is the "
                 "normal preview.\n\n"
                 "Height in the stack: how far up the stack each point sits, dark blue at the base to pale "
                 "yellow at the top. A plate whose relief never reached the mesh is one flat colour here, and "
                 "that is the first thing to rule out.\n\n"
                 "Which band: the filament each point's own layer belongs to, one colour per band. The "
                 "picture's terraces should read as flat fields with hard edges.\n\n"
                 "Terrace or wall: green where the shader looks down through the column, red where it reads "
                 "the cut side of the layers instead.\n\n"
                 "Where the picture lands: the picture's own coordinates on the model, black beyond its edge.\n\n"
                 "What the solve predicted: the panel's \"What prints\" image laid on the model. Where this "
                 "and the normal preview disagree, the solve and the shader disagree.\n\n"
                 "Colour with no light on it: the walk's own answer with the shading taken off."));
    }
}

// The picture and the print it is predicted to make, at the size of the canvas.
//
// The card's two thumbnails answer "is this worth printing"; they cannot answer "is this face still
// the face", which is the question a portrait or anything with text in it actually turns on. Drawn
// over the 3D view rather than in the panel, because the panel is 420 px wide and that is the whole
// problem.
// The picture's colours and the stack's reach, in the one space where the question is legible.
//
// The error overlay answers "where", per pixel, on the model. It cannot answer "why", and the three
// usual whys - two filaments fighting over the same depths, a band repeating the one below it, a
// picture whose colours are not in the drawer - are indistinguishable from a red wash over a flower.
// Hue against lightness separates them: what the stack can print is a curve through its own levels,
// what the picture wants is a cloud weighted by how much of the picture each colour is, and the
// distance from cloud to curve is the thing that goes wrong.
//
// Drawn over the 3D view rather than in the panel, as the zoom is, because 420 px is not a chart.
void GLGizmoTextureDisplacement::render_chroma_diagnostics()
{
    if (m_chroma_diag_slot < 0 || size_t(m_chroma_diag_slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    const ChromaDiagnostics &diag = m_chroma_diag[size_t(m_chroma_diag_slot)];
    if (diag.empty()) {
        m_chroma_diag_slot = -1;
        return;
    }

    const ImGuiStyle &style  = ImGui::GetStyle();
    const ImVec2      screen = ImGui::GetIO().DisplaySize;
    const float       pad    = std::round(m_imgui->scaled(1.f));
    const float       line   = ImGui::GetTextLineHeightWithSpacing();
    const float       win_w  = std::min(screen.x * 0.9f, m_imgui->scaled(44.f));
    const float       win_h  = std::min(screen.y * 0.88f, m_imgui->scaled(34.f));
    // Twice as wide as it is tall: hue runs a full turn and lightness only black to white, so equal
    // axes would squash the one that carries the most information into a strip.
    const float       plot_w = win_w - 2.f * pad - m_imgui->scaled(2.2f);
    const float       plot_h = std::max(m_imgui->scaled(6.f), std::min(0.5f * plot_w, win_h * 0.45f));

    const auto col_of = [](const ColorRGB &c, int alpha) {
        return IM_COL32(int(std::lround(255.f * std::clamp(c.r(), 0.f, 1.f))),
                        int(std::lround(255.f * std::clamp(c.g(), 0.f, 1.f))),
                        int(std::lround(255.f * std::clamp(c.b(), 0.f, 1.f))), alpha);
    };

    ImGui::SetNextWindowPos(ImVec2(0.5f * (screen.x - win_w), 0.5f * (screen.y - win_h)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    bool open = true;
    if (ImGui::Begin("##chroma_diag", &open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) {
        m_imgui->title(_u8L("Color analysis"));

        // ---- The chart ----
        const float   label_w = m_imgui->scaled(2.2f);
        ImDrawList   *wdl     = ImGui::GetWindowDrawList();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + label_w);
        const ImVec2  origin  = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(plot_w, plot_h));
        const ImU32   grid    = ImGui::GetColorU32(ImGuiCol_Separator);
        const ImU32   faint   = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        wdl->AddRectFilled(origin, ImVec2(origin.x + plot_w, origin.y + plot_h),
                           ImGui::GetColorU32(ImGuiCol_FrameBg));

        // Lightness runs up the left, because that is the axis the relief works along: a column's
        // height reaches the eye mostly as tone, so two filaments at the same height on this chart
        // give a ramp that does not ramp however different their hues are.
        for (int l = 0; l <= 100; l += 25) {
            const float y = origin.y + plot_h * (1.f - 0.01f * float(l));
            wdl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + plot_w, y), grid);
            const std::string t = std::to_string(l);
            wdl->AddText(ImVec2(origin.x - m_imgui->scaled(0.3f) - ImGui::CalcTextSize(t.c_str()).x,
                                y - 0.5f * ImGui::GetTextLineHeight()),
                         faint, t.c_str());
        }
        // Hue wraps, so the cut has to be drawn rather than left for the reader to infer: 0 and 360
        // are the same colour and the two edges of this chart are neighbours.
        for (int h = 0; h <= 360; h += 90) {
            const float x = origin.x + plot_w * (float(h) / 360.f);
            wdl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + plot_h), grid);
        }

        const auto at = [&](const ChromaMapCoord &c) {
            return ImVec2(origin.x + plot_w * std::clamp(c.hue_deg, 0.f, 360.f) / 360.f,
                          origin.y + plot_h * (1.f - std::clamp(c.lightness, 0.f, 100.f) / 100.f));
        };

        // What the picture asks for, each bin as large as its share of the picture. Drawn first and
        // underneath: the question is whether the curve passes through the cloud, and a cloud over the
        // curve hides the answer.
        float heaviest = 0.f;
        for (const ChromaWant &w : diag.wants)
            heaviest = std::max(heaviest, w.weight);
        for (const ChromaWant &w : diag.wants) {
            const float share = heaviest > 0.f ? w.weight / heaviest : 0.f;
            const float r     = m_imgui->scaled(0.12f) + m_imgui->scaled(0.42f) * std::sqrt(share);
            // A near-grey has a hue angle that is arithmetically fine and means nothing, so it is drawn
            // faint rather than scattered around the circle on the strength of a rounding difference.
            const float vivid = std::clamp(w.at.chroma_ab / 30.f, 0.f, 1.f);
            const int   a     = int(std::lround(60.f + 150.f * vivid));
            wdl->AddCircleFilled(at(w.at), r, col_of(w.color, a));
        }

        // What the stack can print: consecutive levels joined, so the line is the walk the palette
        // takes from the base to the top. Where it doubles back on itself, a swap is being paid for
        // twice; where the cloud sits far from it, those colours cannot be printed at all.
        for (size_t i = 1; i < diag.reach.size(); ++i) {
            const ChromaReach &a0 = diag.reach[i - 1];
            const ChromaReach &b0 = diag.reach[i];
            wdl->AddLine(at(a0.at), at(b0.at), col_of(b0.color, 230), m_imgui->scaled(0.14f));
        }
        for (const ChromaReach &r : diag.reach) {
            const ImVec2 p = at(r.at);
            // A level that prints none of the picture is a layer nobody asked for, and it is drawn
            // hollow so a dead stretch of the curve can be seen as a run of them.
            const float  rad = m_imgui->scaled(r.serves > 0.f ? 0.22f : 0.16f);
            if (r.serves > 0.f)
                wdl->AddCircleFilled(p, rad, col_of(r.color, 255));
            else
                wdl->AddCircle(p, rad, faint, 0, m_imgui->scaled(0.07f));
            wdl->AddCircle(p, rad, ImGui::GetColorU32(ImGuiCol_WindowBg), 0, m_imgui->scaled(0.05f));
        }
        wdl->AddRect(origin, ImVec2(origin.x + plot_w, origin.y + plot_h), grid);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + label_w);
        ImGui::TextDisabled("%s", _u8L("Across: hue, a full turn, so the two edges of the chart are "
                                       "neighbours. Up: lightness. Filled dots are the picture, the line "
                                       "is what this stack can print, and a hollow dot is a layer that "
                                       "prints none of the picture.")
                                      .c_str());

        // ---- The numbers, as figures rather than as a sentence ----
        //
        // They were one line of prose, and at this width the end of it was drawn past the edge of the
        // window - the reader lost the last two numbers of five and had no way of knowing it. Figures
        // in boxes read at a glance, line up under each other, and cannot run off anything.
        ImGui::Dummy(ImVec2(0.f, pad));
        {
            struct Tile
            {
                std::string   value;
                std::string   label;
                const ImVec4 *color;
            };
            const ImVec4 *avg_col = diag.mean_de > 15.  ? &CHROMA_COL_WARN :
                                    diag.mean_de <= 5.  ? &CHROMA_COL_GOOD :
                                                          nullptr;
            const ImVec4 *bad_col = diag.unreachable_weight > 0.15f ? &CHROMA_COL_WARN : nullptr;
            const std::vector<Tile> tiles = {
                { Slic3r::format("%1$.1f", diag.mean_de), _u8L("average off"), avg_col },
                { Slic3r::format("%1$.1f", diag.worst_de), _u8L("worst off"), nullptr },
                { Slic3r::format("%1$.0f%%", 100. * double(diag.matched_weight)), _u8L("matched"), nullptr },
                { Slic3r::format("%1$.0f%%", 100. * double(diag.unreachable_weight)), _u8L("out of reach"), bad_col },
                { Slic3r::format("%1$d / %2$d", diag.distinct_levels, diag.relief_layers),
                  _u8L("layers that change it"), nullptr },
            };
            const float gap = std::round(m_imgui->scaled(0.4f));
            const float tw  = std::floor((win_w - 2.f * pad - gap * float(tiles.size() - 1)) / float(tiles.size()));
            const float th  = std::round(ImGui::GetTextLineHeight() * 2.8f);
            for (size_t i = 0; i < tiles.size(); ++i) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(tw, th));
                wdl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(ImGuiCol_FrameBg),
                                   style.FrameRounding);
                const ImVec2 vs = ImGui::CalcTextSize(tiles[i].value.c_str());
                wdl->AddText(ImVec2(std::round(p.x + 0.5f * (tw - vs.x)), std::round(p.y + 0.22f * th)),
                             tiles[i].color != nullptr ? ImGui::GetColorU32(*tiles[i].color) :
                                                         ImGui::GetColorU32(ImGuiCol_Text),
                             tiles[i].value.c_str());
                const ImVec2 ls = ImGui::CalcTextSize(tiles[i].label.c_str());
                wdl->AddText(ImVec2(std::round(p.x + 0.5f * (tw - ls.x)), std::round(p.y + th - ls.y - 0.16f * th)),
                             faint, tiles[i].label.c_str());
                if (i + 1 < tiles.size())
                    ImGui::SameLine(0.f, gap);
            }
        }

        // The findings are not here. They were paragraphs, several of them, and a window that opens
        // on a chart and a row of figures should not then ask to be read: what this window is for is
        // seeing where the picture is and where the stack can go, which the chart says without words.
        // ChromaDiagnostics still carries them, and the card still counts on them to know whether
        // there is anything to show at all.
        ImGui::Dummy(ImVec2(0.f, pad));
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), win_h - ImGui::GetFrameHeight() - 2.f * pad));

        const std::string close = _u8L("Close");
        const float       cw    = ImGui::CalcTextSize(close.c_str()).x + 2.f * style.FramePadding.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), win_w - pad - cw));
        if (ImGui::Button((close + "##chroma_diag_close").c_str(), ImVec2(cw, ImGui::GetFrameHeight())))
            open = false;
        // Escape and a click outside, because this is something being read rather than answered.
        if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)) ||
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
             !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)))
            open = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (!open)
        m_chroma_diag_slot = -1;
}

void GLGizmoTextureDisplacement::render_chroma_zoom()
{
    if (m_chroma_zoom_slot < 0 || size_t(m_chroma_zoom_slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    const size_t     slot = size_t(m_chroma_zoom_slot);
    const GLTexture *src  = m_chroma_source_thumb[slot].get();
    const GLTexture *out  = m_show_chroma_error && m_chroma_error_tex[slot] ? m_chroma_error_tex[slot].get() :
                                                                             m_chroma_preview[slot].get();
    if (src == nullptr && out == nullptr) {
        m_chroma_zoom_slot = -1;
        return;
    }

    const ImGuiStyle &style  = ImGui::GetStyle();
    const ImVec2      screen = ImGui::GetIO().DisplaySize;
    // Two squares side by side with a gap, as large as the shorter axis of the canvas allows. Square
    // because both textures are, and the pair has to be compared at one size or the comparison is
    // between two different pictures.
    const float pad  = std::round(m_imgui->scaled(1.f));
    const float side = std::max(m_imgui->scaled(8.f),
                                std::min(0.5f * (screen.x * 0.9f - 3.f * pad),
                                         screen.y * 0.82f - 4.f * pad - 2.f * ImGui::GetTextLineHeight()));
    const ImVec2 win(2.f * side + 3.f * pad, side + 4.f * pad + 2.f * ImGui::GetTextLineHeight());

    ImGui::SetNextWindowPos(ImVec2(0.5f * (screen.x - win.x), 0.5f * (screen.y - win.y)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(win, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    bool open = true;
    if (ImGui::Begin("##chroma_zoom", &open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) {
        const auto one = [&](const GLTexture *tex, const std::string &caption) {
            ImGui::BeginGroup();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (tex != nullptr)
                ImGui::Image((ImTextureID) (intptr_t) tex->get_id(), ImVec2(side, side));
            else {
                ImGui::Dummy(ImVec2(side, side));
                ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + side, p.y + side),
                                                    ImGui::GetColorU32(ImGuiCol_Separator));
            }
            ImGui::TextDisabled("%s", caption.c_str());
            ImGui::EndGroup();
        };
        one(src, _u8L("Your picture"));
        ImGui::SameLine(0.f, pad);
        one(out, m_show_chroma_error && m_chroma_error_tex[slot] ? _u8L("Where it is wrong") : _u8L("What prints"));

        const std::string close = _u8L("Close");
        const float       cw    = ImGui::CalcTextSize(close.c_str()).x + 2.f * style.FramePadding.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), win.x - pad - cw));
        if (ImGui::Button((close + "##chroma_zoom_close").c_str(), ImVec2(cw, ImGui::GetFrameHeight())))
            open = false;
        // Escape, and a click anywhere outside it, because this is a picture being looked at rather
        // than a dialog being answered.
        if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)) ||
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)))
            open = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (!open)
        m_chroma_zoom_slot = -1;
}

bool GLGizmoTextureDisplacement::chroma_commit_adjusted_picture(TextureDisplacementLayer &layer,
                                                                const DecodedHeightTexture &adjusted)
{
    if (adjusted.empty())
        return false;

    boost::system::error_code ec;
    const boost::filesystem::path dir(chroma_user_picture_dir());
    boost::filesystem::create_directories(dir, ec);

    // Named after the picture it came from, numbered rather than overwritten: a user who adjusts the
    // same photograph twice means two pictures, not one that quietly changed under the first plate.
    const std::string     stem = layer.name.empty() ? _u8L("Picture") : layer.name;
    boost::filesystem::path path;
    for (int n = 1; n < 1000; ++n) {
        const std::string suffix = n == 1 ? std::string() : " " + std::to_string(n);
        path                     = dir / (stem + " - " + _u8L("adjusted") + suffix + ".png");
        if (!boost::filesystem::exists(path, ec))
            break;
    }

    const bool written = adjusted.has_color() ?
                             png::write_rgb_to_file(path.string(), size_t(adjusted.width), size_t(adjusted.height),
                                                    adjusted.rgb) :
                             png::write_gray_to_file(path.string(), size_t(adjusted.width), size_t(adjusted.height),
                                                     adjusted.pixels);
    if (!written) {
        show_error(nullptr, Slic3r::format(_u8L("Could not save the adjusted picture to %1%."), path.string()));
        return false;
    }

    std::string                                 error;
    std::shared_ptr<std::vector<unsigned char>> bytes = load_texture_image_data(path.string(), error);
    if (!bytes) {
        show_error(nullptr, error);
        return false;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Adjust ChromaStack picture"),
                                  UndoRedo::SnapshotType::GizmoAction);
    // Cleared before the layer is pointed at the new file, as the relief path does and for the same
    // reason: the saved image already has the adjustment in its pixels, so a live preview_adjust left
    // set would apply all of it a second time on top of itself.
    layer.preview_adjust = TextureAdjustments{};
    layer.chroma_source  = std::move(bytes);
    layer.path           = path.string();
    layer.name           = path.stem().string();
    // The picture is the solve's whole input, so this is a re-solve, not a redraw.
    chroma_touch(layer.slot);
    return true;
}

} // namespace Slic3r::GUI
