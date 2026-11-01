#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "IconManager.hpp"

class wxKeyEvent;

namespace Slic3r {
namespace GUI {

class GLCanvas3D;
class ImGuiWrapper;
class IMToolbarItem;
class PartPlate;

// ImGui window on the Prepare canvas for arranging plates by drag & drop, keyboard or the bottom bar buttons.
// Every change is applied to the plate list right away, as one undo step. Laid out like a gizmo: GLCanvas3D owns
// it, opens it with set_open(), draws it with render_input_window() and forwards it keys and double clicks.
class PlateArrangementDialog
{
public:
    explicit PlateArrangementDialog(GLCanvas3D& parent);
    ~PlateArrangementDialog(); // out of line: IMToolbarItem is incomplete here

    bool is_open() const { return m_open; }
    void set_open(bool open) { m_open = open; on_set_state(); }
    void toggle() { set_open(!m_open); }

    // Called from GLCanvas3D::_render_overlays() within the ImGui frame.
    void render_input_window();

    // Called by the canvas for each key event before its own shortcuts. Returns true when the dialog takes the
    // key; a key-down's decision also covers its char event and its release.
    bool on_key(const wxKeyEvent& evt);
    // The canvas forwards wx's double click: ImGui's own detection times clicks by its frame clock, which only
    // advances when the canvas redraws, so it misses most double clicks.
    void on_double_click() { m_double_click = m_open; }

    void on_change_color_mode(bool is_dark);

    // 2x2 up to 4 plates, 3x3 up to 9, 4x4 up to 16, 5x5 up to 25, else 6x6.
    static int grid_columns(int plate_count);

private:
    enum class Move { First, Previous, Next, Last };

    // resources/images/plate_arrangement_<name>[_dark].svg
    enum iconId {
        First, Previous, Next, Last, ZoomIn, ZoomOut, Locked, Unlocked, Settings, Arrange, Orient, Count
    };

    const char* const icon_names[iconId::Count] = {
        "first", "previous", "next", "last", "zoom_in", "zoom_out", "locked", "unlocked", "settings", "arrange", "orient" 
    };

    // The actions of the icons on a plate, as Plater::select_plate_by_hover_id() numbers them.
    enum PlateAction { ActionOrient = 2, ActionArrange = 3, ActionLock = 4, ActionSettings = 5 };

    void on_set_state();
    void on_render_input_window();

    // Parts of the window, in drawing order.
    void render_title_bar();
    void render_plate_grid();
    void render_plate(ImDrawList* dl, PartPlate* plate, const ImVec2& pmin, float size, bool selected, bool hovered, float alpha) const;
    void render_drag_preview() const;
    void render_bottom_bar();
    // A bottom bar button; `state` colours its background (locked, modified settings) like the plate's own icon.
    bool render_icon_button(iconId icon, bool enabled, const std::string& tooltip, const ImVec4* state = nullptr);

    // Sizes from the ImGui font, so they follow the display scaling; made again when the font size changes.
    void  create_gui_cfg();
    float cell_size(int cols) const;

    int index_of(PartPlate* plate);

    // Plates, previews and icons, brought up to date before each frame.
    void update_plates(int count);
    void update_thumbnails(int count);
    void update_icons();

    // Selection. The cursor is the plate keyboard moves start from; it is also the canvas' current plate.
    void select(int index, bool extend, bool toggle);
    void set_canvas_plate(int index);
    bool is_selected(PartPlate* plate) const { return m_selection.count(plate) != 0; }
    std::vector<bool> selection_mask() const;

    // Each returns new_order (new_order[k] = current index of the plate that goes to slot k).
    std::vector<int> order_after_move(Move move) const;
    std::vector<int> order_after_drop(int insert_index) const;
    static bool      is_identity(const std::vector<int>& order);
    static void      apply_order(const std::vector<int>& order);

    void zoom_to_selection();

    struct GuiCfg
    {
        // The design sizes in create_gui_cfg() are pixels at that scaling.
        float no_font_scale{1.1f}; // plate number size relative to the UI font
        float ghost_scale  {.85f}; // plate following the cursor while dragging
        float ghost_alpha  {.75f};
        float dragged_alpha{.35f}; // plates left behind while they are dragged
        int   thumbnail_max{512};
        int   max_columns  {6};   // densest grid; it sets the fixed size of the window
        float font_size{ 0.f };   // the ImGui font size the sizes below were made for
        float grid_size;          // edge of the square grid, fixed: sized for the densest grid
        float plate_spacing;      // between cells; the drop marker is drawn in it
        float plate_rounding;
        float plate_inset;        // number and state badges from the plate edge
        float border;             // selection and hover border; idle plates get a 1 px one
        float marker;             // drop marker
        float window_padding;
        float window_rounding;
        float frame_rounding;
        float title_height;       // also the height of the Close button
        float close_width;        // from the translated label
        float add_width; 
        float delete_width; 
        float button_size;        // bottom bar buttons, and the height of the name field
        float button_padding;     // around a button's icon; also its corner radius
        float item_spacing;
        float group_spacing;      // between button groups
        float row_spacing;        // between the title bar, the grid and the bottom bar rows
        float drag_threshold;
    };
    std::optional<GuiCfg> m_gui_cfg;

    struct Drag
    {
        bool   pressed{ false };   // left button went down on a plate
        bool   active{ false };    // ... and moved past the threshold
        int    press_index{ -1 };
        ImVec2 press_pos;
        ImVec2 grab_offset;        // cursor offset inside the pressed cell
        int    insert_index{ -1 }; // slot in [0, count], -1 when the drop would change nothing
    };

    struct Thumbnail
    {
        std::unique_ptr<IMToolbarItem> texture;
        std::size_t                    fingerprint{ 0 }; // of what was rendered, see update_thumbnails()
        int                            px{ 0 };
    };

    GLCanvas3D&   m_parent;
    ImGuiWrapper* m_imgui;
    bool          m_is_dark_mode{ false };

    bool   m_open{ false };
    bool   m_focused{ false };       // the window had the ImGui focus in the last frame
    bool   m_focus_request{ false };
    int    m_center_frames{ 0 };     // frames left to keep centering: an auto-sized window has its size from its 2nd frame on
    bool   m_positioned{ false };    // placed once: later opens in this session reuse m_window_pos
    bool   m_double_click{ false };  // for the frame drawn for a double click
    ImVec2 m_window_pos;
    ImVec2 m_window_size;
    float  m_info_button_size{ 0.f }; // GLGizmoUtils::render_tooltip_button() as drawn last frame, to centre it in its row
    Drag   m_drag;
    std::set<int> m_claimed_keys;    // keys whose key-down the dialog took

    std::set<PartPlate*> m_selection;
    PartPlate*           m_anchor{ nullptr };       // shift range start
    PartPlate*           m_cursor{ nullptr };
    PartPlate*           m_canvas_plate{ nullptr }; // the canvas' current plate as last seen
    // Plate switches the dialog asked for that have not run yet: they must not read as the user's own.
    std::shared_ptr<int> m_pending_switches{ std::make_shared<int>(0) };

    PartPlate*            m_name_plate{ nullptr };  // plate whose name is in m_name_buf
    std::array<char, 256> m_name_buf{};

    std::map<PartPlate*, Thumbnail> m_thumbnails;

    IconManager         m_icon_manager;
    IconManager::VIcons m_icons;
    int                 m_icons_px{ 0 };
    bool                m_icons_dark{ false };
};

} // namespace GUI
} // namespace Slic3r
