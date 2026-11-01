#include "PlateArrangementDialog.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#include <utility>

#include <wx/colour.h> // the WXK_* key codes, as misc-include-cleaner maps them
#include <wx/event.h>
#include <wx/string.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>
#include <imgui/imgui_internal.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Utils.hpp"
#include <libslic3r/Point.hpp>
#include "libslic3r/format.hpp"
#include <slic3r/GUI/IconManager.hpp>

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "Gizmos/GLGizmoUtils.hpp"
#include "I18N.hpp"
#include "IMToolbar.hpp"
#include "ImGuiWrapper.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"

namespace Slic3r {
namespace GUI {

PlateArrangementDialog::PlateArrangementDialog(GLCanvas3D& parent) : m_parent(parent), m_imgui(wxGetApp().imgui()) {}

PlateArrangementDialog::~PlateArrangementDialog() = default;

void PlateArrangementDialog::render_input_window()
{
    if (!m_open) {
        m_thumbnails.clear(); // deferred from on_set_state(), the previews may have been in the last frame
        m_double_click = false;
        return;
    }
    // Fetched once per frame for every function of the dialog.
    m_plate_list  = &wxGetApp().plater()->get_partplate_list();
    m_plate_count = m_plate_list->get_plate_count();
    create_gui_cfg();
    update_plates();
    update_thumbnails();
    update_icons();

    on_render_input_window();

    // Keep frames coming while the mouse animates something, and while the text cursor blinks.
    if (m_center_frames > 0 || m_drag.pressed || ImGui::IsAnyItemActive()) {
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
    m_double_click = false;
}

bool PlateArrangementDialog::on_key(const wxKeyEvent& evt)
{
    const int key = evt.GetKeyCode();
    if (evt.GetEventType() == wxEVT_KEY_UP)
        return m_claimed_keys.erase(key) > 0;
    if (evt.GetEventType() != wxEVT_KEY_DOWN)
        return m_claimed_keys.count(key) > 0;

    static const std::set<int> keys = { WXK_LEFT, WXK_RIGHT, WXK_UP, WXK_DOWN, WXK_HOME, WXK_END, WXK_ESCAPE };
    // The name field keeps its keys, and the canvas keeps them while the dialog is not the focused overlay.
    if (!m_open || !m_focused || !keys.count(key) || m_imgui->text_input_active())
        return false;
    m_claimed_keys.insert(key);
    m_imgui->set_requires_extra_frame();

    if (key == WXK_ESCAPE) {
        if (m_drag.active)
            m_drag = Drag();
        else
            set_open(false);
        return true;
    }
    // Between frames: a plate may have been added or removed since the last one.
    m_plate_count = m_plate_list->get_plate_count();
    if (m_drag.pressed || m_cursor == nullptr || m_plate_count < 1)
        return true;

    if (evt.CmdDown()) { // Ctrl (Cmd on macOS) moves the selected plates
        const Move move = key == WXK_HOME ? Move::First : key == WXK_END ? Move::Last : key == WXK_LEFT ? Move::Previous : Move::Next;
        if (key != WXK_UP && key != WXK_DOWN)
            apply_order(order_after_move(move));
        return true;
    }

    const int cols = grid_columns(m_plate_count), cur = m_cursor->get_index(), last = m_plate_count - 1;
    int       target = cur;
    switch (key) {
    case WXK_LEFT:  target = cur - 1; break;
    case WXK_RIGHT: target = cur + 1; break;
    case WXK_UP:    target = cur - cols; break;
    // Down from above the incomplete last row lands on the last plate.
    case WXK_DOWN:  target = cur + cols <= last ? cur + cols : (cur / cols < last / cols ? last : cur); break;
    case WXK_HOME:  target = 0; break;
    case WXK_END:   target = last; break;
    }
    if (target != cur && target >= 0 && target <= last)
        select(target, evt.ShiftDown(), false);
    return true;
}

void PlateArrangementDialog::on_change_color_mode(bool is_dark) { m_is_dark_mode = is_dark; }

int PlateArrangementDialog::grid_columns(int plate_count)
{
    return plate_count > 25 ? 6 : plate_count > 16 ? 5 : plate_count > 9 ? 4 : plate_count > 4 ? 3 : 2;
}

void PlateArrangementDialog::on_set_state()
{
    m_drag       = Drag();
    m_name_plate = nullptr;
    m_selection.clear();
    if (!m_open) {
        // The previews may still be in this frame's draw lists: they are freed by the next render_input_window().
        m_focused = false;
        m_anchor = m_cursor = nullptr;
        return;
    }
    m_center_frames = m_positioned ? 0 : 3; // centred the first time, then where the user left it
    m_focus_request = true;
    // Opened before its first frame, which is where the plate list is fetched otherwise.
    m_plate_list   = &wxGetApp().plater()->get_partplate_list();
    m_plate_count  = m_plate_list->get_plate_count();
    m_canvas_plate = m_anchor = m_cursor = m_plate_list->get_curr_plate();
    if (m_cursor != nullptr)
        m_selection.insert(m_cursor);
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void PlateArrangementDialog::on_render_input_window()
{
    const GuiCfg& cfg = *m_gui_cfg;

    // Whole-pixel position, so the icons are not resampled. Centred once the auto-sized window knows its size.
    const Size   cnv = m_parent.get_canvas_size();
    const ImVec2 room(std::max(0.f, cnv.get_width() - m_window_size.x), std::max(0.f, cnv.get_height() - m_window_size.y));
    if (std::exchange(m_focus_request, false))
        ImGui::SetNextWindowFocus(); // arrow keys work right away
    if (m_center_frames > 0) {
        m_window_pos = room * .5f;
        m_positioned = --m_center_frames == 0;
    }
    m_window_pos = ImVec2(std::round(std::clamp(m_window_pos.x, 0.f, room.x)), std::round(std::clamp(m_window_pos.y, 0.f, room.y)));
    m_imgui->set_next_window_pos(m_window_pos.x, m_window_pos.y, ImGuiCond_Always);

    // The gizmos' window style, with this window's sizes and the Regular buttons of Widgets/Button.
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding  , ImVec2(cfg.window_padding, cfg.window_padding));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing    , ImVec2(cfg.item_spacing, cfg.item_spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding , cfg.window_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , cfg.frame_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Button            , ImGuiWrapper::to_ImVec4(decode_color_to_float_array(m_is_dark_mode ? "#3E3E45" : "#DFDFDF")));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered     , ImGuiWrapper::to_ImVec4(decode_color_to_float_array(m_is_dark_mode ? "#4D4D54" : "#D4D4D4")));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive      , ImGuiWrapper::to_ImVec4(decode_color_to_float_array(m_is_dark_mode ? "#3E3E45" : "#DFDFDF")));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled      , m_is_dark_mode ? ImVec4(1.f,1.f,1.f,.4f) : ImVec4(0.f,0.f,0.f,.4f));
    ImGui::PushStyleColor(ImGuiCol_Border            , ImGuiWrapper::to_ImVec4(decode_color_to_float_array(m_is_dark_mode ? "#4E4E54" : "#D4D4D8")));

    m_imgui->begin(std::string("##PlateArrangementDialog"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                                                            ImGuiWindowFlags_AlwaysAutoResize);
    m_window_size = ImGui::GetWindowSize();

    render_title_bar();
    render_plate_grid();
    render_bottom_bar();
    m_focused = m_open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    m_imgui->end();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(5);
    ImGuiWrapper::pop_toolbar_style();

    if (m_drag.active)
        render_drag_preview();
}

void PlateArrangementDialog::render_title_bar()
{
    const GuiCfg& cfg   = *m_gui_cfg;
    const ImVec2  p     = ImGui::GetCursorScreenPos();
    const ImVec2  start = ImGui::GetCursorPos();

    ImGui::InvisibleButton("##title_bar", ImVec2(cfg.grid_size - cfg.close_width - cfg.item_spacing, cfg.title_height));
    if (ImGui::IsItemActive())
        m_window_pos += ImGui::GetIO().MouseDelta;
    ImGui::SetCursorPos(ImVec2(start.x, start.y + std::round((cfg.title_height - ImGui::GetTextLineHeight()) * .5f)));
    m_imgui->bold_text(_u8L("Arrange Plates"));

    ImGui::SetCursorPos(ImVec2(start.x + cfg.grid_size - cfg.close_width, start.y));
    if (m_imgui->button(_L("Close"), ImVec2(cfg.close_width, cfg.title_height), true))
        set_open(false);

    const float y  = std::round(p.y + cfg.title_height + cfg.v_spacing * .5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, y),
        ImVec2(p.x + cfg.grid_size, y),
        m_focused ? ImGui::GetColorU32(ImGuiWrapper::COL_ORCA) : ImGui::GetColorU32(ImGuiCol_Separator),
        m_focused ? 2.f : 1.f
    );
    ImGui::SetCursorPos(ImVec2(start.x, start.y + cfg.title_height + cfg.v_spacing));
}

void PlateArrangementDialog::render_plate_grid()
{
    const GuiCfg& cfg   = *m_gui_cfg;
    const int     cols  = grid_columns(m_plate_count);
    const int     rows  = std::max(1, (m_plate_count + cols - 1) / cols);
    const float   cell  = cell_size(cols);
    const float   pitch = cell + cfg.plate_spacing;
    const float   bleed = std::round(cfg.plate_spacing * .5f); // room for borders and markers outside the outer cells
    ImGuiIO&      io    = ImGui::GetIO();

    // More plates than cells (above the 36 plates Orca allows) would scroll.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - bleed);
    ImGui::BeginChild("##plate_grid", ImVec2(cfg.grid_size + 2.f * bleed, cfg.grid_size + 2.f * bleed), false,
                      rows > cols ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList*  dl       = ImGui::GetWindowDrawList();
    const ImVec2 origin   = ImGui::GetCursorScreenPos() + ImVec2(bleed, bleed); // follows the scroll position
    auto         cell_min = [&](int i) { return origin + ImVec2(std::round(float(i % cols) * pitch), std::round(float(i / cols) * pitch)); };

    if (m_plate_count == 1) { // the single plate, with the message across the free row below it
        const std::string msg  = _u8L("There is only one plate. Add more plates to arrange them.");
        const float       wrap = cfg.grid_size - 2.f * cfg.plate_spacing;
        const ImVec2      tsz  = ImGui::CalcTextSize(msg.c_str(), nullptr, false, wrap);
        const ImVec2      c    = cell_min(cols) + ImVec2(cfg.grid_size, cell) * .5f;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), c - tsz * .5f, ImGui::GetColorU32(ImGuiCol_TextDisabled), msg.c_str(), nullptr, wrap);
    }

    for (int i = 0; i < m_plate_count; ++i) {
        PartPlate* plate = m_plate_list->get_plate(i);
        ImGui::SetCursorScreenPos(cell_min(i));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##plate", ImVec2(cell, cell));
        ImGui::PopID();
        if (ImGui::IsItemActivated())
            m_drag = Drag{ true, false, i, io.MousePos, io.MousePos - cell_min(i) };
        if (m_double_click && ImGui::IsItemHovered()) { // double click zooms to the plate
            select(i, false, false);
            zoom_to_selection();
        }

        const bool hovered = !m_drag.active && ImGui::IsItemHovered();
        const bool lifted  = m_drag.active && is_selected(plate);
        render_plate(dl, plate, cell_min(i), cell, is_selected(plate), hovered, lifted ? cfg.dragged_alpha : 1.f);
        if (hovered) {
            const std::string nm = plate->get_plate_name();
            // TRN %1% is the plate number, %2% the name the user gave the plate.
            m_imgui->tooltip(nm.empty() ? format(_u8L("Plate %1%"), i + 1) : format(_u8L("Plate %1%: %2%"), i + 1, nm), ImGui::GetFontSize() * 20.f);
        }
    }
    ImGui::SetCursorScreenPos(origin + ImVec2(0.f, float(rows) * pitch - cfg.plate_spacing));
    ImGui::Dummy(ImVec2(1.f, bleed)); // content height, for scrolling

    // A press turns into a drag past the threshold; it moves the selection, or the pressed plate alone.
    if (m_drag.pressed && !m_drag.active && io.MouseDown[0]) {
        const ImVec2 d = io.MousePos - m_drag.press_pos;
        if (d.x * d.x + d.y * d.y > cfg.drag_threshold * cfg.drag_threshold) {
            if (!is_selected(m_plate_list->get_plate(m_drag.press_index)))
                select(m_drag.press_index, false, false);
            m_drag.active = true;
        }
    }

    if (m_drag.active) {
        const ImVec2 local = io.MousePos - origin;
        const int    row   = std::clamp(int(std::floor(local.y / pitch)), 0, rows - 1);
        const int    col   = std::clamp(int(std::floor(local.x / pitch)), 0, cols - 1);
        int          idx   = row * cols + col;
        bool         after = local.x - float(col) * pitch > cell * .5f;
        if (idx >= m_plate_count) { // past the last plate of the last row
            idx   = m_plate_count - 1;
            after = true;
        }
        const int insert    = idx + (after ? 1 : 0);
        m_drag.insert_index = is_identity(order_after_drop(insert)) ? -1 : insert;

        if (m_drag.insert_index >= 0) { // drop marker, in the gap next to the target cell
            const ImVec2 cmin = cell_min(idx);
            const float  x    = after ? cmin.x + cell + bleed : cmin.x - bleed;
            const float  t    = cfg.marker;
            dl->AddLine(ImVec2(x, cmin.y + t), ImVec2(x, cmin.y + cell - t), ImGui::ColorConvertFloat4ToU32(ImGuiWrapper::COL_ORCA), t);
        }
    }

    if (m_drag.pressed && ImGui::IsMouseReleased(0)) {
        if (!m_drag.active)
            select(m_drag.press_index, io.KeyShift, io.KeyCtrl || io.KeySuper);
        else if (m_drag.insert_index >= 0)
            apply_order(order_after_drop(m_drag.insert_index));
        m_drag = Drag();
    }
    ImGui::EndChild();
}

void PlateArrangementDialog::render_plate(ImDrawList* dl, PartPlate* plate, const ImVec2& pmin, float size, bool selected, bool hovered, float alpha) const
{
    const GuiCfg& cfg   = *m_gui_cfg;
    const ImVec2  pmax  = pmin + ImVec2(size, size);
    const float   inset = cfg.plate_inset;

    auto w_alpha = [](ImVec4 c, float a) { c.w *= a; return c; }; // reduce on drag preview
    auto u32     = [](ImVec4 c) { return  ImGui::ColorConvertFloat4ToU32(c); };

    ImVec4 plate_tint     = ImVec4(0.f, 0.f, 0.f, m_is_dark_mode ? .1f : .05f);
    ImVec4 plate_sel_tint = m_focused ? w_alpha(ImGuiWrapper::COL_ORCA, .25f) : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    dl->AddRectFilled(pmin, pmax, u32(w_alpha(selected ? plate_sel_tint : plate_tint, alpha)), cfg.plate_rounding);
    auto it = m_thumbnails.find(plate);
    if (it != m_thumbnails.end() && it->second.texture && it->second.texture->texture_id)
        dl->AddImageRounded(it->second.texture->texture_id, pmin, pmax, ImVec2(0.f, 1.f), ImVec2(1.f, 0.f), IM_COL32(255, 255, 255, 255), cfg.plate_rounding);

    // Square number badge
    const std::string num   = std::to_string(plate->get_index() + 1);
    ImFont*           font  = ImGui::GetFont();
    const float       fsize = ImGui::GetFontSize() * std::clamp(size / (2.f * cell_size(cfg.max_columns)), .8f, 1.f); // use smaller font on denser grids
    const ImVec2      ref   = font->CalcTextSizeA(fsize, FLT_MAX, 0.f, m_plate_count > 9 ? "99" : "9");
    const float       side  = std::round(std::max(ref.x + fsize * .4f, ref.y));
    const ImVec2      bmin  = pmin + ImVec2(inset, inset);
    const ImVec2      tsz   = font->CalcTextSizeA(fsize, FLT_MAX, 0.f, num.c_str());
    ImVec4            no_bg = m_focused ? ImGuiWrapper::COL_ORCA : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    dl->AddRectFilled(bmin, bmin + ImVec2(side, side), u32(w_alpha(selected ? no_bg : plate_tint, alpha)), std::max(0.f, cfg.plate_rounding - inset));
    dl->AddText(font, fsize, bmin + (ImVec2(side, side) - tsz) * .5f, u32(w_alpha(selected ? ImVec4(1.f, 1.f, 1.f, 1.f) : ImGui::GetStyleColorVec4(ImGuiCol_Text), alpha)), num.c_str());

    // State badges
    float right = pmax.x - inset;
    for (const auto& [shown, bg] : {
        std::tuple{ plate->is_locked()          , ImGuiWrapper::COL_ORCA   },
        std::tuple{ plate->has_custom_settings(), ImGuiWrapper::COL_WARNING}
    }) {
        if (!shown) continue;
        const float r = side * .3f;
        dl->AddCircleFilled(ImVec2(right - inset - r * .5f, pmin.y + inset + r), r, u32(w_alpha(bg, alpha)), 24);
        right -= r * 2.5f;
    }

    if (selected || hovered) {
        const float t = cfg.border;
        ImVec4 border_clr = m_focused ? (selected && hovered ? ImGuiWrapper::COL_ORCA_HOVER : ImGuiWrapper::COL_ORCA) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        dl->AddRect(pmin - ImVec2(t, t) * .5f, pmax + ImVec2(t, t) * .5f, u32(w_alpha(border_clr, alpha)), cfg.plate_rounding + t * .5f, 0, t);
    } else
        dl->AddRect(pmin, pmax, u32(w_alpha(ImGui::GetStyleColorVec4(ImGuiCol_Border), alpha)), cfg.plate_rounding, 0, 1.f);
}

void PlateArrangementDialog::render_drag_preview() const
{
    const GuiCfg& cfg  = *m_gui_cfg;
    PartPlate*    lead = nullptr; // first dragged plate in plate order
    for (PartPlate* p : m_selection)
        if (lead == nullptr || p->get_index() < lead->get_index())
            lead = p;
    if (lead == nullptr)
        return;

    const float  size = cell_size(grid_columns(m_plate_count)) * cfg.ghost_scale;
    const ImVec2 pmin = ImGui::GetIO().MousePos - m_drag.grab_offset * cfg.ghost_scale;
    ImDrawList*  dl   = ImGui::GetForegroundDrawList();
    render_plate(dl, lead, pmin, size, true, false, cfg.ghost_alpha);

    if (m_selection.size() > 1) { // count badge for group drags
        const std::string n   = std::to_string(m_selection.size());
        const ImVec2      tsz = ImGui::CalcTextSize(n.c_str());
        const float       r   = std::max(tsz.x, tsz.y) * .5f + cfg.plate_inset;
        const ImVec2      c   = pmin + ImVec2(size, size) - ImVec2(r, r) * .5f;
        dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(ImGuiWrapper::COL_ORCA), 24);
        dl->AddText(c - tsz * .5f, IM_COL32_WHITE, n.c_str());
    }
}

void PlateArrangementDialog::render_bottom_bar()
{
    const GuiCfg& cfg    = *m_gui_cfg;
    Plater*       plater = wxGetApp().plater();
    const float   btn    = cfg.button_size;
    const float   gap    = cfg.item_spacing;
    const float   group  = cfg.group_spacing;
    const float   x0     = ImGui::GetCursorPosX();
    PartPlate*    single = m_selection.size() == 1 ? *m_selection.begin() : nullptr;

    auto w_alpha = [](ImVec4 c, float alpha) { c.w *= alpha; return c; }; // reduce on drag preview

    // Every item of a row starts at the row's top and is as tall as a button, the name field included.
    const float row1 = std::round(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + cfg.v_spacing);
    const float row2 = row1 + btn + cfg.v_spacing;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(cfg.item_spacing, (btn - ImGui::GetFontSize()) * .5f));

    // Top row for plate
    const float  actions = x0 + cfg.grid_size - 4.f * btn - 3.f * gap;
    const float  badge   = btn - 2.f * cfg.plate_inset;
    ImGui::SetCursorPos(ImVec2(x0, row1));
    const ImVec2 f0      = ImGui::GetCursorScreenPos();
    const ImVec2 f1      = f0 + ImVec2(actions - group - x0, btn);
    const bool   editing = ImGui::GetActiveID() == ImGui::GetID("##plate_name");
    const ImU32  frame   = editing ? ImGui::ColorConvertFloat4ToU32(ImGuiWrapper::COL_ORCA) : ImGui::GetColorU32(ImGuiCol_Border);
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    // While editing: Orca accent at 15% and an accent border.
    dl->AddRectFilled(f0, f1, editing ? ImGui::ColorConvertFloat4ToU32(w_alpha(ImGuiWrapper::COL_ORCA, .15f)) : ImGui::GetColorU32(ImGuiCol_WindowBg), cfg.frame_rounding);
    dl->AddRect(f0, f1, frame, cfg.frame_rounding);
    if (single != nullptr) {
        if (single != m_name_plate && !editing) {
            m_name_buf.fill('\0');
            std::strncpy(m_name_buf.data(), single->get_plate_name().c_str(), m_name_buf.size() - 1);
            m_name_plate = single;
        }
        const ImVec2      b0  = f0 + ImVec2(cfg.plate_inset, cfg.plate_inset);
        const std::string num = std::to_string(single->get_index() + 1);
        dl->AddText(b0 + (ImVec2(badge, badge) - ImGui::CalcTextSize(num.c_str())) * .5f, ImGui::GetColorU32(ImGuiCol_Text), num.c_str());
        dl->AddLine(ImVec2(b0.x + badge + .5f, b0.y), ImVec2(b0.x + badge + .5f, b0.y + badge), ImGui::GetColorU32(ImGuiCol_Border), 1.f);

        // The text field itself starts after the number and has no frame of its own.
        ImGui::SetCursorScreenPos(ImVec2(b0.x + badge + gap, f0.y));
        ImGui::SetNextItemWidth(f1.x - b0.x - badge - gap);
        ImGui::PushStyleColor(ImGuiCol_FrameBg       , ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive , ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::InputTextWithHint("##plate_name", _u8L("Plate name").c_str(), m_name_buf.data(), m_name_buf.size());
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemDeactivatedAfterEdit() && m_name_plate != nullptr)
            plater->CallAfter([this, target = m_name_plate, name = std::string(m_name_buf.data())]() {
                if (index_of(target) >= 0) { // the plate may be gone by now
                    target->set_plate_name(name);
                    wxGetApp().plater()->get_view3D_canvas3D()->set_as_dirty(); // redraw the name on the bed
                }
            });
    } else {
        const int         n    = int(m_selection.size());
        const std::string text = format(std::string(_L_PLURAL("%1% plate selected", "%1% plates selected", n).utf8_str()), n);
        dl->AddText(f0 + ImVec2(gap * 2.f, (btn - ImGui::GetTextLineHeight()) * .5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str());
    }

    auto plate_action = [this, plater, single](int action) {
        plater->CallAfter([this, plate = single, action]() {
            if (const int i = index_of(plate); i >= 0)
                wxGetApp().plater()->select_plate_by_hover_id(i * int(PartPlate::GRABBER_COUNT) + action);
        });
    };
    const bool all_locked = std::all_of(m_selection.begin(), m_selection.end(), [](PartPlate* plate) { return plate->is_locked(); });
    const bool unlocked   = single != nullptr && !single->is_locked(); // a locked plate keeps its objects where they are
    ImGui::SetCursorPos(ImVec2(actions, row1));
    if (render_icon_button(IconId::Orient, unlocked, _u8L("Auto orient objects on current plate")))
        plate_action(ActionOrient);
    ImGui::SameLine();
    if (render_icon_button(IconId::Arrange, unlocked, _u8L("Arrange objects on current plate")))
        plate_action(ActionArrange);
    ImGui::SameLine();
    const std::string lock_tip = single != nullptr ? (all_locked ? _u8L("Unlock current plate")   : _u8L("Lock current plate")) :
                                                     (all_locked ? _u8L("Unlock selected plates") : _u8L("Lock selected plates"));
    if (render_icon_button(all_locked ? IconId::Locked : IconId::Unlocked, !m_selection.empty(), lock_tip, all_locked ? &ImGuiWrapper::COL_ORCA : nullptr)) {
        // Through the plate list rather than the plate's own action, which toggles one plate per undo step.
        plater->CallAfter([this, plates = std::vector<PartPlate*>(m_selection.begin(), m_selection.end()), lock = !all_locked]() {
            Plater*              pl = wxGetApp().plater();
            Plater::TakeSnapshot snapshot(pl, "lock partplate"); // one undo step for all of them
            for (PartPlate* plate : plates)
                if (const int i = index_of(plate); i >= 0) // the plate may be gone by now
                    pl->get_partplate_list().lock_plate(i, lock);
            pl->get_view3D_canvas3D()->set_as_dirty();
        });
    }
    ImGui::SameLine();
    if (render_icon_button(IconId::Settings, single != nullptr, _u8L("Customize current plate"),
                           single != nullptr && single->has_custom_settings() ? &ImGuiWrapper::COL_WARNING : nullptr))
        plate_action(ActionSettings);

    // Bottom row for actions
    ImGui::SetCursorPos(ImVec2(x0, row2 + std::round((btn - m_info_button_size) * .5f)));
    const wxString ctrl  = from_u8(shortkey_ctrl_prefix());
    const wxString shift = from_u8(shortkey_shift_prefix());
    // TRN Keyboard and mouse shortcuts
    const std::vector<std::pair<wxString, wxString>> shortcuts = {
        { _L("Left mouse button"), _L("Select plate") },
        { ctrl + _L("Left mouse button"), _L("Add or remove from selection") },
        { shift + _L("Left mouse button"), _L("Select range") },
        { _L("Double click"), _L("Zoom to plate") },
        { _L("Drag"), _L("Move selected plates") },
        { _L("Arrow keys"), _L("Select neighbouring plate") },
        { shift + _L("Arrow keys"), _L("Extend selection") },
        { _L("Home / End"), _L("Select first / last plate") },
        { ctrl + _L("Left / Right arrow"), _L("Move to previous / next") },
        { ctrl + _L("Home / End"), _L("Move to beginning / end") },
        { _L("Esc"), _L("Cancel dragging or close") },
    };
    // The gizmos' shortcuts button; its tooltip opens below the dialog.
    GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, shortcuts, m_window_pos.x, m_window_pos.y);
    m_info_button_size = ImGui::GetItemRectSize().y;

    struct MoveButton { IconId icon; Move move; std::string tooltip; };
    const MoveButton moves[] = {
        { IconId::First,    Move::First,    _u8L("Move to beginning") },
        { IconId::Previous, Move::Previous, _u8L("Move to previous") },
        { IconId::Next,     Move::Next,     _u8L("Move to next") },
        { IconId::Last,     Move::Last,     _u8L("Move to end") },
    };
    ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetItemRectSize().x + group, row2));
    for (const MoveButton& b : moves) {
        const std::vector<int> order = order_after_move(b.move);
        if (render_icon_button(b.icon, !is_identity(order), b.tooltip))
            apply_order(order);
        ImGui::SameLine();
    }

    ImGui::SameLine(0.f, group);
    if (render_icon_button(IconId::ZoomIn, true, _u8L("Zoom to selected plates")))
        zoom_to_selection();
    ImGui::SameLine();
    if (render_icon_button(IconId::ZoomOut, true, _u8L("Zoom out to all plates")))
        m_parent.zoom_to_plate(REQUIRES_ZOOM_TO_ALL_PLATE);

    ImGui::SetCursorPos(ImVec2(x0 + cfg.grid_size - cfg.choose_width * 2.f - cfg.item_spacing, row2));
    if (m_imgui->button(_L("Add"), ImVec2(cfg.choose_width, cfg.button_size), plater->can_add_plate()))
        plater->CallAfter([]() { wxGetApp().plater()->add_plate(); }); // the new plate becomes current, and selected
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        m_imgui->tooltip(_u8L("Add plate"), ImGui::GetFontSize() * 20.f);

    ImGui::SameLine(0.f, cfg.item_spacing); // apart from Add, as it removes plates
    if (m_imgui->button(_L("Remove"), ImVec2(cfg.choose_width, cfg.button_size), (m_plate_count > 1 && int(m_selection.size()) < m_plate_count))){
        std::vector<int> indices;
        for (PartPlate* plate : m_selection)
            indices.push_back(plate->get_index());
        std::sort(indices.rbegin(), indices.rend()); // from the back, so the other indices stay valid
        plater->CallAfter([indices]() {
            Plater*              pl = wxGetApp().plater();
            Plater::TakeSnapshot snapshot(pl, "Delete plates"); // one undo step for all of them
            for (int index : indices)
                pl->delete_plate(index);
        });
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        m_imgui->tooltip(_u8L("Remove selected plates"), ImGui::GetFontSize() * 20.f);
    ImGui::PopStyleVar();
}

bool PlateArrangementDialog::render_icon_button(IconId icon, bool enabled, const std::string& tooltip, const ImVec4* state)
{
    const GuiCfg&            cfg  = *m_gui_cfg;
    const IconManager::Icon& i    = *m_icons[size_t(icon)].front();
    const float              size = cfg.button_size - 2.f * cfg.button_padding;
    auto shade = [](const ImVec4& c, bool dark, float a /*amount*/) { // lighter in dark mode, darker in light mode
        return dark ? ImVec4(c.x + (1.f - c.x) * a, c.y + (1.f - c.y) * a, c.z + (1.f - c.z) * a, c.w) :
                      ImVec4(c.x * (1.f - a)      , c.y * (1.f - a)      , c.z * (1.f - a)      , c.w);
    };
    if (state != nullptr) { // the state colour, shaded on hover; pressed stays as is, as in the Regular style
        ImGui::PushStyleColor(ImGuiCol_Button       , *state);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(*state, m_is_dark_mode, .1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive , *state);
    }
    m_imgui->disabled_begin(!enabled);
    ImGui::PushID(icon_names[size_t(icon)]); // the icons share one texture, which image_button() takes for the ID
    const bool clicked = m_imgui->image_button((ImTextureID)(intptr_t)i.tex_id, ImVec2(size, size), i.tl, i.br, int(cfg.button_padding)) && enabled;
    ImGui::PopID();
    m_imgui->disabled_end();
    if (state != nullptr)
        ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        m_imgui->tooltip(tooltip, ImGui::GetFontSize() * 20.f);
    return clicked;
}

void PlateArrangementDialog::create_gui_cfg()
{
    const float font_size = m_imgui->get_font_size();
    if (m_gui_cfg.has_value() && m_gui_cfg->font_size == font_size)
        return;
    // Design sizes in pixels at 100% display scaling, scaled as the gizmos scale theirs (ImGuiWrapper::scaled()).
    auto px = [this](float v) { return std::round(m_imgui->scaled(v / 15.f)); }; // em_unit() 10 x 1.5 (GLCanvas3D::set_imgui_scaling())

    GuiCfg cfg;
    cfg.font_size       = font_size;
    cfg.plate_spacing   = px(6.f);
    cfg.grid_size       = cfg.max_columns * px(64.f) + (cfg.max_columns - 1) * cfg.plate_spacing;
    cfg.plate_rounding  = px(5.f);
    cfg.plate_inset     = px(3.f);
    cfg.border          = px(2.f);
    cfg.marker          = px(2.f);
    cfg.window_padding  = px(12.f);
    cfg.window_rounding = px(10.f);
    cfg.frame_rounding  = px(3.f);
    cfg.title_height    = px(26.f);
    cfg.close_width     = std::round(std::max(px(60.f), m_imgui->calc_text_size(_L("Close")).x)  + px(20.f));
    cfg.choose_width    = std::round(std::max(px(60.f), std::max(m_imgui->calc_text_size(_L("Remove")).x, m_imgui->calc_text_size(_L("Add")).x)) + px(20.f));
    cfg.button_size     = px(26.f);
    cfg.button_padding  = cfg.frame_rounding; // image_button() rounds its corners by at most the padding
    cfg.item_spacing    = px(4.f);
    cfg.group_spacing   = px(10.f);
    cfg.v_spacing     = px(12.f);
    cfg.drag_threshold  = px(5.f);
    m_gui_cfg = cfg;
}

int PlateArrangementDialog::index_of(PartPlate* plate) // -1 once the plate is gone
{
    // Called from CallAfter(), after the plates may have changed: the live count, not the frame's.
    for (int i = 0; i < m_plate_list->get_plate_count(); ++i)
        if (m_plate_list->get_plate(i) == plate)
            return i;
    return -1;
}

float PlateArrangementDialog::cell_size(int cols) const
{
    return (m_gui_cfg->grid_size - (cols - 1) * m_gui_cfg->plate_spacing) / cols;
}

void PlateArrangementDialog::update_plates()
{
    // Forget plates deleted since the last frame (undo, the plate's delete icon, ...).
    std::set<PartPlate*> alive;
    for (int i = 0; i < m_plate_count; ++i)
        alive.insert(m_plate_list->get_plate(i));
    for (auto it = m_selection.begin(); it != m_selection.end();)
        it = alive.count(*it) ? std::next(it) : m_selection.erase(it);
    for (auto it = m_thumbnails.begin(); it != m_thumbnails.end();)
        it = alive.count(it->first) ? std::next(it) : m_thumbnails.erase(it);
    for (PartPlate** p : { &m_anchor, &m_cursor, &m_name_plate })
        if (!alive.count(*p))
            *p = nullptr;

    // Follow the plate picked in the 3D view, or made current by adding / deleting plates.
    PartPlate* curr = m_plate_list->get_curr_plate();
    if (*m_pending_switches == 0 && curr != m_canvas_plate) {
        m_canvas_plate = curr;
        m_selection.clear();
    }
    if (m_selection.empty() && curr != nullptr)
        m_selection.insert(m_anchor = m_cursor = curr);
    if (m_cursor == nullptr && !m_selection.empty())
        m_cursor = *m_selection.begin();
    // No drag can end on a grid shown for a single plate's sake.
    if (m_plate_count < 2)
        m_drag = Drag();
}

void PlateArrangementDialog::update_thumbnails()
{
    // Wait until a drag (of plates, objects or the camera) ends: re-rendering on every move would stall it.
    if (ImGui::GetIO().MouseDown[0])
        return;

    // Rendered at 2x the cell for crisp previews on HiDPI screens.
    const int px = std::clamp(int(std::ceil(cell_size(grid_columns(m_plate_count)) * 2.f)), 64, 512);

    if (wxGetApp().plater()->is_gcode_3mf()) { // no model to render from: use the previews stored in the 3mf
        for (int i = 0; i < m_plate_count; ++i)
            if (Thumbnail& t = m_thumbnails[m_plate_list->get_plate(i)]; !t.texture) {
                t.texture = std::make_unique<IMToolbarItem>();
                t.texture->set_thumbnail(m_plate_list->get_plate(i)->thumbnail_data);
            }
        return;
    }

    // Fingerprints, per plate, what the thumbnail renderer draws: which volumes, their geometry, placement and color
    // re-rendered when its fingerprint changes, whatever code path changed the scene.
    std::vector<std::size_t> fingerprints(size_t(m_plate_count), 0);
    std::map<std::pair<int, int>, int> plate_of; // (object, instance) -> plate index

    auto hash_combine = [](std::size_t& seed, const auto& v) {
        using T = std::decay_t<decltype(v)>;
        seed ^= std::hash<T>{}(v) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    };

    for (const GLVolume* v : m_parent.get_volumes().volumes) {
        if (v == nullptr || v->is_wipe_tower || v->object_idx() < 0)
            continue;
        const auto [it, added] = plate_of.try_emplace({ v->composite_id.object_id, v->composite_id.instance_id }, -1);
        for (int i = 0; added && i < m_plate_count; ++i)
            if (m_plate_list->get_plate(i)->contain_instance(it->first.first, it->first.second)) {
                it->second = i;
                break;
            }
        if (it->second < 0)
            continue;
        std::size_t& h = fingerprints[size_t(it->second)];
        hash_combine(h, v->composite_id.object_id);
        hash_combine(h, v->composite_id.volume_id);
        hash_combine(h, v->composite_id.instance_id);
        hash_combine(h, v->geometry_id.first);
        hash_combine(h, v->geometry_id.second);
        const Transform3d m = v->world_matrix();
        for (int k = 0; k < 16; ++k)
            hash_combine(h, m.data()[k]);
        for (int k = 0; k < 4; ++k)
            hash_combine(h, v->color.data()[k]);
    }

    for (int i = 0; i < m_plate_count; ++i) {
        Thumbnail& t = m_thumbnails[m_plate_list->get_plate(i)];
        if (t.texture && t.fingerprint == fingerprints[size_t(i)] && t.px == px)
            continue;
        ThumbnailData data;
        m_parent.render_thumbnail(data, unsigned(px), unsigned(px), ThumbnailsParams{ {}, false, true, false, true, i }, Camera::EType::Ortho, Camera::ViewAngleType::Iso);
        if (!t.texture)
            t.texture = std::make_unique<IMToolbarItem>();
        t.texture->set_thumbnail(data);
        t.fingerprint = fingerprints[size_t(i)];
        t.px          = px;
    }
}

void PlateArrangementDialog::update_icons()
{
    // Rasterised at the exact size they are drawn at, so they stay sharp; again when the DPI or the theme changes.
    const int px = int(m_gui_cfg->button_size - 2.f * m_gui_cfg->button_padding);
    if (!m_icons.empty() && px == m_icons_px && m_is_dark_mode == m_icons_dark)
        return;
    std::vector<std::string> files;
    for (const char* name : icon_names)
        files.push_back(resources_dir() + "/images/plate_arrangement_" + name + (m_is_dark_mode ? "_dark" : "") + ".svg");
    m_icons      = m_icon_manager.init(files, ImVec2(float(px), float(px)));
    m_icons_px   = px;
    m_icons_dark = m_is_dark_mode;
}

void PlateArrangementDialog::select(int index, bool extend, bool toggle)
{
    PartPlate* plate = m_plate_list->get_plate(index);
    m_cursor         = plate;

    if (toggle && !extend) { // Ctrl + click: the canvas keeps its plate
        if (is_selected(plate) && m_selection.size() > 1)
            m_selection.erase(plate);
        else
            m_selection.insert(plate);
        m_anchor = plate;
        return;
    }
    if (extend && m_anchor != nullptr) {
        if (!toggle)
            m_selection.clear();
        const int a = m_anchor->get_index();
        for (int i = std::min(a, index); i <= std::max(a, index); ++i)
            m_selection.insert(m_plate_list->get_plate(i));
    } else
        m_selection = { m_anchor = plate };
    set_canvas_plate(index);
}

void PlateArrangementDialog::set_canvas_plate(int index)
{
    PartPlate* plate = m_plate_list->get_plate(index);
    if (plate == m_canvas_plate)
        return;
    m_canvas_plate = plate;
    ++*m_pending_switches;
    wxGetApp().plater()->CallAfter([this, plate, pending = m_pending_switches]() {
        --*pending;
        if (const int i = index_of(plate); i >= 0 && i != m_plate_list->get_curr_plate_index()) {
            wxGetApp().plater()->select_plate(i);
            wxGetApp().plater()->deselect_all();
        }
    });
}

std::vector<bool> PlateArrangementDialog::selection_mask() const
{
    std::vector<bool> mask(static_cast<size_t>(m_plate_count));
    for (size_t i = 0; i < mask.size(); ++i)
        mask[i] = is_selected(m_plate_list->get_plate(int(i)));
    return mask;
}

std::vector<int> PlateArrangementDialog::order_after_move(Move move) const
{
    const std::vector<bool> sel = selection_mask();
    const int               n   = int(sel.size());
    std::vector<int>        order(sel.size());
    std::iota(order.begin(), order.end(), 0);

    switch (move) {
    case Move::First: std::stable_partition(order.begin(), order.end(), [&](int i) { return sel[i]; }); break;
    case Move::Last:  std::stable_partition(order.begin(), order.end(), [&](int i) { return !sel[i]; }); break;
    // Each selected plate steps over its unselected neighbour; adjacent selected plates move together.
    case Move::Previous:
        for (int k = 1; k < n; ++k)
            if (sel[order[k]] && !sel[order[k - 1]])
                std::swap(order[k], order[k - 1]);
        break;
    case Move::Next:
        for (int k = n - 2; k >= 0; --k)
            if (sel[order[k]] && !sel[order[k + 1]])
                std::swap(order[k], order[k + 1]);
        break;
    }
    return order;
}

std::vector<int> PlateArrangementDialog::order_after_drop(int insert_index) const
{
    const std::vector<bool> sel = selection_mask();
    std::vector<int>        moving, rest;
    for (int i = 0; i < int(sel.size()); ++i)
        (sel[i] ? moving : rest).push_back(i);
    // Once the selection is lifted, the slot is after the unselected plates in front of it.
    const auto pos = std::count_if(rest.begin(), rest.end(), [insert_index](int i) { return i < insert_index; });
    rest.insert(rest.begin() + pos, moving.begin(), moving.end());
    return rest;
}

bool PlateArrangementDialog::is_identity(const std::vector<int>& order)
{
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != int(i))
            return false;
    return true;
}

void PlateArrangementDialog::apply_order(const std::vector<int>& order)
{
    if (!is_identity(order)) // the plate list is changed outside of the ImGui frame
        wxGetApp().plater()->CallAfter([order]() { wxGetApp().plater()->arrange_plates(order); });
}

void PlateArrangementDialog::zoom_to_selection()
{
    if (m_selection.size() == 1)
        m_parent.zoom_to_plate((*m_selection.begin())->get_index());
    else {
        BoundingBoxf3 box;
        for (PartPlate* plate : m_selection)
            box.merge(plate->get_bounding_box(true));
        box.min.z() = box.max.z() = 0.;
        m_parent.zoom_to_box(box);
    }
}

} // namespace GUI
} // namespace Slic3r
