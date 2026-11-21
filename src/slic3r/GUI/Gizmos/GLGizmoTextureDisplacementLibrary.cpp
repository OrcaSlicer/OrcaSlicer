// One of the files GLGizmoTextureDisplacement is defined across. The class is declared once, in
// GLGizmoTextureDisplacement.hpp; its members are defined in GLGizmoTextureDisplacement.cpp and in the
// siblings named after the part of the gizmo they hold - Chroma, Library, Mesh, Preview, UV. The split
// is along those lines and nothing else: no member moved between parts, and the class did not change.
// Helpers more than one part needs live in GLGizmoTextureDisplacementInternal.hpp.

// Where a layer's texture comes from: adding a layer, the built-in library and the picker popup it is
// browsed in, importing a file of one's own, and the image adjustment window - brightness, contrast,
// blur, invert - with its live preview.

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

namespace {
// Size the image-adjustment panel's live preview is computed and uploaded at. The panel draws the image
// in a box about 240 px wide, so there is nothing to show above this - and the preview is recomputed on
// every slider move, which is what makes its cost worth bounding. Measured on a 1024x1024 with every
// knob engaged, a full-size pass is 13-19 ms grayscale / 40-60 ms in colour (130-190 ms at 2048 in
// colour); the same pass on a 384 px copy is 3-4 ms / 9-12 ms, and it runs off the UI thread besides.
// The full-size image is only ever adjusted once, on commit.
constexpr int ADJUST_PREVIEW_MAX_PX = 384;

// Library textures the texture picker decodes per frame. Each decode runs on the GUI thread, so the
// rest of the visible tiles fill in over the following frames instead of in one stall.
constexpr int PICKER_LOADS_PER_FRAME = 2;
} // namespace

void GLGizmoTextureDisplacement::add_texture_layer(bool chroma)
{
    ModelVolume *mv = texture_volume();
    if (!mv)
        return;

    // One ChromaStack layer per object, refused here rather than at Apply: the swaps live in the
    // object's height ranges, of which there is one set, so a second stack could be solved and
    // previewed and would then silently lose its swaps to the first.
    if (chroma)
        for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.chroma_enabled) {
                show_error(nullptr, _u8L("This object already has a ChromaStack picture. An object can only have one: "
                                         "the filament swaps are the object's own height ranges, and there is one set "
                                         "of those."));
                return;
            }

    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> used{};
    for (const TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot >= 0 && size_t(l.slot) < used.size())
            used[size_t(l.slot)] = true;
    int free_slot = -1;
    for (size_t i = 0; i < used.size(); ++i)
        if (!used[i]) { free_slot = int(i); break; }
    if (free_slot < 0) {
        show_error(nullptr, _u8L("Maximum number of texture displacement layers reached."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add texture displacement layer"), UndoRedo::SnapshotType::GizmoAction);

    TextureDisplacementLayer layer;
    layer.slot = free_slot;
    // A model that came with a UV channel is mapped by it. Re-unwrapping such a model throws away the
    // layout it was authored against and can only be worse than using it, so the default follows the
    // model rather than the other way round; the Mapping row still offers every other method. Not for a
    // picture: its placement is the fit onto the painted face, which that mapping would ignore.
    if (!chroma && mv->has_texture_uv())
        layer.projection_method = TextureProjectionMethod::ModelUV;
    if (chroma) {
        // A ChromaStack layer's height map is solved, never picked, so it starts with no texture and
        // the card asks for a picture instead. Everything the solve writes is pinned here so the card
        // never has to show a control that would break it.
        layer.chroma_enabled = true;
        layer.name           = _u8L("ChromaStack picture");
        layer.tile_enabled   = false;
        layer.color_enabled  = false;
        layer.invert         = false;
        layer.midlevel       = 0.f;
        layer.smoothing      = 0.f;
        layer.blend_mode     = TextureBlendMode::Add;
    } else {
        // Start a relief layer off on the first library texture rather than on nothing at all: a
        // textureless layer looks broken (painting on it appears to do nothing, because there is no
        // height map to displace by). The user swaps it for another from the layer's own picker.
        const std::vector<TextureLibraryEntry> &library = texture_library();
        if (!library.empty())
            if (const LibraryTexture *tex = get_library_texture(library.front().path)) {
                layer.name       = library.front().name;
                layer.path       = library.front().path;
                layer.image_data = tex->image_data;
            }
    }
    mv->texture_displacement_layers.push_back(std::move(layer));
    // A new layer opens with every setting showing; there is nothing on it yet to hide settings from.
    m_layer_expanded[size_t(free_slot)] = true;

    set_active_layer(free_slot);
    // set_active_layer() is a no-op when the new slot happens to be the one already active (slot 0,
    // for the very first layer added), so the preview would not pick the new texture up on its own.
    rebuild_preview();
    m_parent.set_as_dirty();

    // The picture is the one thing a ChromaStack layer cannot do without, so choosing the kind goes
    // straight on to choosing it. Last, so the layer is in the model and active before the modal opens.
    if (chroma)
        if (TextureDisplacementLayer *added = active_layer())
            chroma_pick_picture(*added);
}

const GLGizmoTextureDisplacement::LibraryTexture *GLGizmoTextureDisplacement::get_library_texture(const std::string &path)
{
    if (auto it = m_library_textures.find(path); it != m_library_textures.end())
        return it->second.image_data ? &it->second : nullptr;

    LibraryTexture entry;
    std::string    error;
    entry.image_data = load_texture_image_data(path, error);
    if (entry.image_data) {
        TextureDisplacementLayer probe;
        probe.image_data = entry.image_data;
        entry.thumbnail  = upload_height_thumbnail(decode_height_texture(probe, /* keep_decoded */ false));
        if (!entry.thumbnail)
            entry.image_data.reset(); // decoded to nothing usable - treat it as a failed load
    } else {
        BOOST_LOG_TRIVIAL(error) << "Texture displacement: could not load texture " << path << ": " << error;
    }

    // Cached whether it loaded or not: a file that failed is remembered as unusable, so the picker
    // does not retry (and re-log) it on every frame it is on screen.
    const auto [pos, inserted] = m_library_textures.emplace(path, std::move(entry));
    return pos->second.image_data ? &pos->second : nullptr;
}

void GLGizmoTextureDisplacement::set_layer_texture(TextureDisplacementLayer &layer, const TextureLibraryEntry &entry)
{
    const LibraryTexture *tex = get_library_texture(entry.path);
    if (tex == nullptr) {
        show_error(nullptr, _u8L("Could not load the selected texture."));
        return;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change texture displacement texture"), UndoRedo::SnapshotType::GizmoAction);
    layer.name = entry.name;
    layer.path = entry.path;
    // Handing over the library's own buffer (rather than a copy) is what lets decode_height_texture()
    // - whose cache is keyed by exactly this pointer - hit straight away instead of re-decoding
    // the PNG the first time the layer is previewed or baked.
    layer.image_data = tex->image_data;
    // The user's own textures are listed most recently used first.
    if (entry.source == TextureSource::User)
        touch_user_texture(entry.path);

    rebuild_preview();
    m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::import_custom_texture(TextureDisplacementLayer &layer)
{
    const wxString wildcard = _L("Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp");
    wxFileDialog   dialog(nullptr, _L("Choose a texture image (height map)"), wxEmptyString, wxEmptyString, wildcard,
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    // Converts to the 8-bit grayscale PNG the bake code understands and copies it into the user's
    // own texture folder, so it stays available for later models (and survives an app update, which
    // rewrites the shipped folder wholesale).
    std::string                              error;
    const std::optional<TextureLibraryEntry> entry = import_texture_to_library(into_u8(dialog.GetPath()), error);
    if (!entry) {
        show_error(nullptr, error);
        return;
    }
    set_layer_texture(layer, *entry);
}

void GLGizmoTextureDisplacement::ease_wheel_scroll(float &target, float &applied)
{
    ImGuiIO    &io         = ImGui::GetIO();
    const float scroll_now = ImGui::GetScrollY();
    const float scroll_max = ImGui::GetScrollMaxY();

    // Anything that moved the scroll without us - dragging the grab, a keyboard/gamepad nav step, the
    // content shrinking under a clamped offset, a SetScroll*() call - has to re-seed the target, or the
    // easing below would immediately drag the view back to where it last animated to.
    if (applied < 0.f || std::abs(scroll_now - applied) > 0.5f)
        target = scroll_now;

    if (io.MouseWheel != 0.f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        target -= io.MouseWheel * ImGui::GetFontSize() * 4.f;
    // Whole pixels: ImGui floors whatever SetScrollY() is given, so a fractional target could never be
    // reached and the "still gliding" test below would stay true forever, repainting the canvas for good.
    target = std::floor(std::clamp(target, 0.f, scroll_max));

    const float delta = target - scroll_now;
    if (std::abs(delta) >= 1.f) {
        // Exponential ease, formulated against the frame time so the glide takes the same wall time
        // whether the canvas is running at 30 or 144 fps. The last sub-pixel step would be floored
        // away, so land on the target outright once the remainder is that small.
        const float t    = 1.f - std::exp(-20.f * std::clamp(io.DeltaTime, 1.f / 240.f, 1.f / 15.f));
        const float step = (std::abs(delta * t) < 1.f) ? delta : delta * t;
        const float next = std::floor(scroll_now + step);
        ImGui::SetScrollY(next);
        applied = next;
        m_parent.set_as_dirty(); // nothing else would redraw mid-glide once the mouse stops
    } else {
        applied = scroll_now;
    }
}

void GLGizmoTextureDisplacement::render_texture_library_popup(const ImVec2 &panel_min, const ImVec2 &panel_max)
{
    ModelVolume              *mv    = texture_volume();
    TextureDisplacementLayer *layer = nullptr;
    if (mv != nullptr)
        for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
            if (l.slot == m_picker_slot)
                layer = &l;

    static const char *const popup_id = "##texture_library";
    if (m_picker_open_request) {
        m_picker_open_request = false;
        if (layer != nullptr)
            ImGui::OpenPopup(popup_id);
    }

    const float       tile        = std::round(m_imgui->scaled(3.2f));
    const float       spacing     = std::round(m_imgui->scaled(0.4f));
    const float       grid_w      = 4.f * tile + 3.f * spacing;
    const float       gap         = m_imgui->scaled(0.5f);
    const float       scrollbar_w = m_imgui->scaled(0.5f);
    const float       ring_w      = std::max(2.f, std::round(m_imgui->scaled(0.1f))); // the current texture's ring
    const float       frame_h     = ImGui::GetFrameHeight();
    const ImVec4      orca        = ImGuiWrapper::COL_ORCA;
    const ImGuiStyle &style       = ImGui::GetStyle();

    // A fixed size rather than a fit-to-content one. The width is the four-column grid, a gutter and the
    // slim scrollbar beside it, plus the window padding, so the columns always fit and no horizontal
    // scrollbar is needed; it derives from scaled values, so it tracks the DPI like the rest.
    const float fixed_w = grid_w + spacing + scrollbar_w + 2.f * style.WindowPadding.x;

    // BeginPopup() ORs in ImGuiWindowFlags_AlwaysAutoResize (deps_src/imgui/imgui.cpp:8371) and no flag
    // turns it back off. A SetNextWindowSize() overrides it for each axis it sets (imgui.cpp:5983-5996),
    // so an explicit size is the only way to fix the popup's size; the grid child below then scrolls.
    //
    // Top-aligned with the panel, which keeps it clear of the gizmo toolbar above a docked panel, and as
    // tall as the panel or 70% of the window, whichever is taller, while keeping a gap inside the window.
    // The height follows the window and the panel, never the content, so it does not jump as packs load or
    // sections collapse.
    const ImGuiViewport *viewport  = ImGui::GetMainViewport();
    const float          vp_top    = viewport->Pos.y + gap;
    const float          vp_bottom = viewport->Pos.y + viewport->Size.y - gap;
    const float          height    = std::max(std::min(std::max(0.70f * viewport->Size.y, panel_max.y - panel_min.y), vp_bottom - vp_top),
                                              2.f * frame_h);
    const float          popup_y   = std::max(vp_top, std::min(panel_min.y, vp_bottom - height));
    // Beside the panel rather than over it, so the layer being retextured stays in view: to its left when
    // there is room (the docked panel sits against the right edge of the canvas), otherwise to its right.
    const float width   = viewport->Size.x > 0.f ? std::min(fixed_w, viewport->Size.x) : fixed_w;
    const bool  left    = panel_min.x - gap - width >= 0.f;
    const float popup_x = left ? panel_min.x - gap - width : panel_max.x + gap;
    // Cond_Always, not Appearing: a viewport resize while the popup is open must move and resize it too.
    ImGui::SetNextWindowPos(ImVec2(popup_x, popup_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    // Drawn as the panel's own sheet: its opaque background and rounding, no border. A popup is filled with
    // PopupBg, which the panel's toolbar style leaves at ImGui's translucent near-black - a dark window
    // beside a light panel, with whatever it covers showing through. Only Begin reads these, so they are
    // popped at once and the tooltips shown from inside keep their own look.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, style.WindowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
    // The window itself never scrolls; the grid child below does.
    const bool open = ImGui::BeginPopup(popup_id, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (!open)
        return;
    if (layer == nullptr) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    // While this popup is up its own tooltips belong beside it, not beside the panel it is already
    // covering the left of. Restored before every exit below.
    const ImVec2 prev_anchor_min = m_tooltip_anchor_min;
    const ImVec2 prev_anchor_max = m_tooltip_anchor_max;
    m_tooltip_anchor_min         = ImGui::GetWindowPos();
    m_tooltip_anchor_max         = ImVec2(m_tooltip_anchor_min.x + ImGui::GetWindowSize().x,
                                          m_tooltip_anchor_min.y + ImGui::GetWindowSize().y);

    // The header stays put while the grid scrolls under it: the title says what the popup is for, and the
    // close button is there for anyone who does not know that a click outside closes it.
    bool close_clicked = false;
    {
        ImDrawList  *hdl = ImGui::GetWindowDrawList();
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 0.5f * (frame_h - ImGui::GetTextLineHeight())));
        m_imgui->bold_text(_u8L("Choose a texture"));
        const ImVec2 btn(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - frame_h, row.y);
        ImGui::SetCursorScreenPos(btn);
        close_clicked       = ImGui::InvisibleButton("##close", ImVec2(frame_h, frame_h));
        const bool   hot    = ImGui::IsItemHovered();
        const ImVec2 c(btn.x + 0.5f * frame_h, btn.y + 0.5f * frame_h);
        const float  k      = std::round(0.18f * frame_h);
        const float  line_w = std::max(1.f, std::round(m_imgui->scaled(0.08f)));
        const ImU32  x_col  = ImGui::GetColorU32(ImGuiCol_Text, hot ? 1.f : 0.6f);
        if (hot)
            hdl->AddCircleFilled(c, 0.5f * frame_h, ImGui::GetColorU32(ImGuiCol_Text, 0.08f));
        hdl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), x_col, line_w);
        hdl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), x_col, line_w);
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + frame_h + style.ItemSpacing.y));
        ImGui::Separator();
    }

    const std::vector<TextureLibraryEntry> &library      = texture_library();
    // A reinstalled pack can drop new bytes at a path already cached on this gizmo, and get_library_texture()
    // caches a failure as well as a success, so a moved generation makes every cached entry suspect.
    if (const uint64_t generation = texture_library_generation(); generation != m_library_generation) {
        m_library_textures.clear();
        m_library_generation = generation;
    }

    // The grid scrolls as the panel body does: the same slim scrollbar, and the wheel eased by
    // ease_wheel_scroll() instead of ImGui's five-line jumps (hence NoScrollWithMouse).
    push_slim_scrollbar_style(scrollbar_w, wxGetApp().dark_mode());
    ImGui::BeginChild("##texture_grid", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_NoScrollWithMouse);
    ease_wheel_scroll(m_picker_scroll_target, m_picker_scroll_applied);

    ImDrawList                        *dl = ImGui::GetWindowDrawList(); // the child's, so tiles clip to the grid
    std::optional<TextureLibraryEntry> chosen;
    std::optional<TextureLibraryEntry> to_remove;
    bool                               import_clicked = false;
    bool                               download_clicked = false;
    int                                loads_left       = PICKER_LOADS_PER_FRAME;
    bool                               loads_deferred   = false;

    // Tiles are placed on the four-column grid explicitly rather than flowed with SameLine(): a user tile
    // overlays a remove button on its corner, and that button would otherwise be "the previous item" the
    // next tile lines up against.
    const auto tile_pos = [&](const ImVec2 &origin, int i) {
        return ImVec2(origin.x + float(i % 4) * (tile + spacing), origin.y + float(i / 4) * (tile + spacing));
    };
    // Leaves the cursor below a grid of `count` tiles, with the grid's area registered as content.
    const auto end_grid = [&](const ImVec2 &origin, int count) {
        ImGui::SetCursorScreenPos(origin);
        if (count > 0) {
            const int rows = (count + 3) / 4;
            ImGui::Dummy(ImVec2(grid_w, float(rows) * tile + float(rows - 1) * spacing));
        }
    };
    // One section's tiles, in the order the section holds them (shipped alphabetically, pack textures by
    // name, the user's own most recently used first - the order texture_library() produced); a user tile
    // carries a remove button on hover. Returns how many tiles it laid out, drawn or not.
    const auto grid = [&](const ImVec2 &origin, const std::vector<const TextureLibraryEntry *> &entries, bool removable) {
        int n = 0;
        for (const TextureLibraryEntry *entry : entries) {
            const ImVec2 rect_min = tile_pos(origin, n++);
            const ImVec2 rect_max = ImVec2(rect_min.x + tile, rect_min.y + tile);
            const bool   current  = entry->path == layer->path;
            // The selected tile is centred even when it is off screen, so it is drawn and loaded next frame.
            if (current && m_picker_reveal_selected) {
                ImGui::SetScrollFromPosY(0.5f * (rect_min.y + rect_max.y) - ImGui::GetWindowPos().y, 0.5f);
                m_parent.request_extra_frame();
            }
            if (!ImGui::IsRectVisible(rect_min, rect_max))
                continue;
            // Thumbnails decode on this thread, so only a few per frame; the rest fill in on later frames.
            const bool cached = m_library_textures.count(entry->path) != 0;
            if (!cached && loads_left == 0) {
                loads_deferred = true;
                dl->AddRectFilled(rect_min, rect_max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
                continue;
            }
            if (!cached)
                --loads_left;
            ImGui::SetCursorScreenPos(rect_min);
            ImGui::PushID(entry->path.c_str());
            const LibraryTexture *tex     = get_library_texture(entry->path);
            GLTexture            *thumb   = tex != nullptr ? tex->thumbnail.get() : nullptr;
            bool                  clicked = false;
            if (thumb != nullptr) {
                // Centre-crop a non-square image into the square tile instead of squashing it.
                const float aspect = thumb->get_height() > 0 ? float(thumb->get_width()) / float(thumb->get_height()) : 1.f;
                ImVec2      uv0(0.f, 0.f), uv1(1.f, 1.f);
                if (aspect > 1.f) {
                    uv0.x = 0.5f * (1.f - 1.f / aspect);
                    uv1.x = 1.f - uv0.x;
                } else if (aspect < 1.f) {
                    uv0.y = 0.5f * (1.f - aspect);
                    uv1.y = 1.f - uv0.y;
                }
                clicked = ImGui::ImageButton((ImTextureID) (intptr_t) thumb->get_id(), ImVec2(tile, tile), uv0, uv1, 0);
            } else {
                clicked = ImGui::Button("?", ImVec2(tile, tile));
            }
            if (removable)
                ImGui::SetItemAllowOverlap(); // the remove button drawn over its corner takes its own clicks
            const ImVec2 a       = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool   hovered = ImGui::IsItemHovered();
            // The layer's current texture carries an accent ring, and the tile under the pointer a thinner,
            // fainter one. Both are drawn inside the tile, where the grid's clip rect never cuts them.
            if (current || hovered) {
                const float w = current ? ring_w : std::round(0.5f * ring_w);
                dl->AddRect(ImVec2(a.x + 0.5f * w, a.y + 0.5f * w), ImVec2(b.x - 0.5f * w, b.y - 0.5f * w),
                            ImGui::GetColorU32(current ? orca : ImVec4(orca.x, orca.y, orca.z, 0.6f)), 0.f, 0, w);
            }
            if (hovered)
                panel_tooltip(entry->name);

            // Clipped by the grid's clip rect, so a tile scrolled out of the grid or under its scrollbar
            // does not offer its remove button. IsMouseHoveringRect() clips by default.
            if (removable && ImGui::IsMouseHoveringRect(a, b)) {
                const float  r = std::round(tile * 0.17f);
                const ImVec2 c(b.x - r - 2.f, a.y + r + 2.f);
                ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
                const bool remove_clicked = ImGui::InvisibleButton("##remove", ImVec2(2.f * r, 2.f * r));
                const bool hot            = ImGui::IsItemHovered();
                dl->AddCircleFilled(c, r, hot ? IM_COL32(214, 72, 64, 235) : IM_COL32(0, 0, 0, 170));
                const float k = 0.42f * r;
                dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32_WHITE, 1.5f);
                dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), IM_COL32_WHITE, 1.5f);
                if (hot)
                    panel_tooltip(_u8L("Remove from My textures"));
                if (remove_clicked)
                    to_remove = *entry;
            }
            ImGui::PopID();
            if (clicked)
                chosen = *entry;
        }
        return n;
    };

    // Every section collapses; the state is remembered for the app run. Pack keys use pack_name, a
    // capability name unique per type across loaded packages and stable across reinstalls. This function-local
    // static survives popup closes, never touches app_config, and resets on restart.
    static std::map<std::string, bool> s_section_collapsed; // true = collapsed; absent = expanded

    const auto split_pack_title = [](const std::string &title) -> std::pair<std::string, std::string> {
        size_t split = std::string::npos;
        for (size_t i = 0; i + 3 < title.size(); ++i)
            if (title[i] == ' ' && (title[i + 1] == 'b' || title[i + 1] == 'B') &&
                (title[i + 2] == 'y' || title[i + 2] == 'Y') && title[i + 3] == ' ')
                split = i;
        if (split == std::string::npos)
            return { title, {} };

        std::string name = title.substr(0, split);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
        std::string attribution = title.substr(split);
        size_t      first         = 0;
        while (first < attribution.size() && std::isspace(static_cast<unsigned char>(attribution[first])))
            ++first;
        attribution.erase(0, first);
        if (name.empty() || attribution.empty())
            return { title, {} };
        return { name, attribution };
    };

    // A section heading, one row as wide as the grid: a click anywhere on it collapses or expands the
    // section. Child headings indent their contents while keeping their counts right-aligned. Attribution
    // lines use a smaller disabled style; the grid follows closer than the next heading does.
    // Returns true when the section is open, so the caller knows whether to draw its contents.
    const auto section_header = [&](const std::string &key, const std::string &title, size_t count, float indent,
                                    const std::string &attribution, const std::string &full_title) {
        bool        &collapsed = s_section_collapsed[key]; // default-inserts false -> expanded
        const ImVec2 row       = ImGui::GetCursorScreenPos();
        const bool   two_lines = !attribution.empty();
        const float  line_h          = ImGui::GetTextLineHeight();
        const float  small_font_size = ImGui::GetFontSize() * 0.8f;
        const float  small_line_h    = line_h * 0.8f;
        const float  row_h           = two_lines ? std::max(frame_h, line_h + small_line_h + spacing) : frame_h;
        const ImVec2 row_max(row.x + grid_w, row.y + row_h);
        if (ImGui::InvisibleButton(key.c_str(), ImVec2(grid_w, row_h)))
            collapsed = !collapsed;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            dl->AddRectFilled(row, row_max, ImGui::GetColorU32(ImGuiCol_Text, 0.05f), style.FrameRounding);

        const float inset = std::round(0.25f * frame_h);
        const float arrow = std::round(ImGui::GetFontSize() * 0.55f);
        const float cx = row.x + inset + indent + 0.5f * arrow;
        const float cy = row.y + 0.5f * row_h;
        const float hs = 0.3f * arrow;
        const ImU32 accent = ImGui::GetColorU32(orca);
        if (collapsed)
            dl->AddTriangleFilled(ImVec2(cx - 0.5f * hs, cy - hs), ImVec2(cx + 0.5f * hs, cy), ImVec2(cx - 0.5f * hs, cy + hs), accent);
        else
            dl->AddTriangleFilled(ImVec2(cx - hs, cy - 0.5f * hs), ImVec2(cx + hs, cy - 0.5f * hs), ImVec2(cx, cy + 0.5f * hs), accent);

        const std::string n     = std::to_string(count);
        const float       n_w   = ImGui::CalcTextSize(n.c_str()).x;
        const float       tx    = row.x + inset + indent + arrow + spacing;
        const float       max_text_w = std::max(0.f, row_max.x - inset - n_w - spacing - tx);
        const std::string shown = ellipsize(title, max_text_w);
        const std::string shown_attribution = two_lines ? ellipsize(attribution, max_text_w, small_font_size) : std::string{};
        const float       title_y            = two_lines ? row.y + 0.5f * (row_h - line_h - small_line_h)
                                                         : row.y + 0.5f * (frame_h - line_h);
        const float       count_y            = row.y + 0.5f * (row_h - line_h);
        dl->AddText(ImVec2(tx, title_y), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        if (two_lines)
            dl->AddText(ImGui::GetFont(), small_font_size, ImVec2(tx, title_y + line_h),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), shown_attribution.c_str());
        dl->AddText(ImVec2(row_max.x - inset - n_w, count_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), n.c_str());
        if (hovered && (shown != title || shown_attribution != attribution))
            panel_tooltip(full_title); // a long plugin-supplied name, cut by the fixed width
        ImGui::SetCursorScreenPos(ImVec2(row.x, row_max.y + spacing));
        return !collapsed;
    };

    // Built-in is first, plugin packs are grouped under Texture libraries, and My
    // textures stays last so its import tile and help line describe that group. Pack names identify both
    // their child section and state.
    struct Section
    {
        std::string                              key;
        std::string                              title;
        std::vector<const TextureLibraryEntry *> entries;
    };
    std::vector<Section>          sections;
    std::map<std::string, size_t> pack_section; // pack_name -> index in `sections`
    sections.push_back({ "builtin:", _u8L("Built-in"), {} });
    for (const TextureLibraryEntry &entry : library) {
        if (entry.source == TextureSource::Shipped) {
            sections.front().entries.push_back(&entry); // the section exists even when the library is empty
            continue;
        }
        if (entry.source == TextureSource::User)
            continue; // added below, so My textures stays last
        if (entry.source != TextureSource::Plugin)
            continue;
        const auto [it, inserted] = pack_section.emplace(entry.pack_name, sections.size());
        if (inserted)
            sections.push_back({ "pack:" + entry.pack_name, entry.pack_name, {} });
        sections[it->second].entries.push_back(&entry);
    }
    sections.push_back({ "user:", _u8L("My textures"), {} });
    for (const TextureLibraryEntry &entry : library)
        if (entry.source == TextureSource::User)
            sections.back().entries.push_back(&entry);

    const float child_indent = std::round(m_imgui->scaled(0.8f));
    const Section &built_in = sections.front();
    if (section_header(built_in.key, built_in.title, built_in.entries.size(), 0.f, {}, built_in.title)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int    count  = grid(origin, built_in.entries, false);
        end_grid(origin, count);
        if (count == 0)
            ImGui::TextDisabled("%s", _u8L("No textures found.").c_str());
    }

    size_t subscribed_count = 0;
    for (size_t i = 1; i + 1 < sections.size(); ++i)
        subscribed_count += sections[i].entries.size();

    const std::string subscribed_title = _u8L("Texture libraries");
    if (section_header("subscribed:", subscribed_title, subscribed_count, 0.f, {}, subscribed_title)) {
        for (size_t i = 1; i + 1 < sections.size(); ++i) {
            const Section &pack = sections[i];
            const auto [name, attribution] = split_pack_title(pack.title);
            if (!section_header(pack.key, name, pack.entries.size(), child_indent, attribution, pack.title))
                continue;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const int    count  = grid(origin, pack.entries, false);
            end_grid(origin, count);
        }

        ImGui::Dummy(ImVec2(0.f, spacing));
        const std::string download_label = _u8L("Explore more texture libraries");
        if (ImGui::Button(download_label.c_str(), ImVec2(grid_w, frame_h)))
            download_clicked = true;
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Opens OrcaCloud in the Home page to browse for plugins."));
    }

    const Section &user = sections.back();
    if (section_header(user.key, user.title, user.entries.size(), 0.f, {}, user.title)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int    count  = grid(origin, user.entries, true);
        ImGui::SetCursorScreenPos(tile_pos(origin, count));
        if (ImGui::Button("+##import_texture", ImVec2(tile, tile)))
            import_clicked = true;
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Import your own image as a height map. It is converted to the format the slicer "
                               "bakes from and saved to your personal texture folder, kept separate from the "
                               "textures shipped with OrcaSlicer."));
        end_grid(origin, count + 1);
        // Belongs to this section: it describes the import tile and the remove-on-hover tiles, both of
        // which live here, so it is hidden with them rather than left describing controls off-screen.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + grid_w);
        ImGui::TextDisabled("%s", _u8L("PNG, JPG or BMP - converted to a height map on import. Hover one of yours to remove it.").c_str());
        ImGui::PopTextWrapPos();
    }
    if (loads_deferred)
        m_parent.request_extra_frame();
    // Done whether or not the current texture was drawn: one in a collapsed section stays where it is.
    m_picker_reveal_selected = false;
    ImGui::EndChild();
    pop_slim_scrollbar_style();

    if (chosen || import_clicked || close_clicked || download_clicked)
        ImGui::CloseCurrentPopup();
    m_tooltip_anchor_min = prev_anchor_min;
    m_tooltip_anchor_max = prev_anchor_max;
    ImGui::EndPopup();
    // The OrcaCloud plugin hub filtered to resource textures; the pane's session handoff redirects here.
    if (download_clicked)
        wxGetApp().open_orca_cloud("/app/plugins/plugin-hub?types=resource-textures");

    // Only once the loop over the library is done: importing, picking (which reorders the recently used)
    // and removing all change the library the loop was iterating.
    if (chosen)
        set_layer_texture(*layer, *chosen);
    else if (import_clicked)
        import_custom_texture(*layer);
    else if (to_remove) {
        MessageDialog dlg(nullptr,
                          wxString::Format(_L("Remove \"%s\" from My textures?\n\nThe image file is deleted from your "
                                              "texture folder. Layers already using it keep it."),
                                           from_u8(to_remove->name)),
                          _L("Remove texture"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() == wxID_YES) {
            std::string error;
            if (remove_user_texture(to_remove->path, error))
                m_library_textures.erase(to_remove->path); // its cached thumbnail and bytes go with it
            else
                show_error(nullptr, error);
        }
        // The modal dialog took the focus, which closes the popup; the user was choosing a texture and
        // still is, so bring it back.
        m_picker_open_request = true;
    }
}
// One filter: a name, what it does to the five adjustments, and when it is the right one.
//
// The set is chosen for what a filament print needs rather than for what a photo editor offers. A
// ChromaStack palette is a few dozen colours reachable from a handful of filaments, so the useful moves
// are the ones that fit a picture into that: more contrast where the stack has range to spare, less
// where the picture already clips, saturation towards or away from what the filaments can reach, and
// the spatial pair for a source that is noisy or soft. Nothing here is a look for its own sake.
namespace {
struct TextureFilterPreset
{
    wxString           name;
    std::string        when; // the tooltip: the picture this is the right answer for
    TextureAdjustments adjust;
};

std::vector<TextureFilterPreset> texture_filter_presets()
{
    const auto make = [](const wxString &name, const std::string &when, float brightness, float contrast,
                         float sharpness, float blur, float hue_deg, float saturation) {
        TextureFilterPreset p;
        p.name              = name;
        p.when              = when;
        p.adjust.brightness = brightness;
        p.adjust.contrast   = contrast;
        p.adjust.sharpness  = sharpness;
        p.adjust.blur       = blur;
        p.adjust.hue_deg    = hue_deg;
        p.adjust.saturation = saturation;
        return p;
    };

    // TRN These are image filters, in the sense a phone camera means: one click instead of five sliders.
    return {
        make(_L("Original"), _u8L("The picture as it is, with nothing done to it."),
             0.f, 0.f, 0.f, 0.f, 0.f, 0.f),
        make(_L("Bold"), _u8L("A little more contrast, colour and bite. The one to try first: a stack of a "
                              "few filaments has less range than a screen, and a picture that looks right "
                              "on one usually prints flat."),
             0.02f, 0.30f, 0.15f, 0.f, 0.f, 0.15f),
        make(_L("Poster"), _u8L("Hard contrast and strong colour, for a graphic look in few colours. Suits "
                                "line art, logos and anything you want to read from across the room."),
             0.f, 0.55f, 0.30f, 0.f, 0.f, 0.35f),
        make(_L("Soft"), _u8L("Less contrast and a touch of blur, for a grainy or noisy photograph. Speckle "
                              "becomes single-layer freckles in a print, and this is what removes it."),
             0.f, -0.12f, 0.f, 0.18f, 0.f, -0.05f),
        make(_L("Lift"), _u8L("Brighter, with the shadows opened up. For a dark picture, where the stack "
                              "runs out of dark colours and everything in shadow prints as the same black."),
             0.18f, -0.10f, 0.05f, 0.f, 0.f, 0.05f),
        make(_L("Deepen"), _u8L("Darker and firmer. For a washed-out or overexposed picture, which otherwise "
                                "prints as the top filament almost everywhere."),
             -0.12f, 0.22f, 0.10f, 0.f, 0.f, 0.10f),
        make(_L("Crisp"), _u8L("Detail only, with the tones left alone. For a slightly soft scan or a "
                               "picture you have already graded."),
             0.f, 0.f, 0.45f, 0.f, 0.f, 0.f),
        make(_L("Monochrome"), _u8L("All colour removed, contrast firmed up. This is the filter for a print "
                                    "in two filaments, where the picture is tone and nothing else."),
             0.f, 0.18f, 0.10f, 0.f, 0.f, -1.f),
        make(_L("Warmer"), _u8L("Turned towards red and yellow. Useful when the filaments you own are warmer "
                                "than the picture, which is otherwise a mismatch no number of layers fixes."),
             0.02f, 0.10f, 0.f, 0.f, -10.f, 0.12f),
        make(_L("Cooler"), _u8L("Turned towards green and blue, for the same reason in the other direction."),
             0.f, 0.10f, 0.f, 0.f, 12.f, 0.08f),
    };
}

// Big enough that a face in a thumbnail is a face, small enough that ten filter passes are a few
// milliseconds all together: each is run on the CPU the moment the panel opens.
constexpr int FILTER_THUMB_PX = 128;
} // namespace

// One thumbnail per filter, each this picture under it: the adjust window's row and the ChromaStack card's
// both show exactly this.
static std::vector<std::unique_ptr<GLTexture>> make_filter_thumbnails(const DecodedHeightTexture            &base,
                                                                      const std::vector<TextureFilterPreset> &presets)
{
    std::vector<std::unique_ptr<GLTexture>> out;
    out.reserve(presets.size());
    for (const TextureFilterPreset &preset : presets)
        out.push_back(preset.adjust.identity() ?
                          upload_height_thumbnail(base, FILTER_THUMB_PX) :
                          upload_height_thumbnail(apply_texture_adjustments(base, preset.adjust), FILTER_THUMB_PX));
    return out;
}

void GLGizmoTextureDisplacement::rebuild_filter_thumbnails()
{
    const std::vector<TextureFilterPreset> presets = texture_filter_presets();
    if (m_adjust_preview_source.empty()) {
        m_filter_thumbs.clear();
        m_filter_thumb_source = nullptr;
        return;
    }
    // Already built for this picture. The thumbnails show the filters, not the sliders, so nothing the
    // user does to the sliders invalidates them - only the panel being pointed at another image does.
    if (m_filter_thumb_source == m_adjust_source_image && m_filter_thumbs.size() == presets.size())
        return;

    // From the preview copy rather than the full-size image: it is already decoded and already small,
    // and a 128 px thumbnail cannot show what the difference between the two would be.
    m_filter_thumbs       = make_filter_thumbnails(downscale_height_texture(m_adjust_preview_source, FILTER_THUMB_PX), presets);
    m_filter_thumb_source = m_adjust_source_image;
}

bool GLGizmoTextureDisplacement::adjust_session_on(const TextureDisplacementLayer &layer) const
{
    if (m_adjust_slot < 0 || layer.slot != m_adjust_slot)
        return false;
    const ModelVolume *mv = texture_volume();
    return mv != nullptr && mv->id().id == m_adjust_volume_id;
}

void GLGizmoTextureDisplacement::sync_image_adjust(TextureDisplacementLayer &layer)
{
    // A ChromaStack picture's own image is the height map its solve wrote, and preview_adjust is applied to
    // whatever a layer's image is: a filter handed over that way sharpened the answer rather than the
    // question. The solve reads the sliders itself instead (chroma_live_adjust()), so a change asks for a
    // solve, which also brings the card's thumbnails and the model along.
    if (layer.chroma_enabled && layer.chroma_source) {
        if (!layer.preview_adjust.identity())
            layer.preview_adjust = TextureAdjustments{};
        if (m_adjust != m_adjust_applied) {
            m_adjust_applied = m_adjust;
            chroma_touch(layer.slot);
            m_parent.set_as_dirty();
        }
        return;
    }

    // Hand the sliders to the layer, which is what puts the adjusted image in front of the displacement:
    // decode_height_texture() applies preview_adjust, so the relief, the colour and the thumbnails all
    // follow from here. Written every frame rather than only on a change, so an undo snapshot or a
    // reselection that resets the layer cannot leave the model showing an adjustment the panel has moved
    // on from.
    layer.preview_adjust = m_adjust;
    if (m_adjust != m_adjust_applied) {
        m_adjust_applied = m_adjust;
        invalidate_adjust_textures();
        // Marked dirty rather than rebuilt here: the panel's own flag is consumed by the main panel's
        // "rebuild once the mouse is released" rule, so dragging a slider recomputes the displacement
        // once, on release, instead of on every frame of the drag. That is what keeps the UI responsive
        // while a full-size resample and a mesh displacement happen behind it.
        m_preview_params_dirty = true;
        // The window is drawn after the block that consumes that flag, so the rebuild happens on a later
        // frame - which means there has to *be* one. Cheap insurance: a slider release that generated no
        // further canvas event would otherwise leave the flag set and the model unchanged until something
        // unrelated redrew.
        m_parent.set_as_dirty();
    }
}

void GLGizmoTextureDisplacement::render_chroma_filters(TextureDisplacementLayer &layer, float width)
{
    if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS || !layer.chroma_source)
        return;
    const size_t                           slot    = size_t(layer.slot);
    const std::vector<TextureFilterPreset> presets = texture_filter_presets();
    const float                            gap     = std::round(m_imgui->scaled(0.4f));
    const float                            frame_h = ImGui::GetFrameHeight();

    // The thumbnails, once per picture: they show the filters, not whatever the sliders say, so only
    // another picture invalidates them. The decode is the cached one the solve uses. A grayscale picture
    // gets none, and the key is what stops that being worked out again every frame.
    if (m_chroma_filter_key[slot] != layer.chroma_source.get()) {
        // The thumbnail-sized copy the source thumbnail is made from too (FILTER_THUMB_PX is that size).
        const DecodedHeightTexture &base = chroma_thumb_base(layer);
        m_chroma_filter_thumbs[slot].clear();
        // Only for a colour picture, as in the window: a filter is a look, and most of a look is colour.
        if (!base.empty() && base.has_color())
            m_chroma_filter_thumbs[slot] = make_filter_thumbnails(base, presets);
        m_chroma_filter_key[slot] = layer.chroma_source.get();
    }
    const bool has_color = m_chroma_filter_thumbs[slot].size() == presets.size();

    // Whether the adjustment session is this picture's. Slots are numbered per volume, so the volume it
    // was opened on is part of the answer.
    const auto ours = [&]() { return adjust_session_on(layer); };
    // Every change below goes through here: the first one opens the session on this picture with its
    // window out of the way - the card is the panel - and the solve follows from then on.
    const auto apply = [&](const TextureAdjustments &adj) {
        if (!ours())
            open_image_adjust(layer);
        if (ours())
            m_adjust = adj;
    };
    const TextureAdjustments current = ours() ? m_adjust : TextureAdjustments{};

    // ---- The filters, in one row ----
    //
    // One row that scrolls sideways, as a phone camera has them: the picture is what is being looked at,
    // and a block of ten thumbnails under it pushed the rest of the step off the panel. Each is cropped
    // square from the middle of the picture, which is where the subject of most pictures is.
    if (has_color) {
        const float cell    = std::round(frame_h * 2.5f);
        const float label_h = ImGui::GetTextLineHeight();
        const float bar     = std::round(m_imgui->scaled(0.45f));
        push_slim_scrollbar_style(bar, wxGetApp().dark_mode());
        ImGui::BeginChild("##chroma_filters", ImVec2(width, cell + label_h + bar + 1.f), false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        // The wheel scrolls the row, a cell at a time, while the pointer is on it: a row that only scrolls
        // by its thin bar is a row whose end nobody finds. The notch is used up here, so the panel body,
        // which reads the wheel after its content (see ease_wheel_scroll()), does not scroll with it.
        if (ImGui::IsWindowHovered()) {
            ImGuiIO &io = ImGui::GetIO();
            if (io.MouseWheel != 0.f) {
                ImGui::SetScrollX(std::clamp(ImGui::GetScrollX() - io.MouseWheel * (cell + gap), 0.f, ImGui::GetScrollMaxX()));
                io.MouseWheel = 0.f;
                m_parent.set_as_dirty();
            }
        }
        ImDrawList  *dl    = ImGui::GetWindowDrawList();
        const float  round = std::round(cell * 0.12f);
        for (size_t i = 0; i < presets.size(); ++i) {
            if (i > 0)
                ImGui::SameLine(0.f, gap);
            ImGui::PushID(int(i));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            // The filter as it would be applied: the brightness mode survives a pick, as it does in the
            // window, which is also what makes it read as chosen once picked.
            TextureAdjustments preset = presets[i].adjust;
            preset.brightness_space   = current.brightness_space;
            if (ImGui::InvisibleButton("##chroma_filter", ImVec2(cell, cell + label_h)))
                apply(preset);
            const bool hot = ImGui::IsItemHovered();
            if (hot)
                panel_tooltip(into_u8(presets[i].name) + "\n\n" + presets[i].when);
            const bool   chosen = current == preset;
            const ImVec2 br(at.x + cell, at.y + cell);
            if (const GLTexture *tex = m_chroma_filter_thumbs[slot][i].get(); tex != nullptr) {
                const float tw = float(std::max(1, tex->get_width())), th = float(std::max(1, tex->get_height()));
                const float cu = tw > th ? 0.5f * (1.f - th / tw) : 0.f;
                const float cv = th > tw ? 0.5f * (1.f - tw / th) : 0.f;
                dl->AddImageRounded((ImTextureID) (intptr_t) tex->get_id(), at, br, ImVec2(cu, cv),
                                    ImVec2(1.f - cu, 1.f - cv), IM_COL32_WHITE, round);
            }
            dl->AddRect(at, br,
                        ImGui::GetColorU32(chosen ? ImGuiWrapper::COL_ORCA : ImGui::GetStyleColorVec4(ImGuiCol_Separator)),
                        round, 0, chosen ? 2.f : 1.f);
            if (hot && !chosen)
                dl->AddRect(at, br, ImGui::GetColorU32(ImGuiCol_Text), round, 0, 1.f);
            // The name under its picture, centred when it fits and cut at the cell's edge when it does
            // not, so a long translation never runs into its neighbour.
            const std::string name = into_u8(presets[i].name);
            const float       nw   = ImGui::CalcTextSize(name.c_str()).x;
            dl->PushClipRect(ImVec2(at.x, br.y), ImVec2(br.x, br.y + label_h), true);
            dl->AddText(ImVec2(nw < cell ? std::round(at.x + 0.5f * (cell - nw)) : at.x, br.y),
                        ImGui::GetColorU32(ImGuiCol_Text, chosen ? 1.f : (hot ? 0.9f : 0.6f)), name.c_str());
            dl->PopClipRect();
            ImGui::PopID();
        }
        ImGui::EndChild();
        pop_slim_scrollbar_style();
    }

    // ---- The sliders behind them ----
    //
    // On the card rather than in a window of their own: a filter is a starting point, and the sliders
    // are where it is taken from there, so they belong under the filters they start from.
    {
        TextureAdjustments adj = current;
        ImGui::TextDisabled("%s", _u8L("Fine-tune").c_str());
        const auto row = [&](const char *id, const wxString &label, float *v, float v_min, float v_max,
                             const char *format, bool enabled, const wxString &tip) {
            const float x0 = ImGui::GetCursorPosX();
            slider_label(label);
            const float item_w = std::max(m_imgui->scaled(2.f), width - (ImGui::GetCursorPosX() - x0) - panel_edit_icon_room());
            m_imgui->disabled_begin(!enabled);
            typed_slider(id, item_w, v, v_min, v_max, format, false, tip);
            m_imgui->disabled_end();
        };
        row("##chroma_adj_brightness", _L("Brightness"), &adj.brightness, -1.f, 1.f, "%.2f", true, {});
        row("##chroma_adj_contrast", _L("Contrast"), &adj.contrast, -1.f, 1.f, "%.2f", true, {});
        row("##chroma_adj_sharpness", _L("Sharpness"), &adj.sharpness, 0.f, 1.f, "%.2f", true,
            _L("Brings out the finest detail in the image. Applied after Blur, so the two can be used together: "
               "blur away the speckle, then sharpen the edges that remain."));
        row("##chroma_adj_blur", _L("Blur"), &adj.blur, 0.f, 1.f, "%.2f", true,
            _L("Blurs the picture. Speckle in a photograph prints as single-layer freckles, and this is what "
               "removes it."));
        row("##chroma_adj_hue", _L("Hue"), &adj.hue_deg, -180.f, 180.f, "%.0f°", has_color, {});
        row("##chroma_adj_saturation", _L("Saturation"), &adj.saturation, -1.f, 1.f, "%.2f", has_color,
            _L("How strong the colours are. A stack of a few filaments reaches a limited set of colours, and "
               "pulling the picture towards what it can reach is often the difference between a print that "
               "looks like the picture and one that does not."));
        bool perceptual = adj.brightness_space == TextureBrightnessSpace::PerceptualLab;
        if (m_imgui->bbl_checkbox(_L("Keep the colors while brightening"), perceptual))
            adj.brightness_space = perceptual ? TextureBrightnessSpace::PerceptualLab : TextureBrightnessSpace::Rgb;
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Brightens by lightness alone, so a red stays red instead of drifting toward pink as "
                               "it lightens. Off, the brighten is the plain per-channel one."));
        if (adj != current)
            apply(adj);
    }

    // What to do with a change being tried: keep it, which writes the picture to the gallery and points
    // the layer at it, or drop it. Until then the readiness line says so, and Bake waits. Only while
    // there is something to keep, so the step ends with the sliders the rest of the time.
    if (ours() && !m_adjust.identity()) {
        ImGui::Dummy(ImVec2(1.f, gap));
        const float half = std::floor(0.5f * (width - gap));
        GLGizmoUtils::push_orca_button_style();
        if (ImGui::Button(_u8L("Keep").c_str(), ImVec2(half, frame_h)))
            commit_image_adjust(layer);
        GLGizmoUtils::pop_orca_button_style();
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Saves the picture with this filter to your pictures as a new one and prints that. "
                               "The picture you started from stays where it was."));
        ImGui::SameLine(0.f, gap);
        if (ImGui::Button(_u8L("Undo").c_str(), ImVec2(half, frame_h)))
            close_image_adjust();
        if (ImGui::IsItemHovered())
            panel_tooltip(_u8L("Drops the filter and goes back to the picture as it was."));
    }
}

bool GLGizmoTextureDisplacement::load_image_adjust_source(const TextureDisplacementLayer &layer)
{
    const std::shared_ptr<std::vector<unsigned char>> &image = adjust_image_of(layer);
    if (!image || image->empty())
        return false;

    // The *raw* image: neither the layer's Smoothing nor the panel's own preview_adjust, so what the
    // sliders act on is always the image as it is actually stored rather than the last preview of it.
    // Without clearing preview_adjust here the adjustment would compound on itself every reload.
    TextureDisplacementLayer raw = layer;
    raw.image_data               = image;
    raw.smoothing                = 0.f;
    raw.preview_adjust           = TextureAdjustments{};

    DecodedHeightTexture decoded = decode_height_texture(raw);
    if (decoded.empty())
        return false;

    m_adjust_source         = std::move(decoded);
    m_adjust_preview_source = downscale_height_texture(m_adjust_source, ADJUST_PREVIEW_MAX_PX);
    m_adjust_source_image   = image.get();
    // The unadjusted image, so the box is filled now rather than one background pass later. Pairing it
    // with an identity `shown` is what makes update_image_adjust_preview() relaunch for the sliders'
    // current values against the new source instead of deciding it is already showing them.
    m_adjust_preview_shown = TextureAdjustments{};
    m_adjust_preview_tex   = upload_height_thumbnail(m_adjust_preview_source, ADJUST_PREVIEW_MAX_PX);
    m_adjust_preview_valid = m_adjust_preview_tex != nullptr;
    return true;
}

void GLGizmoTextureDisplacement::open_image_adjust(const TextureDisplacementLayer &layer)
{
    if (!adjust_image_of(layer) || layer.slot < 0)
        return;

    close_image_adjust(); // reverts and clears any previous panel

    if (!load_image_adjust_source(layer))
        return;

    m_adjust               = TextureAdjustments{};
    m_adjust_applied       = TextureAdjustments{};
    m_adjust_slot          = layer.slot;
    const ModelVolume *mv  = texture_volume();
    m_adjust_volume_id     = mv != nullptr ? mv->id().id : 0;
}

void GLGizmoTextureDisplacement::invalidate_adjust_textures()
{
    // The GPU copies of a layer's image - the list thumbnail, the preview shader's height and colour
    // textures - are keyed on the image_data pointer and the layer's Smoothing, and a live adjustment
    // changes neither: the pointer still names the same PNG. Rather than widen three cache keys for a
    // panel that is open a few seconds at a time, the panel drops the copies itself whenever it moves
    // the adjustment. Resetting the source is enough; the next get_layer_*() rebuilds from the decode,
    // which does see the adjustment.
    m_thumbnail_source.fill({});
    m_color_tex_source.reset();
    m_height_tex_source.reset();
}

void GLGizmoTextureDisplacement::close_image_adjust()
{
    // Revert the layer's live adjustment. Every exit that reaches here is a discard: a commit resets
    // preview_adjust itself, before pointing the layer at the file that already carries the edit, so this
    // cannot undo a committed adjustment.
    //
    // Found by the id recorded when the panel opened rather than through texture_volume(), which answers
    // for the *current* selection. One of the ways to get here is the selection having moved on, and
    // reverting the newly selected object's layer instead would both miss the adjustment that is still
    // live on the original volume and clear one the user never touched.
    bool reverted = false;
    // A ChromaStack picture carries its filter in its solve rather than on the layer (see
    // sync_image_adjust()), so there is nothing to revert on the layer, but the solve has to run again
    // on the picture without it.
    int resolve_slot = -1;
    if (m_adjust_slot >= 0 && m_adjust_volume_id != 0)
        for (ModelObject *mo : wxGetApp().model().objects) {
            for (ModelVolume *mv : mo->volumes) {
                if (mv->id().id != m_adjust_volume_id)
                    continue;
                for (TextureDisplacementLayer &l : mv->texture_displacement_layers) {
                    if (l.slot != m_adjust_slot)
                        continue;
                    if (!l.preview_adjust.identity()) {
                        l.preview_adjust = TextureAdjustments{};
                        reverted         = true;
                    }
                    if (l.chroma_enabled && !m_adjust_applied.identity())
                        resolve_slot = l.slot;
                }
            }
        }

    if (reverted)
        invalidate_adjust_textures();

    // std::async's future blocks here until the pass finishes. That is a few milliseconds on an image
    // this small, and the alternative - a detached thread writing into the members of a closed panel -
    // is worse.
    if (m_adjust_preview_job.valid())
        m_adjust_preview_job.wait();
    m_adjust_preview_job = {};

    m_adjust_slot           = -1;
    m_adjust                = TextureAdjustments{};
    m_adjust_applied        = TextureAdjustments{};
    m_adjust_source         = DecodedHeightTexture{};
    m_adjust_preview_source = DecodedHeightTexture{};
    m_adjust_source_image   = nullptr;
    m_adjust_volume_id      = 0;
    m_adjust_preview_tex.reset();
    m_adjust_preview_valid = false;
    m_filter_thumbs.clear();
    m_filter_thumb_source = nullptr;

    // Only while the gizmo is up: on_shutdown() also comes through here, and rebuilding a preview for a
    // gizmo that is closing would be work for a model nobody is looking at. The reverted layer data is
    // what matters there, and that is already done above.
    if (reverted && m_state == On) {
        rebuild_preview();
        m_parent.set_as_dirty();
    }
    // After the session is gone, so the solve it asks for reads the picture without the filter.
    if (resolve_slot >= 0 && m_state == On) {
        chroma_touch(resolve_slot);
        m_parent.set_as_dirty();
    }
}

void GLGizmoTextureDisplacement::update_image_adjust_preview()
{
    if (m_adjust_preview_source.empty())
        return;

    // Collect a finished pass. wait_for(0) rather than get() outright: this runs inside the frame, and
    // blocking on the worker here would hand back to the UI thread the very stall the worker exists to
    // spare it.
    if (m_adjust_preview_job.valid() &&
        m_adjust_preview_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const DecodedHeightTexture adjusted = m_adjust_preview_job.get();
        m_adjust_preview_job                = {};
        if (std::unique_ptr<GLTexture> tex = upload_height_thumbnail(adjusted, ADJUST_PREVIEW_MAX_PX)) {
            m_adjust_preview_tex   = std::move(tex);
            m_adjust_preview_shown = m_adjust_preview_running;
            m_adjust_preview_valid = true;
        }
    }

    // One pass at a time: whatever the sliders did while it ran is picked up by the next launch, so a
    // drag collapses to the value the sliders ended on instead of queuing one pass per frame.
    if (!m_adjust_preview_job.valid() && m_adjust != m_adjust_preview_shown) {
        m_adjust_preview_running = m_adjust;
        // Captured by value: the pass must not read members the UI thread goes on editing while it runs.
        m_adjust_preview_job = std::async(std::launch::async,
                                          [src = m_adjust_preview_source, adj = m_adjust_preview_running] {
                                              return apply_texture_adjustments(src, adj);
                                          });
    }

    // A pass in flight has no other way to get its result on screen: without a redraw request the canvas
    // can sit idle, and the preview would only catch up on the next unrelated frame.
    if (m_adjust_preview_job.valid())
        m_parent.set_as_dirty();
}

void GLGizmoTextureDisplacement::commit_image_adjust(TextureDisplacementLayer &layer)
{
    if (m_adjust_source.empty() || m_adjust.identity()) {
        // Nothing was changed, so there is nothing to save.
        close_image_adjust();
        return;
    }

    // The full-size pass, once: 13-19 ms grayscale / 40-60 ms colour on a 1024x1024 with every knob
    // engaged. Run on the UI thread deliberately, unlike the live preview: this is a single click, and the
    // rebuild that follows it - a CPU mesh displacement of the painted patch - is already an order of
    // magnitude more work, synchronously, so moving only this off the thread would buy nothing a user
    // could perceive.
    const DecodedHeightTexture adjusted = apply_texture_adjustments(m_adjust_source, m_adjust);

    // A ChromaStack picture is a photograph, and it goes where photographs go: a new file in the
    // picture gallery, with the layer pointed at it. The same shape as the relief path below - the
    // edit becomes an asset of its own rather than overwriting what it came from, so the original is
    // still there to go back to and the edited one can be used again on another plate.
    if (layer.chroma_enabled && layer.chroma_source) {
        if (!chroma_commit_adjusted_picture(layer, adjusted))
            return; // the panel stays open and the layer keeps the live adjustment: the edit is not lost
        close_image_adjust();
        return;
    }

    std::string                              error;
    const std::string                        stem  = layer.name.empty() ? std::string("texture") : layer.name;
    const std::optional<TextureLibraryEntry> entry = save_texture_to_library(adjusted, stem, error);
    if (!entry) {
        show_error(nullptr, error);
        return; // the panel stays open, and the layer keeps the live adjustment: the edit is not lost
    }

    // Cleared *before* the layer is pointed at the new file, and this ordering is the whole correctness of
    // the commit: the saved image already has the adjustment baked into its pixels, so leaving
    // preview_adjust set would apply brightness, contrast and the rest a second time on top of itself.
    layer.preview_adjust = TextureAdjustments{};
    // set_layer_texture() does the rest - the name, the recently-used ordering, a fresh image_data buffer
    // (which is what invalidates every cache keyed by the old pointer), the undo snapshot and the rebuild.
    set_layer_texture(layer, *entry);

    close_image_adjust();
}

void GLGizmoTextureDisplacement::render_image_adjust_window(const ImVec2 &panel_min, const ImVec2 &panel_max)
{
    if (m_adjust_slot < 0)
        return;

    ModelVolume *mv = texture_volume();
    // A different volume is selected than the one this panel was opened on. Slots are numbered per
    // volume, so looking m_adjust_slot up here would find an unrelated layer rather than nothing.
    if (mv == nullptr || mv->id().id != m_adjust_volume_id) {
        close_image_adjust();
        return;
    }

    TextureDisplacementLayer *layer = nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == m_adjust_slot)
            layer = &l;
    if (layer == nullptr) {
        // The layer went away under the panel (removed, or merged away by an undo).
        close_image_adjust();
        return;
    }

    // The layer's image can be swapped from the texture picker while this panel is open - the panel is a
    // window now, so the rest of the gizmo stays usable behind it. Re-read the source when that happens,
    // keeping the slider positions: the user is adjusting this layer, and the 3D view has already moved to
    // the new image (decode_height_texture() reads layer.image_data, which changed), so re-reading here is
    // also what keeps the panel's photo and the model showing the same thing.
    if (adjust_image_of(*layer).get() != m_adjust_source_image && !load_image_adjust_source(*layer)) {
        close_image_adjust(); // the new image cannot be decoded; nothing left to adjust
        return;
    }

    // A ChromaStack picture's adjustment is made on its card - the Picture step's filters and sliders are
    // this window's whole content - so the window is not drawn. The sliders still have to reach the
    // picture every frame.
    if (layer->chroma_enabled && layer->chroma_source) {
        sync_image_adjust(*layer);
        return;
    }

    const float img_w = std::round(m_imgui->scaled(16.f));
    const float gap   = m_imgui->scaled(0.5f);
    // Placed beside the panel on first appearance only, and left wherever the user drags it after that -
    // it is a window, not a popup, so its position is theirs to keep.
    const float win_w = img_w + 2.f * ImGui::GetStyle().WindowPadding.x;
    const bool  left  = panel_min.x - gap - win_w >= 0.f;
    ImGui::SetNextWindowPos(ImVec2(left ? panel_min.x - gap - win_w : panel_max.x + gap,
                                   panel_min.y + m_imgui->scaled(4.f)), ImGuiCond_Appearing);

    // The window sizes itself to its content, and its content is a picture plus ten filter tiles plus
    // six sliders. An auto-resizing window gets no scrollbar from ImGui, so once that total passes the
    // height of the canvas everything below the fold is simply unreachable - which is how the colour
    // sliders went missing under a tall picture. Constrained to the viewport, the window scrolls
    // instead of running off it.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f),
                                        ImVec2(ImGui::GetIO().DisplaySize.x,
                                               std::max(m_imgui->scaled(12.f),
                                                        ImGui::GetIO().DisplaySize.y - m_imgui->scaled(2.f))));

    bool keep_open = true;
    if (!ImGui::Begin((_u8L("Adjust image") + "##texture_adjust").c_str(), &keep_open,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!keep_open)
            close_image_adjust();
        return;
    }

    // Its sliders are the panel's rows, so their tooltips go through panel_tooltip() - which places a
    // tooltip beside the anchor rectangle rather than under the mouse. Beside this window while it is
    // up, as the texture picker does, or the tooltip would be placed beside the panel and land on top
    // of the window it is explaining. Restored before End() below; this function has no other exit.
    const ImVec2 prev_anchor_min = m_tooltip_anchor_min;
    const ImVec2 prev_anchor_max = m_tooltip_anchor_max;
    m_tooltip_anchor_min         = ImGui::GetWindowPos();
    m_tooltip_anchor_max         = ImVec2(m_tooltip_anchor_min.x + ImGui::GetWindowSize().x,
                                          m_tooltip_anchor_min.y + ImGui::GetWindowSize().y);

    // Collect a finished pass and start one for the current slider values before the image is drawn, so
    // the newest available result is what lands on screen this frame.
    update_image_adjust_preview();

    // The photo itself, at the image's own aspect so a non-square texture is not stretched, and fitted
    // into a square box rather than given the full width whatever its shape: a portrait picture given
    // the width outright is half as tall again as the window can afford, and everything under it goes
    // over the edge. The box is also what the filter tiles and the sliders below are laid out against,
    // so the window's width does not change with the shape of the picture in it.
    const float aspect = m_adjust_source.height > 0 ? float(m_adjust_source.width) / float(m_adjust_source.height) : 1.f;
    const float draw_w = aspect >= 1.f ? img_w : std::round(img_w * std::max(0.05f, aspect));
    const float draw_h = aspect >= 1.f ? std::round(img_w / std::max(0.05f, aspect)) : img_w;
    // Centred in the box, so a narrow picture does not sit against the left edge with a gap beside it.
    if (draw_w < img_w)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::round(0.5f * (img_w - draw_w)));
    if (m_adjust_preview_valid && m_adjust_preview_tex)
        ImGui::Image((ImTextureID) (intptr_t) m_adjust_preview_tex->get_id(), ImVec2(draw_w, draw_h));
    else
        ImGui::Dummy(ImVec2(draw_w, draw_h));

    // Grey has no hue to turn and nothing to saturate, and the shipped texture library is grayscale, so
    // on most relief textures the colour controls would visibly do nothing.
    const bool has_color = m_adjust_source.has_color();

    // The filters, as pictures of themselves. Only for a colour picture: a filter is a look, and a
    // height map has none.
    if (has_color) {
        rebuild_filter_thumbnails();
        const std::vector<TextureFilterPreset> presets = texture_filter_presets();
        if (m_filter_thumbs.size() == presets.size()) {
            ImGui::Dummy(ImVec2(1.f, gap));
            ImGui::TextDisabled("%s", _u8L("Filters").c_str());

            constexpr int per_row = 5;
            const float   cell    = std::floor((img_w - float(per_row - 1) * gap) / float(per_row));
            ImDrawList   *wdl     = ImGui::GetWindowDrawList();
            for (size_t i = 0; i < presets.size(); ++i) {
                if (i % per_row != 0)
                    ImGui::SameLine(0.f, gap);
                ImGui::PushID(int(i));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                // The brightness mode survives picking a filter, and picking a filter does not read as
                // a change to it. It is a statement about how this user wants brightening done, not part
                // of what "More bite" means, so a filter sets the sliders and leaves it where it was.
                if (ImGui::InvisibleButton("##filter", ImVec2(cell, cell))) {
                    const TextureBrightnessSpace space = m_adjust.brightness_space;
                    m_adjust                           = presets[i].adjust;
                    m_adjust.brightness_space          = space;
                }
                const bool hot = ImGui::IsItemHovered();
                if (hot)
                    m_imgui->tooltip(into_u8(presets[i].name) + "\n\n" + presets[i].when,
                                     ImGui::GetFontSize() * 18.f);
                const ImVec2 br(at.x + cell, at.y + cell);
                if (m_filter_thumbs[i])
                    wdl->AddImage((ImTextureID) (intptr_t) m_filter_thumbs[i]->get_id(), at, br);
                // The one the sliders are currently sitting on, which after a click is the one just
                // chosen and after a drag is usually none of them.
                // Compared with the mode taken out of the question, for the same reason: ticking the
                // brightness box must not make the filter the user is plainly looking at stop reading as
                // chosen.
                TextureAdjustments probe = presets[i].adjust;
                probe.brightness_space   = m_adjust.brightness_space;
                const bool chosen        = m_adjust == probe;
                wdl->AddRect(at, br, ImGui::GetColorU32(chosen ? ImGuiWrapper::COL_ORCA :
                                                                 ImGui::GetStyleColorVec4(ImGuiCol_Separator)),
                             0.f, 0, chosen ? 2.f : 1.f);
                if (hot && !chosen)
                    wdl->AddRect(at, br, ImGui::GetColorU32(ImGuiCol_Text), 0.f, 0, 1.f);
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(1.f, gap));
        }
    }

    // The label column is sized off the widest of the labels, so the tracks line up whatever the
    // translation does to their lengths.
    float label_w = 0.f;
    for (const wxString &l : { _L("Brightness"), _L("Contrast"), _L("Sharpness"), _L("Blur"), _L("Hue"),
                               _L("Saturation") })
        label_w = std::max(label_w, ImGui::CalcTextSize(into_u8(l).c_str()).x);
    label_w += m_imgui->scaled(1.2f);
    // An explicit track width rather than a fill-to-the-right one: this window is AlwaysAutoResize, so its
    // width comes from its content, and an item asking for "whatever is left" would make the width depend
    // on itself. The image above sets the content width, and the sliders match it.
    const float slider_w = std::max(m_imgui->scaled(4.f), img_w - label_w);

    // The panel's own row (typed_slider()), with this window's label column rather than the panel's: the
    // labels here are measured above, and the track is sized to the image. Everything else - taking a
    // typed value, the range in the tooltip, a tooltip on a greyed row - is the panel's behaviour, so an
    // adjustment slider works exactly like a setting slider.
    const auto row = [&](const char *id, const wxString &label, float *v, float v_min, float v_max,
                         const char *format, bool enabled, const std::string &tip) {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + label_w));
        m_imgui->disabled_begin(!enabled);
        typed_slider(id, std::max(m_imgui->scaled(2.f), slider_w - panel_edit_icon_room()), v, v_min, v_max, format,
                     false, from_u8(tip));
        m_imgui->disabled_end();
    };

    row("##adj_brightness", _L("Brightness"), &m_adjust.brightness, -1.f, 1.f, "%.2f", true, {});
    // How that slider works, directly under it rather than in a settings list somewhere else, because it
    // is not a setting about the panel - it is the second half of what the slider above does. Per channel,
    // a lift drags red, green and blue toward white together, so a saturated colour loses its colour as it
    // gets lighter. In Lab it raises the lightness alone and holds the hue exactly. That matters more here
    // than it would in a photo editor: a stack of a few filaments reaches a limited set of colours, and a
    // brighten that washes them out is spending the few it has.
    {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x0 + label_w);
        bool perceptual = m_adjust.brightness_space == TextureBrightnessSpace::PerceptualLab;
        // Offered on a grayscale image too, unlike Hue and Saturation: there it changes the shape of the
        // tone curve rather than doing nothing, since a neutral's lightness is not its channel value.
        if (m_imgui->bbl_checkbox(_L("Keep the colors while brightening"), perceptual))
            m_adjust.brightness_space = perceptual ? TextureBrightnessSpace::PerceptualLab :
                                                     TextureBrightnessSpace::Rgb;
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Brightens by lightness alone, so a red stays red instead of drifting "
                                  "toward pink as it lightens. Off, the brighten is the plain per-channel "
                                  "one, which is what every version before this did.\n\n"
                                  "A colour that cannot be printed at that lightness loses some of its "
                                  "strength rather than its hue, since hue is what the eye reads a picture "
                                  "by. Costs a little more time on a large image."),
                              ImGui::GetFontSize() * 20.f);
    }
    row("##adj_contrast", _L("Contrast"), &m_adjust.contrast, -1.f, 1.f, "%.2f", true, {});
    row("##adj_sharpness", _L("Sharpness"), &m_adjust.sharpness, 0.f, 1.f, "%.2f", true,
        _u8L("Brings out the finest detail in the image. Applied after Blur, so the two can be used "
             "together: blur away the speckle, then sharpen the edges that remain."));
    row("##adj_blur", _L("Blur"), &m_adjust.blur, 0.f, 1.f, "%.2f", true,
        _u8L("Blurs the image itself. The layer's own Smoothing slider softens the relief without altering "
             "the image, and is the one to reach for first."));
    // Disabled with the reason given, rather than hidden: see has_color above.
    row("##adj_hue", _L("Hue"), &m_adjust.hue_deg, -180.f, 180.f, "%.0f°", has_color, {});
    row("##adj_saturation", _L("Saturation"), &m_adjust.saturation, -1.f, 1.f, "%.2f", has_color,
        _u8L("How strong the colours are. It does more here than it would in a photo editor: a stack of a "
             "few filaments reaches a limited set of colours, and pulling the picture towards what it can "
             "reach is often the difference between a print that looks like the picture and one that does "
             "not. All the way down is a picture in tone alone, for a print in two filaments."));
    if (!has_color) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + img_w);
        ImGui::TextDisabled("%s", _u8L("This texture is grayscale, so it has no hue to shift and no color to "
                                       "strengthen.")
                                      .c_str());
        ImGui::PopTextWrapPos();
    }

    // Resets the sliders and keeps the brightness mode, which is the same rule the filters follow: this
    // button undoes an adjustment, and how brightening is done was never part of one.
    if (m_imgui->button(_L("Reset"))) {
        const TextureBrightnessSpace space = m_adjust.brightness_space;
        m_adjust                           = TextureAdjustments{};
        m_adjust.brightness_space          = space;
    }

    sync_image_adjust(*layer);

    ImGui::Separator();
    m_imgui->text_wrapped(_u8L("The model updates when you release a slider. Save as copy writes the "
                               "adjusted image to your texture library and points this layer at it."), img_w);

    const bool cancel = m_imgui->button(_L("Cancel"));
    ImGui::SameLine();
    // Nothing to save until a slider has actually moved.
    m_imgui->disabled_begin(m_adjust.identity());
    const bool as_copy = m_imgui->button(_L("Save as copy"));
    m_imgui->disabled_end();

    m_tooltip_anchor_min = prev_anchor_min;
    m_tooltip_anchor_max = prev_anchor_max;
    ImGui::End();

    if (cancel || !keep_open)
        close_image_adjust();
    else if (as_copy)
        commit_image_adjust(*layer);
}

} // namespace Slic3r::GUI
