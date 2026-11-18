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
        // The same colour in linear light, which is the space the walk composites in and the space
        // chroma_palette() composited this stack in: handed over rather than converted per fragment,
        // and taken from the library so the shader cannot drift from the prediction it is previewing.
        shader->set_uniform(("band_color_lin" + idx).c_str(), chroma_srgb_to_linear(band.filament.color));
        shader->set_uniform(("band_density" + idx).c_str(),
                            chroma_layer_density(band.filament.td_mm, layer.chroma.layer_height_mm));
        shader->set_uniform(("band_layers" + idx).c_str(),
                            float(band.layers + (i == 0 ? std::max(0, layer.chroma.base_layers) : 0)));
        // The highlight's strength for this band's filament, from the finish its name declares. Set
        // for every band on every pass: an unset uniform is 0, which would take the highlight off.
        const ChromaFinish finish = chroma_finish_from_name(band.filament.name);
        shader->set_uniform(("band_gloss" + idx).c_str(), finish == ChromaFinish::Matte  ? 0.35f :
                                                          finish == ChromaFinish::Glossy ? 2.5f :
                                                                                           1.f);
        // The secondary effect, sent for every band so a band without one costs nothing to branch on:
        // a band that does not shift turns into its own colour, and one that does not glow gives off
        // black.
        const ChromaFilament &f     = band.filament;
        const ColorRGB        shift = f.effect == ChromaEffect::ColorShift ? f.effect_color : f.color;
        shader->set_uniform(("band_shift" + idx).c_str(), shift);
        shader->set_uniform(("band_shift_lin" + idx).c_str(), chroma_srgb_to_linear(shift));
        shader->set_uniform(("band_glow_lin" + idx).c_str(),
                            f.effect == ChromaEffect::Glow ? chroma_srgb_to_linear(f.effect_color) : ColorRGB(0.f, 0.f, 0.f));
    }
    shader->set_uniform("slicer_view", m_chroma_slicer_view ? 1 : 0);
    // Only while something glows: with nothing to give light, lights off is a black plate, and the
    // toggle is not offered then either.
    shader->set_uniform("lights_off", (m_chroma_lights_off && chroma_has_glow(layer.chroma)) ? 1 : 0);
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
        // In linear light for the transmitted pass, which tints the lamp by the filament's hue the way
        // chroma_palette()'s walk_back() does. The reflected pass still uses the sRGB colour, because
        // the blender accumulates the sheets in the frame buffer and that is not a linear space.
        shader->set_uniform("sheet_color_lin", chroma_srgb_to_linear(color));
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
// A drag never settles while it is held (chroma_settle() waits for no item to be active), so this
// only has to outlast the gap between keystrokes and clicks.
static constexpr int64_t CHROMA_SETTLE_MS = 200;

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
        // The secondary effect, from the name for the same reason the finish is: no preset carries
        // one, and every maker puts it in what the spool is called.
        f.effect       = chroma_effect_from_name(f.name);
        f.effect_color = chroma_default_effect_color(f.effect, f.color);
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

    // Every slot's mask goes with the old triangle list, not just this one's. A paint mask means what
    // it did only while the topology is unchanged, and this is a different mesh: a second layer's mask
    // left behind selects whatever triangles happen to carry its old indices, which puts that layer's
    // relief somewhere else on the plate. TriangleSelector::deserialize() refuses only an index past
    // the end, so a rebuilt plate with at least as many triangles takes the stale mask in silence.
    // The bake clears all of them for the same reason, and so does chroma_make_flat_tile(); this
    // slot's own mask is written again from the result on the next line.
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        mv->texture_displacement_facet(i).reset();
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

// A plate the picture's shape, its longer side `longest` millimetres: a photograph stretched to a shape
// that is not its own is not the photograph any more. `aspect` is the picture's width over its height.
static void chroma_plate_fit(ChromaPlate &plate, float longest, float aspect)
{
    aspect          = std::max(0.05f, aspect);
    plate.width_mm  = aspect >= 1.f ? longest : longest * aspect;
    plate.height_mm = aspect >= 1.f ? longest / aspect : longest;
}

void GLGizmoTextureDisplacement::chroma_plate_changed(TextureDisplacementLayer &layer)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    // The frame's cross-section is resolved against the picture's size and the backing under it, so any
    // change to the plate can invalidate it. Re-resolving costs a polyline and saves the moulding quietly
    // drifting out of proportion as the plate is resized.
    chroma_refresh_frame(layer);
    m_chroma_plate_dirty[size_t(layer.slot)] = true;
    chroma_touch(layer.slot);
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
    // No frame until one is picked from the gallery: the picture runs to the edge of the plate. Set
    // here rather than in ChromaFrame's default, which is also what a project saved without a frame
    // entry reads back.
    plate.frame.width_mm = 0.f;
    chroma_plate_fit(plate, CHROMA_PLATE_DEFAULT_MM, aspect);

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

    // A plate the picture made for itself takes the new picture's shape: the plate *is* the picture, and
    // a portrait laid on a plate built for a landscape is a picture squeezed into the wrong frame. The
    // longer side keeps the size it had, since that is the size the plate was given. The rebuild this
    // asks for lays the picture over the new face and solves it again (see chroma_apply_plate()).
    if (layer.chroma.plate.generated) {
        TextureDisplacementLayer probe;
        probe.image_data   = layer.chroma_source;
        ChromaPlate &plate = layer.chroma.plate;
        chroma_plate_fit(plate, std::max(plate.width_mm, plate.height_mm), layer_texture_aspect(probe));
        chroma_plate_changed(layer);
    }

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
    // A filter being tried on the picture. It is read through chroma_solve_input() rather than from the
    // stack, so a key that leaves it out would keep the solve on the unfiltered picture while the
    // card's thumbnail showed the filtered one.
    {
        const TextureAdjustments adj = chroma_live_adjust(layer);
        mix_f(adj.brightness);
        mix_f(adj.contrast);
        mix_f(adj.sharpness);
        mix_f(adj.blur);
        mix_f(adj.hue_deg);
        mix_f(adj.saturation);
        mix(uint64_t(int(adj.brightness_space)));
    }

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

    // How big the picture is on the print, and how fine a line the printer draws. Neither is part of
    // the stack and both reach the solve through chroma_solve_input(): together they size the dither
    // cell (ChromaStackJob derives dither_cell_px from picture_mm / width against line_width_mm), and
    // the first also picks the grid the full-resolution solve runs on, through
    // chroma_printable_solve_px(). Left out, dragging a plate from 100 mm to 250 mm kept a height map
    // solved at two and a half times too few texels with a stipple that coarse in millimetres, and so
    // did changing the nozzle or the line width in the print profile.
    //
    // Hashed whether or not the dither is on, because the resolution is not conditional and because a
    // key that is right only for the current value of another setting is the kind of key this one
    // already had.
    const ChromaPrintScale scale = chroma_print_scale();
    mix_f(scale.line_width_mm);
    mix_f(scale.nozzle_mm);

    const ChromaPatch patch = chroma_patch(layer);
    if (patch.valid) {
        const Vec2f extent = patch.planar_max - patch.planar_min;
        mix_f(std::max(std::abs(extent.x()), std::abs(extent.y())));

        // The patch's geometry otherwise enters only through the tilt correction, which a level patch
        // skips (see chroma_run_solve()), so a flat face - which every generated plate has -
        // contributes nothing more. A tilted one does, through both its angle and where the picture
        // sits on it.
        if (patch.tilt_deg > 0.5f) {
            mix_f(patch.tilt_deg);
            mix_f(layer.tiling_scale);
            mix_f(layer.rotation_deg);
            mix_f(layer.offset.x());
            mix_f(layer.offset.y());
        }
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
    out.source_adjust  = chroma_live_adjust(layer);
    return true;
}

const DecodedHeightTexture &GLGizmoTextureDisplacement::chroma_thumb_base(const TextureDisplacementLayer &layer)
{
    static const DecodedHeightTexture none;
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS || !layer.chroma_source)
        return none;
    const size_t slot = size_t(layer.slot);
    if (m_chroma_thumb_base_key[slot] != layer.chroma_source.get()) {
        TextureDisplacementLayer probe;
        probe.image_data            = layer.chroma_source;
        m_chroma_thumb_base[slot]   = downscale_height_texture(decode_height_texture(probe), THUMBNAIL_MAX_PX);
        m_chroma_thumb_base_key[slot] = layer.chroma_source.get();
    }
    return m_chroma_thumb_base[slot];
}

TextureAdjustments GLGizmoTextureDisplacement::chroma_live_adjust(const TextureDisplacementLayer &layer) const
{
    return layer.chroma_enabled && adjust_session_on(layer) ? m_adjust_applied : TextureAdjustments{};
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
    if (plater == nullptr || mo == nullptr || mv == nullptr)
        return;

    // By slot, searched for rather than indexed with: a slot is a layer's name, not its place in the
    // vector. remove_texture_layer() erases out of the middle and swap_layer_slots() renumbers without
    // reordering, so after either of those the two disagree - indexing found the wrong layer's picture,
    // or ran off the end and did nothing at all. chroma_layer_in_slot() is how every other deferred
    // chroma entry point looks a layer up, and for the same reason.
    TextureDisplacementLayer *found = chroma_layer_in_slot(slot);
    if (found == nullptr)
        return;
    TextureDisplacementLayer &layer = *found;
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
    // The swaps sit at the height of the face the picture is painted on, which is what the base counts
    // up to. With nothing painted there is no such height and the base is only its default, so writing
    // them would put the picture's colours a few layers above the bed. The solve that follows the first
    // stroke measures the base, and the swaps are written then.
    if (!chroma_patch(layer).valid) {
        if (!silent)
            show_error(nullptr, _u8L("Paint the face the picture goes on first."));
        return;
    }
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

void GLGizmoTextureDisplacement::chroma_follow_face(TextureDisplacementLayer &layer)
{
    // A generated plate's base is its own number, kept on the Plate step (render_chroma_plate()).
    if (layer.chroma.plate.generated || !layer.chroma.base_auto)
        return;
    // The base is everything of the model under the painted face, so it changes with the paint - and
    // the paint changes on whichever step is open, which is why this is not left to the Plate step.
    const int measured = chroma_measured_base(layer);
    if (measured > 0 && measured != layer.chroma.base_layers) {
        layer.chroma.base_layers = measured;
        chroma_touch(layer.slot);
    }
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
    // The bands are the pick's whole answer: its layer height and base are the ones it was handed. So
    // only they are replaced. Taking the stack whole reset everything else on it to its defaults - the
    // light, the retouch, the importance mask, and the plate, whose `generated` going false turned a
    // picture's own plate into a picture on somebody's model, with that card's placement rows.
    layer.chroma.bands = std::move(picked.bands);
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

    if (!layer.chroma.plate.generated && !layer.chroma.base_auto) {
        const int measured = chroma_measured_base(layer);
        if (measured > 0 && layer.chroma.base_layers > measured)
            out.push_back(_u8L("The base is set by hand to more than this model has under the painted face. Every "
                               "filament swap is then written above the top of the object, so none of them happens "
                               "and the picture prints in one color. The padlock measures it again."));
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

// A row of mutually exclusive choices drawn as one control: every option is on screen and one click
// picks it, where a combo hides all but one behind a click and a second one. `current` is the index of
// the chosen one, and the row fills `total_w`.
//
// One frame round the whole row, with a rule between two options and the corners rounded only at its
// ends, in the frame colours every field and button on the card uses: a row of separate buttons with a
// gap between each read as a second kind of control beside them.
static bool chroma_segmented(const char *id, const std::vector<std::string> &labels, int &current, float total_w)
{
    if (labels.empty())
        return false;
    const ImGuiStyle &style   = ImGui::GetStyle();
    const float       frame_h = ImGui::GetFrameHeight();
    const float       round   = style.FrameRounding;
    const size_t      n       = labels.size();
    ImDrawList       *dl      = ImGui::GetWindowDrawList();
    // Each option as wide as its word, and what is left of the row shared out evenly: equal widths cut
    // the long option in a row of short ones ("Behind and in front" beside "Behind").
    std::vector<float> widths(n);
    float              natural = 0.f;
    for (size_t i = 0; i < n; ++i)
        natural += widths[i] = ImGui::CalcTextSize(labels[i].c_str()).x + 2.f * style.FramePadding.x;
    for (size_t i = 0; i < n; ++i)
        widths[i] = natural <= total_w ? widths[i] + (total_w - natural) / float(n) : widths[i] * total_w / natural;
    const ImVec2      p0      = ImGui::GetCursorScreenPos();
    const ImVec2      p1(p0.x + total_w, p0.y + frame_h);
    bool              changed = false;

    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_Button), round);
    ImGui::PushID(id);
    float x_at = p0.x;
    for (size_t i = 0; i < n; ++i) {
        const float x0 = std::round(x_at);
        x_at += widths[i];
        const float x1 = i + 1 == n ? p1.x : std::round(x_at);
        ImGui::SetCursorScreenPos(ImVec2(x0, p0.y));
        ImGui::PushID(int(i));
        if (ImGui::InvisibleButton("##seg", ImVec2(x1 - x0, frame_h)) && int(i) != current) {
            current = int(i);
            changed = true;
        }
        const bool        hot     = ImGui::IsItemHovered();
        const bool        on      = int(i) == current;
        const ImDrawFlags corners = n == 1     ? ImDrawFlags_RoundCornersAll :
                                    i == 0     ? ImDrawFlags_RoundCornersLeft :
                                    i + 1 == n ? ImDrawFlags_RoundCornersRight :
                                                 ImDrawFlags_RoundCornersNone;
        if (on || hot)
            dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y),
                              on ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) : ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                              round, corners);
        // The rule before an option, unless either side of it is the chosen one, whose fill is its edge.
        if (i > 0 && !on && int(i) - 1 != current)
            dl->AddLine(ImVec2(x0, p0.y + 1.f), ImVec2(x0, p1.y - 1.f), ImGui::GetColorU32(ImGuiCol_Border));
        const ImVec2 ts = ImGui::CalcTextSize(labels[i].c_str());
        dl->PushClipRect(ImVec2(x0, p0.y), ImVec2(x1, p1.y), true);
        dl->AddText(ImVec2(std::round(0.5f * (x0 + x1 - ts.x)), std::round(p0.y + 0.5f * (frame_h - ts.y))),
                    on ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text), labels[i].c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    ImGui::PopID();
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), round, ImDrawFlags_RoundCornersAll,
                std::max(1.f, style.FrameBorderSize));
    // One item for the whole row, so a tooltip asked for after the call is about the row.
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(total_w, frame_h));
    return changed;
}
static const ImVec4 CHROMA_COL_GOOD = ImVec4(0.39f, 0.69f, 0.51f, 1.f);

// A whole number with a minus and a plus, as one control framed like every field on the card: the
// count is changed a step at a time far more often than it is typed, and the field between the two
// still takes a typed one. Returns whether `v` changed.
static bool chroma_stepper(const char *id, int &v, int v_min, int v_max, float total_w)
{
    const ImGuiStyle &style   = ImGui::GetStyle();
    const float       frame_h = ImGui::GetFrameHeight();
    ImDrawList       *dl      = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    const int    before = v;
    const ImVec2 p0     = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + total_w, p0.y + frame_h);
    const float  bw     = frame_h;
    const float  round  = style.FrameRounding;
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), round);
    const auto end_button = [&](const char *bid, bool plus) {
        const ImVec2 b0  = ImGui::GetCursorScreenPos();
        const bool   hit = ImGui::InvisibleButton(bid, ImVec2(bw, frame_h));
        const bool   hot = ImGui::IsItemHovered();
        if (hot)
            dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + frame_h), ImGui::GetColorU32(ImGuiCol_ButtonHovered), round,
                              plus ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft);
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : 0.8f);
        const float a   = std::round(bw * 0.18f);
        const float cx = b0.x + 0.5f * bw, cy = b0.y + 0.5f * frame_h;
        dl->AddLine(ImVec2(cx - a, cy), ImVec2(cx + a + 1.f, cy), col, 1.5f);
        if (plus)
            dl->AddLine(ImVec2(cx + 0.5f, cy - a), ImVec2(cx + 0.5f, cy + a + 1.f), col, 1.5f);
        return hit;
    };
    if (end_button("##less", false) && v > v_min)
        --v;
    ImGui::SameLine(0.f, 0.f);
    const float       field_w = std::max(1.f, total_w - 2.f * bw);
    const std::string shown   = std::to_string(v);
    const float       pad_x   = std::max(style.FramePadding.x,
                                         std::floor(0.5f * (field_w - ImGui::CalcTextSize(shown.c_str()).x)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad_x, style.FramePadding.y));
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputScalar("##value", ImGuiDataType_S32, &v, nullptr, nullptr, "%d"))
        v = std::clamp(v, v_min, v_max);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.f, 0.f);
    if (end_button("##more", true) && v < v_max)
        ++v;
    const ImU32 edge = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddLine(ImVec2(p0.x + bw, p0.y + 1.f), ImVec2(p0.x + bw, p1.y - 1.f), edge);
    dl->AddLine(ImVec2(p1.x - bw, p0.y + 1.f), ImVec2(p1.x - bw, p1.y - 1.f), edge);
    dl->AddRect(p0, p1, edge, round, ImDrawFlags_RoundCornersAll, std::max(1.f, style.FrameBorderSize));
    // One item for the whole control, so a tooltip asked for after the call is about it.
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(total_w, frame_h));
    ImGui::PopID();
    return v != before;
}

// The average colour difference under which a match is good, and over which the filaments cannot reach
// the picture. Between the two it is close enough, and said in neither colour.
static constexpr double CHROMA_MATCH_GOOD = 5.;
static constexpr double CHROMA_MATCH_FAR  = 15.;

// A filament as the lists on the card name it: its slot and its name, or the slot alone when it has none.
static std::string chroma_filament_label(const ChromaFilament &f)
{
    return f.name.empty() ? Slic3r::format(_u8L("Filament %1%"), f.filament + 1) :
                            Slic3r::format("%1%  %2%", f.filament + 1, f.name);
}

// How light a colour reads, for picking text and marks that stand out against it.
static float chroma_luma(const ColorRGB &c) { return 0.2126f * c.r() + 0.7152f * c.g() + 0.0722f * c.b(); }

// The red a cross that takes something out turns under the pointer, and the cross itself.
static constexpr ImU32 CHROMA_REMOVE_HOT = IM_COL32(224, 89, 75, 255);
static void chroma_draw_cross(ImDrawList *dl, const ImVec2 &c, float a, ImU32 col)
{
    dl->AddLine(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), col, 1.6f);
    dl->AddLine(ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y - a), col, 1.6f);
}

// A popup of `width` beside the panel rather than over it, its top level with the cursor: a list read
// against the rows it belongs to must not cover them.
static void chroma_place_beside_panel(float width, float gap)
{
    const ImVec2 panel = ImGui::GetWindowPos();
    ImGui::SetNextWindowPos(ImVec2(std::max(0.f, panel.x - width - gap), ImGui::GetCursorScreenPos().y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.f));
}

// The colour a match of this average difference is said in, or null between the two thresholds.
static const ImVec4 *chroma_match_col(double mean)
{
    return mean > CHROMA_MATCH_FAR ? &CHROMA_COL_WARN : mean <= CHROMA_MATCH_GOOD ? &CHROMA_COL_GOOD : nullptr;
}

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
    // A filter that is only being tried lives in this panel and nowhere else: a bake would print it,
    // and the project would reopen on the picture without it.
    if (!chroma_live_adjust(layer).identity()) {
        out.color = CHROMA_COL_WARN;
        out.warn  = true;
        out.text  = _u8L("Keep or undo the filter before baking.");
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
    if (report.mean > CHROMA_MATCH_FAR) {
        out.color = CHROMA_COL_WARN;
        out.warn  = true;
        out.text  = _u8L("Your filaments cannot reach these colors.");
        return out;
    }
    out.color = CHROMA_COL_GOOD;
    out.text  = report.mean <= CHROMA_MATCH_GOOD ? _u8L("Good match. Ready to bake.") : _u8L("Close enough. Ready to bake.");
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

    // The same control for a count. Replaces a slider for a count nobody drags: picking 4 out of five
    // options is one click, where a slider is a drag and a read-back.
    const auto segmented = [&](const char *id, const std::vector<int> &values, int &current, float total_w) {
        std::vector<std::string> labels;
        int                      index = -1;
        for (size_t i = 0; i < values.size(); ++i) {
            labels.push_back(std::to_string(values[i]));
            if (values[i] == current)
                index = int(i);
        }
        if (!chroma_segmented(id, labels, index, total_w) || index < 0)
            return false;
        current = values[size_t(index)];
        return true;
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

    // Under the filter being tried, so "Your picture" is the picture the solve is reading. The filter is
    // applied to the thumbnail rather than to the picture, as the filter row's own thumbnails are: a
    // slider being dragged changes it every frame, and a full-size pass per frame on this thread stalls
    // the panel for as long as the drag lasts.
    if (const TextureAdjustments adj = chroma_live_adjust(layer);
        m_chroma_source_key[slot] != layer.chroma_source.get() || m_chroma_source_adjust[slot] != adj) {
        const DecodedHeightTexture &thumb_src = chroma_thumb_base(layer);
        m_chroma_source_thumb[slot] = upload_height_thumbnail(adj.identity() || thumb_src.empty() ? thumb_src :
                                                                                                apply_texture_adjustments(thumb_src, adj));
        m_chroma_source_key[slot]    = layer.chroma_source.get();
        m_chroma_source_adjust[slot] = adj;
    }
    const ChromaReport &report  = m_chroma_report[slot];
    // Waiting for the controls to settle, or actually computing. The ring and the tooltip mean the
    // same thing to the user either way: what is on screen is the previous answer, and a better one
    // is on its way.
    const bool          solving = m_chroma_settle_at[slot] != 0 || m_chroma_solve_running[slot] ||
                                  m_chroma_plate_running[slot];
    const ImVec4       *match   = report.valid ? chroma_match_col(report.mean) : nullptr;
    const ImVec4        ring    = solving         ? ImGuiWrapper::COL_ORCA :
                                  match != nullptr ? *match :
                                                     ImGui::GetStyleColorVec4(ImGuiCol_Separator);
    // While the error view is on, the print's half shows the error rather than the prediction, so the
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
        chip_col = match != nullptr ? match : &chip_plain;
    }

    // ---- The picture and what it prints, as one image ----
    //
    // The picture on the left of a divider and the print on the right, at the picture's own shape: a
    // picture squeezed into a square is not the picture, and two side by side at half the width each
    // made the one judgement this card exists for - is that the face - on two 190 px thumbnails. Dragging
    // the divider walks the comparison across the image; a double click opens the pair at the size of
    // the canvas.
    //
    // Sized from the width the card is allowed, never from the width it has left: a size taken from the
    // live region ties the panel's height to its width, and the panel flickers between two widths as a
    // scrollbar comes and goes.
    {
        const GLTexture *src    = m_chroma_source_thumb[slot].get();
        const GLTexture *out    = error_pane ? m_chroma_error_tex[slot].get() : m_chroma_preview[slot].get();
        const GLTexture *shape  = src != nullptr ? src : out;
        const float      aspect = shape != nullptr && shape->get_height() > 0 ?
                                      float(shape->get_width()) / float(shape->get_height()) : 1.f;
        const float      full_w = std::max(m_imgui->scaled(4.f), content_w);
        // No taller than a wide photograph is at this width: the steps under it are where the work is
        // done, and an image that fills the panel pushes them off it. A taller picture is narrowed to
        // keep its shape rather than cropped.
        float view_w = full_w;
        float view_h = std::round(view_w / std::max(0.05f, aspect));
        if (const float max_h = std::round(full_w * 0.6f); view_h > max_h) {
            view_h = max_h;
            view_w = std::round(view_h * aspect);
        }
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x0 + std::round(0.5f * (full_w - view_w)));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + view_w, p0.y + view_h);
        ImGui::InvisibleButton("##chroma_compare", ImVec2(view_w, view_h));
        const bool hot = ImGui::IsItemHovered();
        float     &split = m_chroma_split[slot];
        if (ImGui::IsItemActive())
            split = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / std::max(1.f, view_w), 0.f, 1.f);
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && out != nullptr)
            m_chroma_zoom_slot = layer.slot;
        const float sx = std::round(p0.x + split * view_w);

        if (src != nullptr)
            dl->AddImage((ImTextureID) (intptr_t) src->get_id(), p0, ImVec2(sx, p1.y), ImVec2(0.f, 0.f), ImVec2(split, 1.f));
        if (out != nullptr)
            dl->AddImage((ImTextureID) (intptr_t) out->get_id(), ImVec2(sx, p0.y), p1, ImVec2(split, 0.f), ImVec2(1.f, 1.f));
        else
            dl->AddRectFilled(ImVec2(sx, p0.y), p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
        // How good the match is, as the colour of the image's own frame.
        dl->AddRect(ImVec2(p0.x - 1.f, p0.y - 1.f), ImVec2(p1.x + 1.f, p1.y + 1.f), ImGui::GetColorU32(ring), 0.f, 0, 2.f);

        // The divider and its handle.
        const ImU32 white = IM_COL32(255, 255, 255, 235);
        dl->AddLine(ImVec2(sx, p0.y), ImVec2(sx, p1.y), white, 2.f);
        const float  r = std::round(frame_h * 0.48f);
        const ImVec2 c(sx, std::round(0.5f * (p0.y + p1.y)));
        dl->AddCircleFilled(c, r, white, 24);
        const ImU32 dark = IM_COL32(40, 40, 44, 255);
        const float a    = std::round(r * 0.32f);
        dl->AddTriangleFilled(ImVec2(c.x - 0.35f * r - a, c.y), ImVec2(c.x - 0.35f * r, c.y - a), ImVec2(c.x - 0.35f * r, c.y + a), dark);
        dl->AddTriangleFilled(ImVec2(c.x + 0.35f * r + a, c.y), ImVec2(c.x + 0.35f * r, c.y + a), ImVec2(c.x + 0.35f * r, c.y - a), dark);

        // What each side is, and the match in figures, on the image rather than under it.
        const float pad = std::round(m_imgui->scaled(0.25f));
        const auto  tag = [&](const std::string &text, const ImVec2 &at, ImU32 bg, ImU32 fg) {
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            dl->AddRectFilled(at, ImVec2(at.x + ts.x + 2.f * pad, at.y + ts.y + 2.f * pad), bg, ImGui::GetStyle().FrameRounding);
            dl->AddText(ImVec2(at.x + pad, at.y + pad), fg, text.c_str());
            return ts.x + 2.f * pad;
        };
        const ImU32       shade = IM_COL32(20, 20, 24, 200);
        const std::string left  = _u8L("Your picture");
        const std::string right = error_pane ? _u8L("Where it is wrong") : _u8L("What prints");
        tag(left, ImVec2(p0.x + pad, p0.y + pad), shade, IM_COL32(235, 235, 235, 255));
        tag(right, ImVec2(p1.x - pad - ImGui::CalcTextSize(right.c_str()).x - 2.f * pad, p0.y + pad), shade,
            IM_COL32(235, 235, 235, 255));
        if (!chip.empty() && chip_col != nullptr)
            tag(chip, ImVec2(p1.x - pad - ImGui::CalcTextSize(chip.c_str()).x - 2.f * pad,
                             p1.y - pad - ImGui::GetTextLineHeight() - 2.f * pad),
                ImGui::GetColorU32(*chip_col), IM_COL32(20, 20, 20, 255));

        if (hot) {
            const std::string said =
                error_pane    ? _u8L("How far this stack lands from the picture, pixel by pixel: dark where it "
                                     "matches, bright where the filaments cannot reach it. Turn the error view "
                                     "off under Check to see the predicted print again.") :
                solving       ? _u8L("Working out the heights for this stack.") :
                !report.valid ? _u8L("What this stack is predicted to print.") :
                                Slic3r::format(_u8L("What this stack is predicted to print, %1$.1f from the picture "
                                                    "on average and %2$.1f at worst, as a color difference.\n\n"
                                                    "Under 5 is a good match. Over 15 means these filaments cannot "
                                                    "reach the picture's colors and more layers will not help: change "
                                                    "a filament, or ask for a different number of colors."),
                                               report.mean, report.max);
            panel_tooltip(said + "\n\n" + _u8L("Drag across the picture to compare. Double-click to see both at the size of the canvas."));
        }
        ImGui::SetCursorPosX(x0);
        ImGui::Dummy(ImVec2(1.f, gap_s));
    }

    // ---- What is wrong, when something is ----
    //
    // One bar, and only when there is something to say: the reason this cannot be baked yet, the
    // warning about the match, or the worst problem chroma_problems() found, in a sentence, with a
    // count of the rest whose tooltip is all of them. A card with nothing wrong says nothing here -
    // "Ready to bake." is on the Check step, which is where that question is asked - and the solve
    // running is said by the chip on the image, not by a bar that would come and go with every edit.
    //
    // Bake's reason for being disabled is the same sentence from the same function (chroma_readiness()):
    // two readings of one state are two things that can come to disagree.
    //
    // Worked out once for the frame, along with where its sentence goes: in the bar when it is a reason
    // not to bake or a warning (a ready card can still carry one - the filaments cannot reach the
    // picture), and on the Check step otherwise. One flag, so the two places cannot both say it or
    // both leave it out.
    const ChromaReadiness state = chroma_readiness(layer);
    const bool            alarm = !solving && (!state.ready || state.warn);
    {
        const std::vector<std::string> problems = chroma_problems(layer);
        std::string said;
        size_t      rest = problems.size();
        if (alarm)
            said = state.text;
        else if (!problems.empty()) {
            said = problems.front();
            rest = problems.size() - 1;
        }
        const bool swaps = chroma_swaps_blocked(layer);
        if (said.empty() && swaps)
            said = _u8L("The object already has filament swaps that this picture did not write.");
        if (!said.empty()) {
            const float  x0    = ImGui::GetCursorPosX();
            const ImVec2 p0    = ImGui::GetCursorScreenPos();
            const float  w     = std::max(m_imgui->scaled(4.f), ImGui::GetContentRegionMax().x - x0 - std::max(card_pad, 1.f));
            const float  h     = std::round(frame_h * 1.3f);
            const float  pad   = std::round(m_imgui->scaled(0.35f));
            const ImU32  tint  = ImGui::GetColorU32(ImVec4(CHROMA_COL_WARN.x, CHROMA_COL_WARN.y, CHROMA_COL_WARN.z, 0.16f));
            const ImU32  edge  = ImGui::GetColorU32(ImVec4(CHROMA_COL_WARN.x, CHROMA_COL_WARN.y, CHROMA_COL_WARN.z, 0.55f));
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), tint, style.FrameRounding);
            dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), edge, style.FrameRounding);

            // What is on its right, measured first so the sentence knows how much room it has.
            const std::string act   = _u8L("Replace them");
            const float       act_w = swaps ? ImGui::CalcTextSize(act.c_str()).x + 2.f * style.FramePadding.x : 0.f;
            const std::string more  = rest > 0 ? Slic3r::format("+%1%", int(rest)) : std::string();
            const float       more_w = more.empty() ? 0.f : ImGui::CalcTextSize(more.c_str()).x + pad;

            // The mark, knocked out rather than drawn over, so it reads at the size a high-DPI panel gets.
            const float a  = std::round(frame_h * 0.28f);
            const float cx = p0.x + pad + a, cy = p0.y + 0.5f * h;
            dl->AddTriangleFilled(ImVec2(cx, cy - a), ImVec2(cx + a, cy + 0.75f * a), ImVec2(cx - a, cy + 0.75f * a),
                                  ImGui::GetColorU32(CHROMA_COL_WARN));
            const ImU32 hole = ImGui::GetColorU32(ImGuiCol_WindowBg);
            dl->AddRectFilled(ImVec2(cx - 0.11f * a, cy - 0.35f * a), ImVec2(cx + 0.11f * a, cy + 0.2f * a), hole);
            dl->AddRectFilled(ImVec2(cx - 0.11f * a, cy + 0.38f * a), ImVec2(cx + 0.11f * a, cy + 0.6f * a), hole);

            const float       tx     = cx + a + pad;
            const float       room   = std::max(frame_h, p0.x + w - pad - act_w - (swaps ? pad : 0.f) - more_w - tx);
            const std::string shown  = ellipsize(said, room);
            const float       ty     = std::round(cy - 0.5f * ImGui::GetTextLineHeight());
            dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
            if (!more.empty())
                dl->AddText(ImVec2(tx + ImGui::CalcTextSize(shown.c_str()).x + pad, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            more.c_str());

            // The sentence in full and everything else wrong, on the bar's own tooltip.
            ImGui::InvisibleButton("##chroma_problem", ImVec2(std::max(1.f, w - act_w - (swaps ? pad : 0.f)), h));
            if (ImGui::IsItemHovered()) {
                const bool  first_shown = !problems.empty() && said == problems.front();
                std::string all         = said;
                for (size_t i = first_shown ? 1 : 0; i < problems.size(); ++i)
                    all += "\n\n" + problems[i];
                panel_tooltip(all);
            }
            // The one action a problem can carry: swaps that did not come from this picture are the one
            // thing here the card itself can put right.
            if (swaps) {
                ImGui::SetCursorScreenPos(ImVec2(p0.x + w - pad - act_w, p0.y + 0.5f * (h - frame_h)));
                if (ImGui::Button(act.c_str(), ImVec2(act_w, frame_h)))
                    chroma_apply_swaps(layer);
                if (ImGui::IsItemHovered())
                    panel_tooltip(_u8L("Writes this picture's own filament swaps over the height ranges the object "
                                       "already had."));
            }
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
            ImGui::Dummy(ImVec2(1.f, gap_s));
        }
    }

    // ---- The steps ----
    //
    // The work in the order it is done: the picture, the colours it is printed in, the plate and the
    // light it is seen in, and checking what came out. One step is on the card at a time, so the card
    // is as tall as the step rather than as tall as all four, and the previews above it - which every
    // step changes - stay where they are while the rows under them change.
    const int step = std::clamp(m_chroma_step[slot], 0, int(ChromaStepCount) - 1);
    {
        // TRN The four steps of a ChromaStack card, in the order the work is done. Short: four of them share
        // the width of the panel, each beside an icon.
        const std::string names[] = { _u8L("Picture"), _u8L("Colors"), _u8L_CONTEXT("Plate", "ChromaStack step"),
                                      _u8L_CONTEXT("Check", "ChromaStack step") };
        const float       row_x   = ImGui::GetCursorPosX();
        const float       row_w   = std::max(m_imgui->scaled(4.f),
                                             ImGui::GetContentRegionMax().x - row_x - std::max(card_pad, 1.f));
        const float       tab_w   = std::floor(row_w / float(ChromaStepCount));
        const float       tab_h   = std::round(frame_h * 1.35f);
        const float       ico     = std::round(frame_h * 0.55f);
        const ImVec2      origin  = ImGui::GetCursorScreenPos();
        // Check carries the card's state as a dot, the same colour as the line above: it is the step
        // whose whole job is that question.
        const ImVec4      check   = state.color;
        // Each step's icon, drawn rather than taken from a font so it keeps its weight when the panel is
        // scaled: a picture, a stack of filament, a slab, a tick.
        const auto icon = [&](int s, const ImVec2 &c, ImU32 col) {
            const float h = 0.5f * ico, w = 0.62f * ico, line = std::max(1.2f, std::round(frame_h * 0.06f));
            if (s == ChromaStepPicture) {
                dl->AddRect(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), col, 1.f, 0, line);
                dl->AddTriangleFilled(ImVec2(c.x - w + line, c.y + h - line), ImVec2(c.x - 0.15f * w, c.y - 0.1f * h),
                                      ImVec2(c.x + 0.4f * w, c.y + h - line), col);
                dl->AddCircleFilled(ImVec2(c.x + 0.45f * w, c.y - 0.35f * h), 0.16f * ico, col);
            } else if (s == ChromaStepColors) {
                for (int k = -1; k <= 1; ++k)
                    dl->AddRectFilled(ImVec2(c.x - w, c.y + float(k) * 0.66f * h - 0.22f * h),
                                      ImVec2(c.x + w, c.y + float(k) * 0.66f * h + 0.22f * h), col, 1.f);
            } else if (s == ChromaStepPlate) {
                const float d = 0.35f * w;
                dl->AddQuad(ImVec2(c.x - w, c.y - 0.1f * h), ImVec2(c.x - w + d, c.y - h), ImVec2(c.x + w, c.y - h),
                            ImVec2(c.x + w - d, c.y - 0.1f * h), col, line);
                dl->AddRect(ImVec2(c.x - w, c.y - 0.1f * h), ImVec2(c.x + w - d, c.y + h), col, 0.f, 0, line);
            } else {
                dl->AddLine(ImVec2(c.x - w, c.y), ImVec2(c.x - 0.25f * w, c.y + 0.75f * h), col, 1.4f * line);
                dl->AddLine(ImVec2(c.x - 0.25f * w, c.y + 0.75f * h), ImVec2(c.x + w, c.y - 0.8f * h), col, 1.4f * line);
            }
        };
        for (int s = 0; s < int(ChromaStepCount); ++s) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + float(s) * tab_w, origin.y));
            ImGui::PushID(s);
            if (ImGui::InvisibleButton("##chroma_step", ImVec2(tab_w, tab_h)) && s != step)
                chroma_set_step(layer, s);
            const bool  hot = ImGui::IsItemHovered();
            const bool  on  = s == step;
            const ImU32 ink = on ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_Text, hot ? 0.85f : 0.55f);
            const ImVec2 ts = ImGui::CalcTextSize(names[s].c_str());
            // The icon and the word together, centred in the tab.
            const float  body = ico * 1.24f + gap_s + ts.x;
            const float  bx   = std::round(origin.x + float(s) * tab_w + 0.5f * (tab_w - body));
            const float  cy   = std::round(origin.y + 0.5f * tab_h - 1.f);
            icon(s, ImVec2(bx + 0.62f * ico, cy), on ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) : ink);
            dl->AddText(ImVec2(bx + ico * 1.24f + gap_s, std::round(cy - 0.5f * ts.y)), ink, names[s].c_str());
            if (s == int(ChromaStepCheck)) {
                const float r = std::max(2.f, std::round(frame_h * 0.09f));
                dl->AddCircleFilled(ImVec2(std::round(bx + body + 2.f * r + 1.f), std::round(cy - 0.3f * ts.y)), r,
                                    ImGui::GetColorU32(check));
            }
            // The step's own underline, across the whole row so the four read as one control, and a short
            // rule between two steps so each reads as a place of its own.
            const float y = origin.y + tab_h - 1.f;
            dl->AddLine(ImVec2(origin.x + float(s) * tab_w, y), ImVec2(origin.x + float(s + 1) * tab_w, y),
                        on ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) : ImGui::GetColorU32(ImGuiCol_Separator),
                        on ? 2.f : 1.f);
            if (s > 0) {
                const float x = std::round(origin.x + float(s) * tab_w);
                dl->AddLine(ImVec2(x, origin.y + 0.25f * tab_h), ImVec2(x, origin.y + 0.75f * tab_h),
                            ImGui::GetColorU32(ImGuiCol_Separator));
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + tab_h));
        ImGui::Dummy(ImVec2(1.f, gap_s));
    }

    // ---- 1 Picture ----
    //
    // Which picture, and what is done to it before it is solved. The filters come first because they
    // are what most pictures want and nobody has to understand a slider to use one.
    if (step == ChromaStepPicture) {
        const float full = std::max(m_imgui->scaled(4.f), ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f));
        // The picture's name in a field, and the gallery it comes from beside it. The field is not
        // typed into: a picture is chosen by looking at it, so clicking the name opens the gallery too.
        {
            const float       x0      = ImGui::GetCursorPosX();
            slider_label(_L("Picture"));
            const std::string gallery = _u8L("Gallery");
            const float       gal_w   = ImGui::CalcTextSize(gallery.c_str()).x + 2.f * style.FramePadding.x;
            const float       field_w = std::max(frame_h, full - (ImGui::GetCursorPosX() - x0) - gal_w - gap_s);
            const ImVec2      p       = ImGui::GetCursorScreenPos();
            const std::string tip     = _u8L("Picks a different picture from the gallery. The stack and the placement stay "
                                             "as they are, a plate made for the picture takes the new one's shape, and the "
                                             "stack is solved again for it.");
            if (ImGui::InvisibleButton("##chroma_picture_name", ImVec2(field_w, frame_h)))
                chroma_pick_picture(layer);
            const bool hot = ImGui::IsItemHovered();
            dl->AddRectFilled(p, ImVec2(p.x + field_w, p.y + frame_h),
                              ImGui::GetColorU32(hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), style.FrameRounding);
            dl->AddRect(p, ImVec2(p.x + field_w, p.y + frame_h), ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding,
                        ImDrawFlags_RoundCornersAll, std::max(1.f, style.FrameBorderSize));
            const std::string name = ellipsize(layer.name.empty() ? _u8L("Picture") : layer.name,
                                               field_w - 2.f * style.FramePadding.x);
            dl->AddText(ImVec2(p.x + style.FramePadding.x, p.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text),
                        name.c_str());
            if (hot)
                panel_tooltip(tip);
            ImGui::SameLine(0.f, gap_s);
            if (ImGui::Button(gallery.c_str(), ImVec2(gal_w, frame_h)))
                chroma_pick_picture(layer);
            if (ImGui::IsItemHovered())
                panel_tooltip(tip);
        }
        render_chroma_filters(layer, full);
    }

    // ---- 4 Check ----
    //
    // Whether it is ready, why it looks the way it does, and the one correction that is not a change to
    // the whole picture. The brush's settings are always on the step rather than appearing when it is
    // switched on: a control that appears has moved everything below it while the reader was looking
    // somewhere else.
    if (step == ChromaStepCheck) {
        const float x0    = ImGui::GetCursorPosX();
        const float row_w = std::max(m_imgui->scaled(4.f), ImGui::GetContentRegionMax().x - x0 - std::max(card_pad, 1.f));
        const float right = x0 + row_w;

        // Whether it is ready. Only "ready" and "still working" are said here: anything wrong is already
        // the bar above the steps, and the same sentence twice in two places reads as two things.
        {
            if (!alarm) {
                const float r = std::max(2.5f, std::round(frame_h * 0.11f));
                ImGui::AlignTextToFramePadding();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(2.f * r + gap_s, frame_h));
                dl->AddCircleFilled(ImVec2(p.x + r, p.y + 0.5f * frame_h), r, ImGui::GetColorU32(state.color));
                ImGui::SameLine(0.f, gap_s);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, state.color);
                ImGui::TextUnformatted(state.text.c_str());
                ImGui::PopStyleColor();
            }
        }

        // ---- Why it looks the way it does ----
        //
        // The colour analysis on the step itself, small: the picture's colours as a cloud and the stack's
        // reach as a line through it, which says at a glance whether the filaments can get there. A click
        // opens it at the size of the canvas, with the figures under it.
        {
            const ChromaDiagnostics &diag = m_chroma_diag[slot];
            heading(_L("Color analysis"), diag.empty() ? std::string() : Slic3r::format(_u8L("%1$.1f off"), diag.mean_de),
                    card_pad);
            // Its height from the width the card is allowed, never from the width it has left: taken from
            // the live region it ties the card's height to the scrollbar, which then comes and goes every
            // frame as the chart shrinks under it and grows back.
            const float  ch  = std::round(std::max(m_imgui->scaled(4.f), content_w) * 0.34f);
            const ImVec2 cp  = ImGui::GetCursorScreenPos();
            const bool   hit = ImGui::InvisibleButton("##chroma_diag_small", ImVec2(row_w, ch));
            const bool   hot = ImGui::IsItemHovered() && !diag.empty();
            if (diag.empty()) {
                dl->AddRectFilled(cp, ImVec2(cp.x + row_w, cp.y + ch), ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
                const std::string wait = _u8L("It appears once the stack has been solved.");
                const ImVec2      ts   = ImGui::CalcTextSize(wait.c_str());
                dl->AddText(ImVec2(std::round(cp.x + 0.5f * (row_w - ts.x)), std::round(cp.y + 0.5f * (ch - ts.y))),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), wait.c_str());
            } else {
                draw_chroma_diag_plot(dl, diag, cp, row_w, ch, 0.55f);
                if (hit)
                    m_chroma_diag_slot = layer.slot;
            }
            dl->AddRect(cp, ImVec2(cp.x + row_w, cp.y + ch),
                        hot ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) : ImGui::GetColorU32(ImGuiCol_Border),
                        style.FrameRounding, ImDrawFlags_RoundCornersAll, std::max(1.f, style.FrameBorderSize));
            if (hot)
                panel_tooltip(_u8L("The picture's colors as dots, each as large as its share of the picture, and what "
                                   "this stack can print as the line through them: hue across, lightness up. Where the "
                                   "dots sit far from the line, these filaments cannot reach those colors.\n\n"
                                   "Click to see it larger, with the figures under it."));
        }

        // ---- Fixing what the solve got wrong ----
        //
        // Everything else on this card changes the whole picture to put one part of it right. This does
        // not: a stroke over the print says what that area should have been, and the solve honours it
        // there and nowhere else.
        ImGui::Dummy(ImVec2(1.f, gap_s));
        {
            // The heading, and painting switched on and off at its right: while it is on, dragging over
            // the model paints instead of turning it.
            const bool        on    = m_chroma_retouch_mode;
            const std::string label = on ? _u8L("Done") : _u8L("Paint");
            const float       bw    = ImGui::CalcTextSize(label.c_str()).x + 4.f * style.FramePadding.x;
            ImGui::AlignTextToFramePadding();
            m_imgui->push_bold_font();
            ImGui::TextUnformatted(_u8L("Fix an area").c_str());
            m_imgui->pop_bold_font();
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - bw));
            if (on)
                GLGizmoUtils::push_orca_button_style();
            if (ImGui::Button(label.c_str(), ImVec2(bw, frame_h))) {
                m_chroma_retouch_mode = !on;
                if (!m_chroma_retouch_mode && m_chroma_retouch_painting)
                    chroma_commit_stroke(layer);
                m_parent.set_as_dirty();
            }
            if (on)
                GLGizmoUtils::pop_orca_button_style();
            if (ImGui::IsItemHovered())
                panel_tooltip(on ? _u8L("Stops the brush, so dragging turns the model again.") :
                                   _u8L("Paint over the print to correct it: the area you paint moves to the color you "
                                        "choose here, and the rest of the picture is left exactly as the solve decided."
                                        "\n\nHold Ctrl to turn the model while the brush is on."));
        }

        // The brush's settings and the corrections made with it, in one frame.
        const float  pad  = std::round(m_imgui->scaled(0.35f));
        const ImVec2 top  = ImGui::GetCursorScreenPos();
        const float  in_w = row_w - 2.f * pad;
        ImGui::Dummy(ImVec2(1.f, pad));
        ImGui::Indent(pad);
        {
            const float ix0 = ImGui::GetCursorPosX();
            // Three words rather than a combo: three is the whole choice.
            if (chroma_segmented("##hf_fix_kind", { _u8L("Layers"), _u8L("Filament"), _u8L("Matters") },
                               m_chroma_retouch_kind, in_w))
                m_parent.set_as_dirty();
            if (ImGui::IsItemHovered())
                panel_tooltip(_u8L("What the brush does to what it covers. Moving by layers nudges that area up or "
                                   "down the stack, which is the gentler correction and keeps the shading. Using a "
                                   "filament sets the area to the top of one band, which is what a flat area of one "
                                   "color wants. Marking what matters changes no height: it tells the solve which "
                                   "part of the picture to choose the filaments for."));

            // Two rows, always these two: the control on the first changes kind with the mode, but
            // neither its place nor its width does, so nothing under it moves when the mode is changed.
            // The clear mark at its end belongs to Matters alone, and keeps its room in the other two.
            const float label_w = m_imgui->scaled(4.2f);
            const float clear_w = frame_h;
            const float field_w = std::max(m_imgui->scaled(3.f), in_w - label_w - clear_w - gap_s);
            ImGui::SetCursorPosX(ix0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", m_chroma_retouch_kind == 0 ? _u8L("Amount").c_str() :
                                      m_chroma_retouch_kind == 1 ? _u8L("Take it to").c_str() :
                                                                   _u8L("Weight").c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(ix0 + label_w);
            if (m_chroma_retouch_kind == 0) {
                chroma_stepper("##hf_fix_delta", m_chroma_retouch_delta, -CHROMA_MAX_RELIEF_LAYERS, CHROMA_MAX_RELIEF_LAYERS, field_w);
                if (ImGui::IsItemHovered())
                    panel_tooltip(_u8L("How many printed layers to move the area, up for a later color and down for "
                                       "an earlier one. One layer is usually the whole correction."));
            } else if (m_chroma_retouch_kind == 1) {
                ImGui::SetNextItemWidth(field_w);
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
            ImGui::SameLine(0.f, gap_s);
            if (m_chroma_retouch_kind == 2) {
                const bool   can_clear = !layer.chroma.importance.region.empty();
                const ImVec2 cp        = ImGui::GetCursorScreenPos();
                m_imgui->disabled_begin(!can_clear);
                if (ImGui::InvisibleButton("##hf_fix_imp_clear", ImVec2(clear_w, frame_h)) && can_clear) {
                    layer.chroma.importance = ChromaImportance{};
                    chroma_touch(layer.slot);
                }
                m_imgui->disabled_end();
                const bool  hot = ImGui::IsItemHovered() && can_clear;
                chroma_draw_cross(dl, ImVec2(cp.x + 0.5f * clear_w, cp.y + 0.5f * frame_h), std::round(clear_w * 0.2f),
                                  hot ? CHROMA_REMOVE_HOT : ImGui::GetColorU32(ImGuiCol_Text, can_clear ? 0.6f : 0.25f));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    panel_tooltip(can_clear ? _u8L("Forgets the marked area, so the whole picture counts equally again.") :
                                              _u8L("Forgets the marked area. Nothing is marked yet."));
            } else
                ImGui::Dummy(ImVec2(clear_w, frame_h));

            ImGui::SetCursorPosX(ix0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("Brush").c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(ix0 + label_w);
            if (typed_slider("##hf_fix_radius", field_w + gap_s + clear_w - panel_edit_icon_room(), &m_chroma_retouch_radius_mm,
                             0.5f, 25.f, "%.1f mm", false,
                             _L("How wide the brush is, measured on the print rather than on the screen, so it "
                                "covers the same amount of the picture however far you have zoomed in.")))
                m_parent.set_as_dirty();

            // The corrections, newest last, each one a stroke: a name, whether it is on, and taking it
            // out. Four on screen and then a scrollbar, so a long session of retouching cannot push the
            // picture it is about off the top of the panel.
            std::vector<ChromaRetouchEdit> &edits   = layer.chroma.retouch.edits;
            int                             drop    = -1;
            const bool                      scrolls = edits.size() > 4;
            if (scrolls) {
                push_slim_scrollbar_style(m_imgui->scaled(0.5f), wxGetApp().dark_mode());
                ImGui::BeginChild("##hf_fix_list", ImVec2(in_w, 4.f * frame_h + 3.f * style.ItemSpacing.y), false);
            }
            const float icon_w = std::round(frame_h * 0.85f);
            for (size_t ei = 0; ei < edits.size(); ++ei) {
                ChromaRetouchEdit &edit = edits[ei];
                ImGui::PushID(int(1000 + ei));
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
                ImGui::SetCursorPosX(ix0);
                ImGui::AlignTextToFramePadding();
                if (edit.enabled)
                    ImGui::TextUnformatted(said.c_str());
                else
                    ImGui::TextDisabled("%s", said.c_str());
                // On or off, as an eye; then the cross that takes it out.
                ImGui::SameLine();
                ImGui::SetCursorPosX(ix0 + in_w - 2.f * icon_w - gap_s - (scrolls ? m_imgui->scaled(0.5f) : 0.f));
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##hf_fix_on", ImVec2(icon_w, frame_h))) {
                        edit.enabled = !edit.enabled;
                        chroma_touch(layer.slot);
                    }
                    const bool  hot = ImGui::IsItemHovered();
                    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : edit.enabled ? 0.75f : 0.35f);
                    const float cx = p.x + 0.5f * icon_w, cy = p.y + 0.5f * frame_h, ew = 0.36f * icon_w;
                    dl->PathArcTo(ImVec2(cx, cy + 0.9f * ew), 1.25f * ew, -2.36f, -0.78f, 10);
                    dl->PathArcTo(ImVec2(cx, cy - 0.9f * ew), 1.25f * ew, 0.78f, 2.36f, 10);
                    dl->PathStroke(col, ImDrawFlags_Closed, 1.3f);
                    if (edit.enabled)
                        dl->AddCircleFilled(ImVec2(cx, cy), 0.32f * ew, col);
                    else
                        dl->AddLine(ImVec2(cx - ew, cy + 0.7f * ew), ImVec2(cx + ew, cy - 0.7f * ew), col, 1.3f);
                    if (hot)
                        panel_tooltip(edit.enabled ? _u8L("Turns this correction off without losing it, so you can see what it is doing.") :
                                                     _u8L("Turns this correction back on."));
                }
                ImGui::SameLine(0.f, gap_s);
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##hf_fix_del", ImVec2(icon_w, frame_h)))
                        drop = int(ei);
                    const bool  hot = ImGui::IsItemHovered();
                    chroma_draw_cross(dl, ImVec2(p.x + 0.5f * icon_w, p.y + 0.5f * frame_h), std::round(icon_w * 0.22f),
                                      hot ? CHROMA_REMOVE_HOT : ImGui::GetColorU32(ImGuiCol_Text, 0.6f));
                    if (hot)
                        panel_tooltip(_u8L("Removes this correction."));
                }
                ImGui::PopID();
            }
            if (scrolls) {
                ImGui::EndChild();
                pop_slim_scrollbar_style();
            }
            if (drop >= 0) {
                edits.erase(edits.begin() + drop);
                chroma_touch(layer.slot);
            }
        }
        ImGui::Unindent(pad);
        ImGui::Dummy(ImVec2(1.f, pad - style.ItemSpacing.y));
        dl->AddRect(top, ImVec2(top.x + row_w, ImGui::GetCursorScreenPos().y - style.ItemSpacing.y),
                    ImGui::GetColorU32(ImGuiCol_Separator), style.FrameRounding);
    }

    // ---- The colours the picture is printed in ----
    //
    // The ramp first, because it is the whole stack's own readout and answers "is this worth printing"
    // before any one band is worth looking at; then the bands, top of the stack first, the way the
    // print is looked at; then the two ways of choosing them again from scratch.
    // Whether the stack prints anything above its base, which Plate needs as well. Read off the stack
    // rather than off its palette: chroma_palette() is empty exactly when one of these fails, and has
    // one entry more than the stack has layers otherwise.
    const bool has_ramp = layer.chroma.total_layers() > 0 && !layer.chroma.bands.empty() &&
                          layer.chroma.layer_height_mm > 0.f;

    // ---- 2 Colors ----
    if (step == ChromaStepColors) {
        const std::vector<ChromaFilament> loaded   = chroma_filaments();
        const std::vector<ColorRGB>       palette  = chroma_palette(layer.chroma);
        const int                         distinct = chroma_distinct_levels(layer.chroma);
        const int                         relief   = layer.chroma.relief_layers();
        if (has_ramp) {
            const float  w     = std::max(m_imgui->scaled(2.f),
                                          ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f));
            const float  h     = frame_h;
            const ImVec2 p     = ImGui::GetCursorScreenPos();
            const int    first = std::max(0, layer.chroma.base_layers);
            const int    count = int(palette.size()) - first;
            // With the lights off the ramp shows what the overlay does, the glow at every height; the
            // band ticks below still read the lit palette, which is what says where one band ends.
            const bool                   dark = m_chroma_lights_off && chroma_has_glow(layer.chroma);
            const std::vector<ColorRGB>  glow = dark ? chroma_glow_palette(layer.chroma) : std::vector<ColorRGB>();
            const std::vector<ColorRGB> &ramp = glow.size() == palette.size() ? glow : palette;
            ImGui::Dummy(ImVec2(w, h));
            for (int i = 0; i < count; ++i) {
                const ColorRGB &c  = ramp[size_t(first + i)];
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
                const float     lum = chroma_luma(c);
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

        // ---- One row per filament, and a drawer for the rest of it ----
        //
        // On the row, what is changed on every pass: which filament, and how many layers of it, with its
        // transmission distance read off beside them. In the drawer under it, what is a filament's own and
        // set once: the transmission distance itself, measuring it, its place in the stack, and taking it
        // out. The row reads as one line of a stack, which is what a list of five of them has to do.
        {
            int remove    = -1;
            // A move up or down the stack, from the arrows in a filament's drawer.
            int drag_from = -1, drag_to = -1;
            for (size_t bi = 0; bi < layer.chroma.bands.size(); ++bi) {
                ChromaBand &band = layer.chroma.bands[bi];
                ImGui::PushID(int(bi));
                const float swatch_px = std::round(frame_h * 0.8f);
                ImGui::BeginGroup();

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
                    const float lum   = chroma_luma(c);
                    const std::string no = std::to_string(bi + 1);
                    const ImVec2      ts = ImGui::CalcTextSize(no.c_str());
                    dl->AddText(ImVec2(mn.x + 0.5f * (swatch_px - ts.x), top + 0.5f * (swatch_px - ts.y)),
                                lum > 0.5f ? IM_COL32(20, 20, 20, 230) : IM_COL32(240, 240, 240, 230), no.c_str());
                    if (drift.any()) {
                        const float r = std::max(2.5f, std::round(swatch_px * 0.2f));
                        dl->AddCircleFilled(ImVec2(mn.x + swatch_px - r, top + r), r, ImGui::GetColorU32(CHROMA_COL_WARN));
                    }
                }
                ImGui::SameLine(0.f, gap_s);
                // ---- the filament ----
                // The name alone on the closed list: the swatch beside it already says which slot it is,
                // and the open list carries the slot numbers that tell two spools of one name apart.
                const std::string current = band.filament.name.empty() ?
                                                Slic3r::format(_u8L("Filament %1%"), band.filament.filament + 1) :
                                                band.filament.name;
                const float step_w   = std::round(frame_h * 3.3f);
                const float td_w     = ImGui::CalcTextSize("TD 00.0").x + gap_s;
                const float icon_w   = std::round(frame_h * 0.85f);
                const float tail     = step_w + td_w + icon_w + 3.f * gap_s + std::max(card_pad, 1.f);
                const bool  open_row = m_chroma_band_open[slot] == int(bi);
                // A transmission distance is the filament's, so it is set on every band printed in it.
                const auto set_td = [&](float v) {
                    for (ChromaBand &b : layer.chroma.bands)
                        if (b.filament.filament == band.filament.filament)
                            b.filament.td_mm = v;
                    chroma_touch(layer.slot);
                };
                // The sentence every reading of the transmission distance carries, on the figure and on
                // the field in the drawer alike.
                // Built only when it is about to be shown: it is a few sentences and several translations,
                // and there is one per band on every frame otherwise.
                const auto td_said = [&]() {
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
                    return said;
                };
                ImGui::SetNextItemWidth(-tail);
                push_panel_combo_style(m_parent.get_scale());
                if (ImGui::BeginCombo("##hf_filament", current.c_str())) {
                    for (const ChromaFilament &f : loaded) {
                        if (ImGui::Selectable(chroma_filament_label(f).c_str(), f.filament == band.filament.filament)) {
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
                // Minus, the count, plus: a count is changed a layer at a time far more often than it is
                // typed, and the field between them still takes a typed one.
                ImGui::SameLine(0.f, gap_s);
                if (chroma_stepper("##hf_layers", band.layers, 1, CHROMA_MAX_RELIEF_LAYERS, step_w))
                    chroma_touch(layer.slot);
                hover_tip(_u8L("How many printed layers this filament gets, counting up the stack."));

                // ---- how deep light gets into it, read off ----
                ImGui::SameLine(0.f, gap_s);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", Slic3r::format(_u8L("TD %1$.1f"), band.filament.td_mm).c_str());
                if (ImGui::IsItemHovered())
                    panel_tooltip(td_said(), { { "chroma_td.gif" } });

                // ---- the rest of it, one click away ----
                ImGui::SameLine(0.f, gap_s);
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                              ImGui::GetContentRegionMax().x - std::max(card_pad, 1.f) - icon_w));
                {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##hf_open", ImVec2(icon_w, frame_h)))
                        m_chroma_band_open[slot] = open_row ? -1 : int(bi);
                    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, ImGui::IsItemHovered() ? 1.f : 0.6f);
                    const float a   = std::round(frame_h * 0.18f);
                    const float cx = p.x + 0.5f * icon_w, cy = p.y + 0.5f * frame_h;
                    if (open_row)
                        dl->AddTriangleFilled(ImVec2(cx - a, cy - 0.5f * a), ImVec2(cx + a, cy - 0.5f * a), ImVec2(cx, cy + 0.7f * a), col);
                    else
                        dl->AddTriangleFilled(ImVec2(cx - 0.5f * a, cy - a), ImVec2(cx + 0.7f * a, cy), ImVec2(cx - 0.5f * a, cy + a), col);
                    if (ImGui::IsItemHovered())
                        panel_tooltip(open_row ? _u8L("Closes this filament's settings.") :
                                                 _u8L("This filament's own settings: its transmission distance, measuring "
                                                      "it, where it comes in the print, and taking it out."));
                }

                // ---- the drawer ----
                //
                // The things that are a filament's own rather than the stack's, under the row they belong
                // to. One drawer open at a time: two open is a list that no longer reads as a stack.
                if (open_row) {
                    const float  pad   = std::round(m_imgui->scaled(0.3f));
                    const float  x0    = ImGui::GetCursorPosX();
                    const ImVec2 top   = ImGui::GetCursorScreenPos();
                    const float  right = ImGui::GetContentRegionMax().x - std::max(card_pad, 1.f); // window-relative
                    ImGui::Dummy(ImVec2(1.f, pad));
                    ImGui::SetCursorPosX(x0 + pad);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("TD");
                    ImGui::SameLine(0.f, gap_s);
                    ImGui::SetNextItemWidth(std::round(frame_h * 2.2f));
                    float td = band.filament.td_mm;
                    if (ImGui::InputFloat("##hf_td", &td, 0.f, 0.f, "%.1f"))
                        set_td(std::clamp(td, 0.05f, 100.f));
                    if (ImGui::IsItemHovered())
                        panel_tooltip(td_said(), { { "chroma_td.gif" } });

                    // Measure it, rather than guess it: no filament preset ships the number and nothing but
                    // a print can tell you it.
                    ImGui::SameLine(0.f, gap_s);
                    if (ImGui::Button(_u8L("Measure").c_str())) {
                        m_chroma_wedge_step = 0;
                        if (m_chroma_wedge_substrate < 0 || m_chroma_wedge_substrate == band.filament.filament) {
                            // The darkest other filament shows through a pale staircase best.
                            float darkest = 2.f;
                            for (const ChromaFilament &f : loaded)
                                if (f.filament != band.filament.filament) {
                                    const float lum = chroma_luma(f.color);
                                    if (lum < darkest) {
                                        darkest                  = lum;
                                        m_chroma_wedge_substrate = f.filament;
                                    }
                                }
                        }
                        ImGui::OpenPopup("hf_measure_pop");
                    }
                    if (ImGui::IsItemHovered())
                        panel_tooltip(_u8L("Measure this filament's transmission distance by printing a step wedge: a "
                                           "staircase of it over a filament it has to hide. Nothing but a print can tell "
                                           "you this number, and every color on this card is computed from it."),
                                      { { "chroma_td.gif" } });

                    // Its place in the print, and taking it out, at the right of the drawer.
                    const auto icon_button = [&](const char *id, int kind, bool enabled, const std::string &tip) {
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        m_imgui->disabled_begin(!enabled);
                        const bool hit = ImGui::InvisibleButton(id, ImVec2(icon_w, frame_h)) && enabled;
                        m_imgui->disabled_end();
                        const bool  hot = ImGui::IsItemHovered() && enabled;
                        const ImU32 col = kind == 2 && hot ? CHROMA_REMOVE_HOT :
                                                             ImGui::GetColorU32(ImGuiCol_Text, !enabled ? 0.25f : hot ? 1.f : 0.6f);
                        const float a  = std::round(icon_w * 0.24f);
                        const float cx = p.x + 0.5f * icon_w, cy = p.y + 0.5f * frame_h;
                        if (kind == 0)
                            dl->AddTriangleFilled(ImVec2(cx - a, cy + 0.5f * a), ImVec2(cx + a, cy + 0.5f * a), ImVec2(cx, cy - 0.7f * a), col);
                        else if (kind == 1)
                            dl->AddTriangleFilled(ImVec2(cx - a, cy - 0.5f * a), ImVec2(cx + a, cy - 0.5f * a), ImVec2(cx, cy + 0.7f * a), col);
                        else
                            chroma_draw_cross(dl, ImVec2(cx, cy), a, col);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            panel_tooltip(tip);
                        return hit;
                    };
                    ImGui::SameLine(0.f, gap_s);
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                                  right - pad - 3.f * icon_w - 2.f * gap_s));
                    if (icon_button("##hf_up", 0, bi > 0,
                                    _u8L("Moves this filament one row up: earlier in the print, nearer the backing."))) {
                        drag_from = int(bi);
                        drag_to   = int(bi) - 1;
                    }
                    ImGui::SameLine(0.f, gap_s);
                    if (icon_button("##hf_down", 1, bi + 1 < layer.chroma.bands.size(),
                                    _u8L("Moves this filament one row down: later in the print, nearer the light."))) {
                        drag_from = int(bi);
                        drag_to   = int(bi) + 1;
                    }
                    ImGui::SameLine(0.f, gap_s);
                    if (icon_button("##hf_remove", 2, layer.chroma.bands.size() > 1,
                                    layer.chroma.bands.size() > 1 ? _u8L("Takes this filament out of the stack.") :
                                                                    _u8L("A stack prints in at least one filament.")))
                        remove = int(bi);
                    ImGui::Dummy(ImVec2(1.f, pad));
                    dl->AddRect(top, ImVec2(ImGui::GetWindowPos().x + right, ImGui::GetCursorScreenPos().y - style.ItemSpacing.y + pad),
                                ImGui::GetColorU32(ImGuiCol_Separator), style.FrameRounding);
                }

                // Beside the panel, not over it, for the same reason the suggestion list is: the wedge
                // is set up by reading it against the row it belongs to - the filament, its current
                // number and where that number came from - and a window over the card hides all three.
                chroma_place_beside_panel(m_imgui->scaled(21.5f), m_imgui->scaled(0.5f));
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
                    if (ImGui::BeginCombo("##hf_sub", (sub == nullptr ? _u8L("pick one") : chroma_filament_label(*sub)).c_str())) {
                        for (const ChromaFilament &f : loaded) {
                            if (f.filament == band.filament.filament)
                                continue;
                            if (ImGui::Selectable(chroma_filament_label(f).c_str(), f.filament == m_chroma_wedge_substrate))
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
                        set_td(measured_td);
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

                ImGui::EndGroup();
                ImGui::PopID();
            }

            // ---- one more filament ----
            //
            // A plus at the end of the list, where the row it would add goes, rather than a button
            // among the edits that act on the whole stack: adding a filament adds a row here, and a
            // control that sits where its result appears needs no label to say so.
            {
                // A button, with the frame every other button on the card has: drawn as a bare glyph
                // it read as a decoration of the list rather than as something to press. The label is
                // the plus itself, so it is centred in the frame by the same code that centres every
                // other button's text.
                const float add_w = std::max(m_imgui->scaled(4.f), ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f));
                const bool  room  = !loaded.empty() && layer.chroma.bands.size() < CHROMA_MAX_BANDS;
                m_imgui->disabled_begin(!room);
                const bool add_one = ImGui::Button(("+  " + _u8L("Add a filament") + "##hf_add_band").c_str(), ImVec2(add_w, frame_h));
                m_imgui->disabled_end();
                if (add_one && room)
                    ImGui::OpenPopup("hf_add");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    panel_tooltip(room       ? _u8L("Adds another filament change on top of the stack.") :
                                  loaded.empty() ? _u8L("Load a filament into the project first.") :
                                                   _u8L("The stack already prints in as many filaments as it can."),
                                  room ? std::vector<TipMedia>{ { "chroma_order.gif" } } : std::vector<TipMedia>{});

                if (PanelPopup popup{"hf_add"}) {
                    // The whole row is the item, with the swatch and the name drawn over it rather than
                    // beside it: an item sized to its own label highlights behind the word only, which
                    // reads as nothing happening rather than as the row being picked. An empty label of
                    // width zero is the way to say that - it carries no size of its own and fills the row.
                    for (const ChromaFilament &f : loaded) {
                        ImGui::PushID(f.filament);
                        const bool picked = ImGui::Selectable("", false, 0, ImVec2(0.f, frame_h));
                        ImGui::SameLine(0.f, 0.f);
                        swatch(f.color, std::round(frame_h * 0.7f));
                        ImGui::SameLine(0.f, gap_s);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(chroma_filament_label(f).c_str());
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

            if (drag_from >= 0 && drag_to >= 0 && drag_from != drag_to &&
                drag_from < int(layer.chroma.bands.size()) && drag_to < int(layer.chroma.bands.size())) {
                const ChromaBand moved = layer.chroma.bands[size_t(drag_from)];
                layer.chroma.bands.erase(layer.chroma.bands.begin() + drag_from);
                layer.chroma.bands.insert(layer.chroma.bands.begin() + drag_to, moved);
                chroma_touch(layer.slot);
            }
            if (remove >= 0) {
                layer.chroma.bands.erase(layer.chroma.bands.begin() + remove);
                chroma_touch(layer.slot);
            }
        }

        // ---- Choosing the whole stack again, rather than one band of it ----
        {
            if (m_chroma_pick_count[slot] <= 0)
                m_chroma_pick_count[slot] = std::max(2, int(layer.chroma.bands.size()));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("Number of colors").c_str());
            ImGui::SameLine(0.f, gap_s);
            // Five choices and one click, instead of a slider nobody drags to pick four.
            std::vector<int> counts;
            for (int n : { 2, 3, 4, 6, 8 })
                if (n <= int(CHROMA_MAX_BANDS))
                    counts.push_back(n);
            segmented("##hf_count", counts, m_chroma_pick_count[slot],
                      std::max(m_imgui->scaled(4.f), ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - std::max(card_pad, 1.f)));
            hover_tip(_u8L("How many filaments the picture is printed in. Fewer is fewer changes to stand over; more is a "
                     "better match, never a worse one."),
                { { "chroma_colors.gif" } });

            // Their own row, a third each: the two ways of choosing the whole stack again, and the one edit
            // to the stack as a whole. On the row above they shared it with the count, and the last of them
            // was drawn past the right edge of the panel: a button nobody can reach.
            const float third = std::max(m_imgui->scaled(3.f),
                                         std::floor((ImGui::GetContentRegionAvail().x - std::max(card_pad, 1.f) - 2.f * gap_s) / 3.f));
            if (ImGui::Button(_u8L("Pick from mine").c_str(), ImVec2(third, frame_h)))
                chroma_pick_colors(layer, m_chroma_pick_count[slot]);
            hover_tip(_u8L("Builds the stack from that many of your loaded filaments: the ones that come closest to this "
                     "picture, in the order that reaches the most colors."),
                { { "chroma_colors.gif" }, { "chroma_order.gif" } });

            ImGui::SameLine(0.f, gap_s);
            if (ImGui::Button(_u8L("Suggest colors").c_str(), ImVec2(third, frame_h))) {
                chroma_suggest(layer, m_chroma_pick_count[slot]);
                ImGui::OpenPopup("hf_suggest");
            }
            hover_tip(_u8L("Works out the colors this picture would most like to be printed in, whether or not you own "
                     "them - so you know what to go and get.\n\n"
                     "A transmission distance matters as much as the color: the same red at TD 1 and at TD 6 are "
                     "different filaments for this. Click a color in the list to put your nearest into that band."),
                { { "chroma_colors.gif" } });

            ImGui::SameLine(0.f, gap_s);
            m_imgui->disabled_begin(!has_ramp || distinct >= relief);
            if (ImGui::Button(_u8L("Trim layers").c_str(), ImVec2(third, frame_h)) && has_ramp && distinct < relief) {
                chroma_trim_bands(layer.chroma);
                chroma_touch(layer.slot);
            }
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                panel_tooltip(Slic3r::format(_u8L("Shortens each band to its last layer that still changes the "
                                                  "color. %1% of %2% picture layers do."),
                                             distinct, relief),
                              { { "chroma_trim.png" } });

            // Beside the panel rather than over it, and at a width of its own. Over it, the list
            // covered the stack it is about: the use of a suggestion is reading it against the bands
            // underneath, and a window that hides them makes the reader close it to look.
            chroma_place_beside_panel(m_imgui->scaled(21.5f), m_imgui->scaled(0.5f));
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
                    ImGui::SameLine(0.f, gap_s);
                    m_imgui->disabled_begin(reachable == 0);
                    if (ImGui::Button(_u8L("Use my closest").c_str(), ImVec2(sug_half, frame_h)))
                        chroma_apply_suggestions(layer);
                    m_imgui->disabled_end();

                }
            }
        }

    }

    // ---- 3 Plate ----
    //
    // The physical thing and the light it is looked at in. Every row in the first half is a dimension
    // of the plate, which is why which way up it prints is among them and why the frame's own two
    // numbers are not: those belong beside the drawing of the moulding, in the gallery where a frame
    // is chosen by looking at it. For a picture painted onto somebody's model, the rows that say where
    // it lands come after these, from the layer card (see render_texture_displacement_panel()).
    if (step == ChromaStepPlate) {
        // A heading over both halves, whether or not the picture made its own plate: for one painted
        // onto a model, the plate is the model, and what is set here is what is under the picture.
        const ChromaPlate &plate = layer.chroma.plate;
        heading(_L("The plate"), plate.generated ? Slic3r::format(_u8L("%1$.0f x %2$.0f x %3$.1f mm"), plate.width_mm,
                                                                  plate.height_mm, plate.base_mm + layer.chroma.depth_mm()) :
                                                   std::string(),
                card_pad);
        render_chroma_plate(layer, card_pad, has_ramp);

        ImGui::Dummy(ImVec2(1.f, gap_s));
        heading(_L_CONTEXT("Light", "ChromaStack lighting"), {}, card_pad);
        render_chroma_light(layer, card_pad);
    }
}

void GLGizmoTextureDisplacement::chroma_set_step(TextureDisplacementLayer &layer, int step)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return;
    const int from = m_chroma_step[size_t(layer.slot)];
    step           = std::clamp(step, 0, int(ChromaStepCount) - 1);
    if (step == from)
        return;
    // The brush is Check's: left on, dragging over the model would go on painting corrections with
    // the controls that say what it paints no longer on the card.
    if (from == ChromaStepCheck && m_chroma_retouch_mode) {
        if (m_chroma_retouch_painting)
            chroma_commit_stroke(layer);
        m_chroma_retouch_mode = false;
    }
    // The placement handle is Plate's, for the same reason.
    if (from == ChromaStepPlate && m_adjust_texture_mode) {
        m_adjust_texture_mode = false;
        m_adjust_drag_handle  = AdjustHandle::None;
    }
    m_chroma_step[size_t(layer.slot)] = step;
    m_parent.set_as_dirty();
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
    // One label column for the section, as wide as its widest label, so every field starts at the same x:
    // the panel's own column is narrower than "Solid backing", and a row whose label overran it pushed
    // its field out of line with the rest.
    float label_w = 0.f;
    for (const wxString &l : { _L("Layers below it"), _L("Picture is"), _L("Solid backing"), _L("Which way up"),
                               _L("Flat tile") })
        label_w = std::max(label_w, ImGui::CalcTextSize(into_u8(l).c_str()).x);
    label_w += m_imgui->scaled(0.8f);
    const auto row_label = [&](const wxString &label) {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + label_w);
    };

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
        // Kept to the face by chroma_follow_face(), every frame and on every step.
        const int measured = chroma_measured_base(layer);
        row_label(_L("Layers below it"));
        int base = layer.chroma.base_layers;
        m_imgui->disabled_begin(layer.chroma.base_auto);
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputInt("##hf_base", &base, 0, 0) && !layer.chroma.base_auto) {
            // Never more than the model has under the painted face. Above that, chroma_ranges() writes
            // every swap higher than the top of the object, so not one of them is reached and the whole
            // picture prints in the bottom band's filament - which is also what the 3D view then shows,
            // since it reads a fragment's height off the object's own bottom. Below it is the one
            // reading that means something: a picture not meant to start at the painted face.
            layer.chroma.base_layers = std::clamp(base, 0, measured > 0 ? measured : 20000);
            chroma_touch(layer.slot);
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            panel_tooltip(Slic3r::format(_u8L("The solid backing under the picture: %1$.2f mm, in layers.\n\n"
                                              "It is a measurement of the model - everything below the painted face - "
                                              "and the colors are computed assuming no light gets through it. It does "
                                              "not make the model any thicker: what it moves is the height each "
                                              "filament swap is written at. The padlock stops it following the model, "
                                              "for a picture that is not meant to start at the painted face."),
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
        // The state in words beside the padlock: closed-means-automatic is not a thing a reader can guess.
        ImGui::SameLine(0.f, gap_s);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", layer.chroma.base_auto ? _u8L("follows the face").c_str() : _u8L("set by hand").c_str());
    }

    // ---- The plate, for a picture that made one for itself ----
    //
    // Only here: a picture painted onto a model has a plate already, and it is the model.
    if (layer.chroma.plate.generated) {
        ChromaPlate &plate  = layer.chroma.plate;
        const auto   redraw = [&]() { chroma_plate_changed(layer); };

        // ---- The frame ----
        //
        // A row of its own, and the row is the frame: the cross-section the plate is wearing, drawn, with
        // its name and its two numbers beside it. A frame is something seen, so the thing that opens the
        // gallery is a picture of what you already have rather than a word and a list of words - and the
        // gallery is where it is chosen anyway, because that is the only place the width, the rise, your
        // own mouldings and cutting the plate to an outline all are.
        ChromaFrame &frame = plate.frame;
        {
            const float  box_w = std::max(m_imgui->scaled(6.f),
                                          ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - std::max(card_pad, 1.f));
            const float  box_h = std::round(frame_h * 2.1f);
            const ImVec2 p0    = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##hf_frame_pick", ImVec2(box_w, box_h))) {
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
            const bool  hot   = ImGui::IsItemHovered();
            const float round = ImGui::GetStyle().FrameRounding;
            const float pad   = std::round(frame_h * 0.3f);
            dl->AddRectFilled(p0, ImVec2(p0.x + box_w, p0.y + box_h),
                              ImGui::GetColorU32(hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), round);
            dl->AddRect(p0, ImVec2(p0.x + box_w, p0.y + box_h),
                        ImGui::GetColorU32(hot ? ImGuiWrapper::COL_ORCA : ImGui::GetStyleColorVec4(ImGuiCol_Border)), round);

            // The cross-section, at the left of the row.
            const float  draw_w = std::round(frame_h * 2.4f);
            const ImVec2 d0(p0.x + pad, p0.y + pad);
            const ImVec2 d1(d0.x + draw_w, p0.y + box_h - pad);
            const ImU32  ink    = ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : 0.8f);
            const float  stroke = std::max(1.4f, std::round(frame_h * 0.07f));
            dl->AddLine(ImVec2(d0.x, d1.y), ImVec2(d1.x, d1.y), ImGui::GetColorU32(ImGuiCol_Separator));
            const std::vector<Vec2f> profile = frame.frame_id.empty() ? std::vector<Vec2f>() :
                                                                        chroma_frame_profile(frame, plate.base_mm);
            if (profile.size() < 2) {
                // No frame on the plate, so the mark is what the gallery is for rather than what is in
                // it: a picture inside a moulding, which is the one shape a frame has whatever its profile.
                const float i = std::max(1.5f, std::round(frame_h * 0.14f));
                dl->AddRect(d0, d1, ImGui::GetColorU32(ImGuiCol_Text, 0.45f), 0.f, 0, stroke);
                dl->AddRect(ImVec2(d0.x + i, d0.y + i), ImVec2(d1.x - i, d1.y - i), ImGui::GetColorU32(ImGuiCol_Text, 0.45f));
            } else {
                float top = plate.base_mm;
                for (const Vec2f &v : profile)
                    top = std::max(top, v.y());
                const float span = std::max(1e-3f, top - plate.base_mm);
                // The picture's surface on the left and the plate's outer edge on the right, which is
                // the way the plate is looked at from above with the moulding running round it.
                const auto at = [&](const Vec2f &v) {
                    return ImVec2(d0.x + draw_w * (v.x() / std::max(1e-3f, frame.width_mm)),
                                  d1.y - (d1.y - d0.y) * (v.y() - plate.base_mm) / span);
                };
                for (size_t i = 0; i + 1 < profile.size(); ++i)
                    dl->AddLine(at(profile[i]), at(profile[i + 1]), ink, stroke);
            }

            // Its name and its two numbers.
            const ChromaFrameDef *def = frame.frame_id.empty() ? nullptr :
                                                                 ChromaFrameLibrary::instance().find(frame.frame_id);
            const std::string name = def != nullptr          ? Slic3r::format(_u8L("Frame: %1%"), into_u8(def->name)) :
                                     !frame.frame_id.empty() ? Slic3r::format(_u8L("Frame: %1%"), frame.frame_id) :
                                                               _u8L("No frame");
            const std::string size = !frame.frame_id.empty() && frame.width_mm > 0.f ?
                                         Slic3r::format(_u8L("%1$.1f mm wide, %2$.1f mm rise"), frame.width_mm, frame.rise_mm) :
                                         _u8L("The picture runs to the edge of the plate.");
            // What a click does, drawn as the button it behaves as, at the right; the name and the two
            // numbers fill what is left and are cut short before they reach it.
            const std::string open = _u8L("Frames");
            const ImVec2      ts   = ImGui::CalcTextSize(open.c_str());
            const ImVec2      b1(p0.x + box_w - pad, std::round(p0.y + 0.5f * (box_h + frame_h)));
            const ImVec2      b0(b1.x - ts.x - 2.f * ImGui::GetStyle().FramePadding.x, b1.y - frame_h);
            const float       tx   = d1.x + pad;
            const float       room = std::max(frame_h, b0.x - pad - tx);
            const float       line = ImGui::GetTextLineHeight();
            const float       ty   = std::round(p0.y + 0.5f * (box_h - 2.f * line));
            m_imgui->push_bold_font();
            dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), ellipsize(name, room).c_str());
            m_imgui->pop_bold_font();
            dl->AddText(ImVec2(tx, ty + line), ImGui::GetColorU32(ImGuiCol_TextDisabled), ellipsize(size, room).c_str());

            dl->AddRectFilled(b0, b1, ImGui::GetColorU32(hot ? ImGuiCol_ButtonHovered : ImGuiCol_Button), round);
            dl->AddText(ImVec2(b0.x + ImGui::GetStyle().FramePadding.x, b0.y + 0.5f * (frame_h - ts.y)),
                        ImGui::GetColorU32(ImGuiCol_Text), open.c_str());
            if (hot)
                panel_tooltip(_u8L("Choose a frame from the gallery, where its width and how far it stands above the "
                                   "picture are set too, along with adding your own and cutting the plate to an SVG "
                                   "outline."));
            ImGui::Dummy(ImVec2(1.f, gap_s));
        }

        // One number for the size, because the other one is the picture's to decide.
        const float aspect  = plate.width_mm / std::max(0.1f, plate.height_mm);
        float       longest = std::max(plate.width_mm, plate.height_mm);
        row_label(_L("Picture is"));
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputFloat("##hf_plate_size", &longest, 0.f, 0.f, "%.0f mm")) {
            chroma_plate_fit(plate, std::clamp(longest, 10.f, 1000.f), aspect);
            redraw();
        }
        ImGui::SameLine(0.f, gap_s);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Slic3r::format(_u8L("wide, so %1$.0f x %2$.0f"), plate.width_mm,
                                                 plate.height_mm)
                                      .c_str());
        hover_tip(_u8L("The longer side of the picture on the plate. The other side follows the picture's own "
                 "proportions, and the frame is added outside both."));

        row_label(_L("Solid backing"));
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputFloat("##hf_plate_base", &plate.base_mm, 0.f, 0.f, "%.1f mm")) {
            plate.base_mm = std::clamp(plate.base_mm, 0.2f, 20.f);
            redraw();
        }
        hover_tip(_u8L("How solid the plate is under the picture. The colors are computed assuming no light gets "
                 "through it, so this has to be thick enough to be opaque in the first filament - about a "
                 "tenth of its transmission distance."),
            { { "chroma_td.gif" } });

        // ---- Which way up it prints ----
        //
        // Face down is the one way to get a picture with no layer lines in it at all: the picture's own
        // face is printed against the build plate, so it comes out as smooth as that plate is.
        row_label(_L("Which way up"));
        {
            const bool down  = plate.orientation == ChromaOrientation::FaceDown;
            int        which = down ? 1 : 0;
            if (chroma_segmented("##hf_orient", { _u8L("Picture up"), _u8L("Picture down") }, which,
                                 ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - std::max(card_pad, 1.f))) {
                if (which == 0) {
                    plate.orientation      = ChromaOrientation::FaceUp;
                    layer.chroma.base_auto = true;
                } else {
                    plate.orientation = ChromaOrientation::FaceDown;
                    // The picture is at the bottom now, so there is no face above it to measure a base
                    // from, and the backing has to be at least as thick as the whole stack is tall -
                    // everything the picture is made of prints above it.
                    layer.chroma.base_auto = false;
                    plate.base_mm          = std::max(plate.base_mm, layer.chroma.total_mm());
                }
                redraw();
            }
            hover_tip(_u8L("Which way up the plate prints. Picture up is the ordinary way, and the picture carries "
                     "the layer lines of its own terraces. Picture down prints the picture's face against the "
                     "build plate, which comes out as smooth as the plate is - and turns the relief inside "
                     "out, so the backing has to be thick enough to hold the whole stack above it.\n\n"
                     "Turn brim and raft off for this: both would print across the picture's face."));

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
        // The terraced plate, or which of the three flat tiles it builds. The flat tiles all end up the
        // same slab; what differs is where the bands sit inside it, and that decides what the fill
        // prints in and the order the light crosses the colours in.
        //
        // The terraced plate has to be a choice of its own and be named as one: it is the default
        // (m_chroma_flat_mode = 3), and a combo that showed it under a flat tile's name made Bake build
        // the plate while the card said FlatStack.
        row_label(_L("Flat tile"));
        {
            // In the order a reader meets them: the plate itself, then the three flat tiles. The index on
            // the bar is not m_chroma_flat_mode's, whose numbering Bake reads.
            static constexpr int modes[] = { 3, 0, 1, 2 };
            int                  which   = 0;
            for (int i = 0; i < 4; ++i)
                if (modes[i] == m_chroma_flat_mode)
                    which = i;
            // TRN The four things a ChromaStack plate can be baked into: the terraced plate itself ("None",
            // no flat tile), clear filament in front of the bands, the backing behind them, and FlatStack.
            if (chroma_segmented("##hf_flat_mode",
                                 { _u8L_CONTEXT("None", "ChromaStack flat tile"), _u8L_CONTEXT("Clear", "ChromaStack flat tile"),
                                   _u8L_CONTEXT("Backing", "ChromaStack flat tile"), _u8L("FlatStack") },
                                 which, ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - std::max(card_pad, 1.f)))
                m_chroma_flat_mode = modes[which];
            hover_tip(_u8L("None bakes the terraced plate: one filament a layer, the picture carried by the height "
                     "of its columns.\n\n"
                     "FlatStack slides each column's own sequence up against the flat face, so the light crosses "
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
void GLGizmoTextureDisplacement::render_chroma_light(TextureDisplacementLayer &layer, float card_pad)
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
    // The name of the choice on its own line, with the one-click lithophane at its right, and the three
    // choices across the whole width under it: on one line with its label, "Behind and in front" was cut.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Light comes from"));
    {
        const ChromaLighting mode = layer.chroma.lighting;
        {
            // One click from a painting to a lithophane, because a lithophane is not only the light moving:
            // the bands want the opposite order and the base wants to be a layer or two, and a user who
            // switched the light alone gets a black plate and no idea which of the other two was why.
            // chroma_make_lithophane() is what it changes and why.
            const float lw = ImGui::CalcTextSize(_u8L("Lithophane").c_str()).x + 2.f * ImGui::GetStyle().FramePadding.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - std::max(card_pad, 1.f) - lw));
            if (ImGui::Button(_u8L("Lithophane").c_str(), ImVec2(lw, 0.f))) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Make the ChromaStack picture a lithophane"),
                                              UndoRedo::SnapshotType::GizmoAction);
                chroma_make_lithophane(layer.chroma);
                if (layer.chroma.plate.generated)
                    chroma_plate_changed(layer);
                else
                    chroma_touch(layer.slot);
            }
            hover_tip(_u8L("Sets the print up to be lit from behind: the light moves behind it, the darkest filament "
                           "goes to the bottom of the stack, and the base becomes two thin layers instead of a solid "
                           "backing. A white lamp still at its default becomes a neutral 4000 K white.\n\n"
                           "The picture is not inverted: thicker is darker from behind, and the solve already puts the "
                           "thick columns where the picture is dark."));
        }

        const bool           back = mode == ChromaLighting::BackLit;
        const bool           both = mode == ChromaLighting::BothLit;
        // The options are in the enum's own order: front, behind, both.
        int which = int(mode);
        if (chroma_segmented("##hf_lighting", { _u8L("The front"), _u8L("Behind"), _u8L("Behind and in front") }, which,
                             ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - std::max(card_pad, 1.f))) {
            layer.chroma.lighting = ChromaLighting(which);
            chroma_touch(layer.slot);
        }
        hover_tip(_u8L("Where the light is when the print is looked at. In front is a filament painting. Behind is "
                 "a lithophane: the light comes through the whole stack, so thicker is darker, the darkest "
                 "filaments belong at the bottom, and the backing has to be a layer or two rather than solid "
                 "- a picture on a thick base is black from behind.\n\n"
                 "Behind and in front is both at once, which is what a lit plate in a lit room really is. "
                 "A stack chosen for the lamp alone washes out as soon as the room light lands on it, so "
                 "this is the one to solve against for anything that will not hang in the dark."));
        // The lamp's combo below, which is still a list: its presets do not fit one row.
        const float combo_w = m_imgui->scaled(7.f);

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
            // The lamp, by what is sold: three whites by colour temperature, the five coloured LEDs, a
            // white of any temperature, and any colour at all through the swatch. `led_color` is what
            // the palette reads in every case; `led_kelvin` only remembers that a white was given as a
            // temperature, so the panel can say so.
            struct LampPreset
            {
                std::string name;
                float       kelvin;
                ColorRGB    color;
            };
            const std::vector<LampPreset> presets = {
                { _u8L("Plain white"), 0.f, ColorRGB(1.f, 1.f, 1.f) },
                { _u8L("Warm white, 2700 K"), 2700.f, ColorRGB() },
                { _u8L("Natural white, 4000 K"), 4000.f, ColorRGB() },
                { _u8L("Cold white, 6500 K"), 6500.f, ColorRGB() },
                { _u8L("White of any temperature"), -1.f, ColorRGB() },
                { _u8L("Red"), 0.f, ColorRGB(1.f, 0.f, 0.f) },
                { _u8L("Green"), 0.f, ColorRGB(0.f, 1.f, 0.f) },
                { _u8L("Blue"), 0.f, ColorRGB(0.f, 0.f, 1.f) },
                { _u8L("Cyan"), 0.f, ColorRGB(0.f, 1.f, 1.f) },
                { _u8L("Magenta"), 0.f, ColorRGB(1.f, 0.f, 1.f) },
            };
            const float kelvin  = layer.chroma.led_kelvin;
            int         current = -1;
            for (int i = 0; i < int(presets.size()) && current < 0; ++i) {
                const LampPreset &pr = presets[size_t(i)];
                if (pr.kelvin > 0.f ? std::abs(kelvin - pr.kelvin) < 0.5f :
                    pr.kelvin < 0.f ? kelvin > 0.f :
                                      kelvin <= 0.f && layer.chroma.led_color == pr.color)
                    current = i;
            }

            slider_label(_L("Lamp"));
            ImGui::SetNextItemWidth(combo_w);
            push_panel_combo_style(m_parent.get_scale());
            if (ImGui::BeginCombo("##hf_led_preset",
                                  current >= 0 ? presets[size_t(current)].name.c_str() : _u8L("Custom color").c_str())) {
                for (int i = 0; i < int(presets.size()); ++i) {
                    const LampPreset &pr = presets[size_t(i)];
                    if (ImGui::Selectable(pr.name.c_str(), i == current) && i != current) {
                        if (pr.kelvin < 0.f)
                            // Any temperature: start from where the lamp is if it is already a white of
                            // one, from a neutral white otherwise.
                            layer.chroma.led_kelvin = kelvin > 0.f ? kelvin : 5000.f;
                        else
                            layer.chroma.led_kelvin = pr.kelvin;
                        layer.chroma.led_color = layer.chroma.led_kelvin > 0.f ?
                                                     chroma_kelvin_to_rgb(layer.chroma.led_kelvin) :
                                                     pr.color;
                        chroma_touch(layer.slot);
                    }
                }
                ImGui::EndCombo();
            }
            pop_panel_combo_style();
            hover_tip(_u8L("The light behind the print. A warm white lamp and a cold white one show the same filaments "
                     "as different colors, and this is where that is accounted for. Most lithophane lamps are a "
                     "white LED; the box or the listing usually gives its temperature in kelvin."));

            ImGui::SameLine(0.f, m_imgui->scaled(0.6f));
            ImVec4 led(layer.chroma.led_color.r(), layer.chroma.led_color.g(), layer.chroma.led_color.b(), 1.f);
            if (ImGui::ColorEdit3("##hf_led", &led.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                layer.chroma.led_color  = ColorRGB(led.x, led.y, led.z);
                // Picked as a colour now, not as a temperature.
                layer.chroma.led_kelvin = 0.f;
                chroma_touch(layer.slot);
            }
            hover_tip(_u8L("Any color for the light, for a lamp none of the presets describe."));

            if (layer.chroma.led_kelvin > 0.f) {
                slider_label(_L("Color temperature"));
                float k = layer.chroma.led_kelvin;
                if (typed_slider("##hf_led_k", m_imgui->scaled(9.f) - panel_edit_icon_room(), &k, 1000.f, 12000.f,
                                 "%.0f K", false,
                                 _L("The lamp's color temperature. 2700 K is a warm household bulb, 4000 K a neutral "
                                    "white, 6500 K daylight."))) {
                    layer.chroma.led_kelvin = std::clamp(k, 1000.f, 12000.f);
                    layer.chroma.led_color  = chroma_kelvin_to_rgb(layer.chroma.led_kelvin);
                    chroma_touch(layer.slot);
                }
            }

            slider_label(_L("Brightness"));
            float lumens = layer.chroma.led_intensity;
            if (typed_slider("##hf_led_i", m_imgui->scaled(9.f) - panel_edit_icon_room(), &lumens, 0.2f, 8.f, "%.1fx",
                             false,
                             _L("How bright that light is, as a multiple of a plain white lamp. A deep stack passes very "
                                "little, so more than one is normal here and is what a bright panel behind the print "
                                "actually does."))) {
                layer.chroma.led_intensity = std::max(0.f, lumens);
                chroma_touch(layer.slot);
            }
        }
    }

    // ---- What the filaments do besides absorbing light ----
    //
    // One row per filament rather than a control in each band's row, which is full: the effect is a
    // property of the filament, it is set once if at all, and most stacks have none. It starts as
    // whatever the filament's name says (chroma_effect_from_name()), and an edit applies to every band
    // printed in that filament, the way the TD field does.
    ImGui::Dummy(ImVec2(0.f, gap_s));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_u8L("Filament effects").c_str());
    hover_tip(_u8L("What a filament does besides absorbing light. Glow in the dark shows in the lights off view; "
                   "color shift turns the filament toward a second color as the print is seen at an angle. "
                   "Neither changes the colors the picture is solved for, only how the preview shows them.\n\n"
                   "Read from the filament's name to begin with: glow, luminous or GITD for a glow, chameleon, "
                   "color shift or starlight for a shift."));
    {
        const auto effect_name = [](ChromaEffect e) {
            switch (e) {
            case ChromaEffect::Glow: return _u8L("Glow in the dark");
            case ChromaEffect::ColorShift: return _u8L("Color shift");
            default: return _u8L("No effect");
            }
        };
        ImDrawList *edl     = ImGui::GetWindowDrawList();
        const float fh      = ImGui::GetFrameHeight();
        const float combo_w = m_imgui->scaled(6.5f);
        const float right   = ImGui::GetContentRegionMax().x - m_imgui->scaled(0.5f);
        std::vector<ChromaBand> &bands = layer.chroma.bands;
        for (size_t bi = 0; bi < bands.size(); ++bi) {
            const ChromaFilament &f = bands[bi].filament;
            // One row per filament: a filament used by two bands is one filament with one effect.
            bool seen = false;
            for (size_t prev = 0; prev < bi && !seen; ++prev)
                seen = f.filament >= 0 && bands[prev].filament.filament == f.filament;
            if (seen)
                continue;

            ImGui::PushID(int(bi));
            const ImVec2 p  = ImGui::GetCursorScreenPos();
            const float  sw = std::round(fh * 0.7f);
            edl->AddRectFilled(ImVec2(p.x, p.y + 0.5f * (fh - sw)), ImVec2(p.x + sw, p.y + 0.5f * (fh + sw)),
                               ImGui::GetColorU32(ImVec4(f.color.r(), f.color.g(), f.color.b(), 1.f)), 2.f);
            ImGui::Dummy(ImVec2(sw, fh));
            ImGui::SameLine(0.f, m_imgui->scaled(0.4f));
            ImGui::AlignTextToFramePadding();
            const std::string label = f.name.empty() ? Slic3r::format(_u8L("Filament %1%"), f.filament + 1) : f.name;
            // Clipped short of the controls, so a long preset name cannot push them off the panel.
            const float name_end = right - combo_w - fh - m_imgui->scaled(0.8f);
            ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                                ImVec2(ImGui::GetWindowPos().x + name_end, ImGui::GetCursorScreenPos().y + fh), true);
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopClipRect();

            const auto apply = [&](ChromaEffect effect, const ColorRGB &color) {
                for (ChromaBand &b : bands)
                    if (&b == &bands[bi] || (f.filament >= 0 && b.filament.filament == f.filament)) {
                        b.filament.effect       = effect;
                        b.filament.effect_color = color;
                    }
                chroma_touch(layer.slot);
                m_parent.set_as_dirty();
            };

            ImGui::SameLine(right - combo_w - fh - m_imgui->scaled(0.4f));
            ImGui::SetNextItemWidth(combo_w);
            push_panel_combo_style(m_parent.get_scale());
            if (ImGui::BeginCombo("##hf_effect", effect_name(f.effect).c_str())) {
                for (ChromaEffect e : { ChromaEffect::None, ChromaEffect::Glow, ChromaEffect::ColorShift })
                    if (ImGui::Selectable(effect_name(e).c_str(), e == f.effect) && e != f.effect) {
                        const ColorRGB color = chroma_default_effect_color(e, f.color);
                        apply(e, color);
                    }
                ImGui::EndCombo();
            }
            pop_panel_combo_style();

            if (f.effect != ChromaEffect::None) {
                ImGui::SameLine(0.f, m_imgui->scaled(0.4f));
                ImVec4 ec(f.effect_color.r(), f.effect_color.g(), f.effect_color.b(), 1.f);
                if (ImGui::ColorEdit3("##hf_effect_color", &ec.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                    apply(f.effect, ColorRGB(ec.x, ec.y, ec.z));
                hover_tip(f.effect == ChromaEffect::Glow ?
                              _u8L("The color it glows. Most glow in the dark filament glows yellow-green or aqua, "
                                   "whatever its color in daylight.") :
                              _u8L("The color it turns when seen at a grazing angle. Face on it is the filament's own "
                                   "color."));
            }
            ImGui::PopID();
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

}


// The views of this picture: what is drawn over the model. Not on the card: the card keeps the printed
// colours on, which is the view a picture is judged by, and leaves the rest at their defaults. Kept with
// the switches for the slicer's view and the glow, which have no other place yet.
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
// The preview's own switches - the softening, seeing through the thin places, the layer slider and the
// debug views - are not on the card. They keep their defaults, which are the preview a print is judged
// by; they are for taking a solve apart, not for making a picture.
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
        } else if (kind == 4) {
            // Lights off: a crescent, the disc with a second one cut out of it in the button's own colour.
            dl->AddCircleFilled(ImVec2(p.x + c, p.y + c), 1.5f * q, fg);
            dl->AddCircleFilled(ImVec2(p.x + c + 0.8f * q, p.y + c - 0.5f * q), 1.3f * q,
                                on  ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) :
                                hot ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) :
                                      ImGui::GetColorU32(ImGuiCol_Button));
        } else if (kind == 3) {
            // Slicer view: three solid bars, each one flat colour and nothing blended between them.
            for (int i = -1; i <= 1; ++i)
                dl->AddRectFilled(ImVec2(p.x + c - 1.6f * q, p.y + c + float(i) * 1.1f * q - 0.4f * q),
                                  ImVec2(p.x + c + 1.6f * q, p.y + c + float(i) * 1.1f * q + 0.4f * q), fg, 1.f);
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

    // Slicer view rides on the printed colors overlay, which is the surface it recolours, so turning it
    // on turns that on too rather than leaving a switch that visibly does nothing.
    ImGui::SameLine(0.f, gap_s);
    bool slicer_on = m_chroma_slicer_view;
    view_toggle("##v_slicer", 3, slicer_on,
                _u8L("Slicer view: every surface in the plain color of the filament its layer is printed in, "
                     "with nothing blended and no light on it - what the sliced preview will show.\n\n"
                     "Use it to see which filament ends up where. The printed colors view is what the print "
                     "will look like."));
    if (slicer_on != m_chroma_slicer_view) {
        m_chroma_slicer_view = slicer_on;
        if (slicer_on && !m_show_chroma_colors) {
            m_show_chroma_colors = true;
            m_chroma_color_key   = 0;
        }
        m_parent.set_as_dirty();
    }

    // Lights off, only for a stack with something in it that glows. Like slicer view it recolours the
    // printed colors overlay, so it turns that on with it.
    if (chroma_has_glow(layer.chroma)) {
        ImGui::SameLine(0.f, gap_s);
        bool dark_on = m_chroma_lights_off;
        view_toggle("##v_dark", 4, dark_on,
                    _u8L("Lights off: the print as it looks in the dark, lit only by its glow in the dark "
                         "filament. The glow comes up through every layer above it, so a translucent stack lets "
                         "it through and an opaque top hides it."));
        if (dark_on != m_chroma_lights_off) {
            m_chroma_lights_off = dark_on;
            if (dark_on && !m_show_chroma_colors) {
                m_show_chroma_colors = true;
                m_chroma_color_key   = 0;
            }
            m_parent.set_as_dirty();
        }
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
}

// The chart of a solve: the picture's colours as a cloud, each as large as its share of the picture,
// and the stack's reach as the line its levels walk from the base to the top, hue across and
// lightness up. Drawn into whatever rectangle it is given - the Check step's small copy and the window
// it opens - with `scale` sizing the dots and the line to the chart.
void GLGizmoTextureDisplacement::draw_chroma_diag_plot(ImDrawList *wdl, const ChromaDiagnostics &diag, const ImVec2 &origin,
                                                       float plot_w, float plot_h, float scale)
{
    const auto col_of = [](const ColorRGB &c, int alpha) {
        return IM_COL32(int(std::lround(255.f * std::clamp(c.r(), 0.f, 1.f))),
                        int(std::lround(255.f * std::clamp(c.g(), 0.f, 1.f))),
                        int(std::lround(255.f * std::clamp(c.b(), 0.f, 1.f))), alpha);
    };
    const ImU32 grid  = ImGui::GetColorU32(ImGuiCol_Separator);
    const ImU32 faint = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    wdl->AddRectFilled(origin, ImVec2(origin.x + plot_w, origin.y + plot_h),
                       ImGui::GetColorU32(ImGuiCol_FrameBg));

    // Lightness runs up the left, because that is the axis the relief works along: a column's
    // height reaches the eye mostly as tone, so two filaments at the same height on this chart
    // give a ramp that does not ramp however different their hues are.
    for (int l = 0; l <= 100; l += 25) {
        const float y = origin.y + plot_h * (1.f - 0.01f * float(l));
        wdl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + plot_w, y), grid);
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
        const float r     = scale * (m_imgui->scaled(0.12f) + m_imgui->scaled(0.42f) * std::sqrt(share));
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
        wdl->AddLine(at(a0.at), at(b0.at), col_of(b0.color, 230), std::max(1.f, scale * m_imgui->scaled(0.14f)));
    }
    for (const ChromaReach &r : diag.reach) {
        const ImVec2 p = at(r.at);
        // A level that prints none of the picture is a layer nobody asked for, and it is drawn
        // hollow so a dead stretch of the curve can be seen as a run of them.
        const float  rad = scale * m_imgui->scaled(r.serves > 0.f ? 0.22f : 0.16f);
        if (r.serves > 0.f)
            wdl->AddCircleFilled(p, rad, col_of(r.color, 255));
        else
            wdl->AddCircle(p, rad, faint, 0, m_imgui->scaled(0.07f));
        wdl->AddCircle(p, rad, ImGui::GetColorU32(ImGuiCol_WindowBg), 0, m_imgui->scaled(0.05f));
    }
    wdl->AddRect(origin, ImVec2(origin.x + plot_w, origin.y + plot_h), grid);
}

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
        const ImU32   faint   = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        draw_chroma_diag_plot(wdl, diag, origin, plot_w, plot_h, 1.f);
        // The lightness scale up the left, outside the plot.
        for (int l = 0; l <= 100; l += 25) {
            const float       y = origin.y + plot_h * (1.f - 0.01f * float(l));
            const std::string v = std::to_string(l);
            wdl->AddText(ImVec2(origin.x - m_imgui->scaled(0.3f) - ImGui::CalcTextSize(v.c_str()).x,
                                y - 0.5f * ImGui::GetTextLineHeight()),
                         faint, v.c_str());
        }

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
            const ImVec4 *avg_col = chroma_match_col(diag.mean_de);
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

// The picture and the print it is predicted to make, at the size of the canvas.
//
// The card's two thumbnails answer "is this worth printing"; they cannot answer "is this face still
// the face", which is the question a portrait or anything with text in it actually turns on. Drawn
// over the 3D view rather than in the panel, because the panel is 420 px wide and that is the whole
// problem.
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
