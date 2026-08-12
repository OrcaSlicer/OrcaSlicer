#pragma once

#include "GUI_Utils.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "libslic3r/TexturePainting.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include "Widgets/PopupWindow.hpp"
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/textctrl.h>
#include "Widgets/SpinInput.hpp"
#include <wx/checkbox.h>
#include <wx/button.h>
#include "Widgets/Button.hpp"
#include <wx/glcanvas.h>
#include <wx/event.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

class GreenSlider;

namespace Slic3r { namespace GUI {

wxDECLARE_EVENT(EVT_TEXTURE_COMPUTE_DONE, wxCommandEvent);
wxDECLARE_EVENT(EVT_TEXTURE_COMPUTE_PROGRESS, wxCommandEvent);
wxDECLARE_EVENT(EVT_TEXTURE_COMPUTE_ERROR, wxCommandEvent);
wxDECLARE_EVENT(EVT_TEXTURE_MESH_REPAIR_DECISION, wxCommandEvent);

enum class TextureImportState {
    Idle,
    Computing,
    Ready,
    Error
};

enum class TextureAutoMixMode {
    CMYW,
    RYBW
};

enum class TextureFilamentKind {
    ExistingPhysical,
    ExistingMixed,
    NewPhysical,
    NewMixed
};

struct TextureFilamentEntry {
    TextureFilamentKind kind{TextureFilamentKind::ExistingPhysical};
    int                 dialog_index{-1};
    size_t              project_config_index{size_t(-1)};
    std::string         color_hex;
    std::string         name;
    std::string         type;
    std::string         preset_name;
    std::vector<unsigned int> mixed_components;
    std::vector<int>          mixed_ratios;
};

struct TextureNewMixedFilament {
    int                       dialog_index{-1};
    std::string               color_hex;
    std::vector<int>          component_dialog_indices;
    std::vector<int>          ratios;
};

struct FilamentMappingRow {
    int                        cluster_id    = -1;
    std::array<std::size_t, 3> source_color  = {0, 0, 0};
    std::string                source_hex;
    int                        target_filament_idx = 0;
    wxPanel*                   source_panel  = nullptr;
    wxPanel*                   target_panel  = nullptr;
};

class FilamentSelectPopup;
class AutoMixSelectPopup;
// Lightweight 3D preview panel using wxGLCanvas.
// Renders: original textured, multi-color, or filament-mapped.
class TexturePreviewCanvas : public wxGLCanvas
{
public:
    enum class RenderMode { Original, MultiColor, FilamentMap };

    TexturePreviewCanvas(wxWindow* parent, const wxGLAttributes& attrs);
    ~TexturePreviewCanvas();

    void set_mesh_data(
        const std::vector<std::array<float, 3>>& vertices,
        const std::vector<std::array<int, 3>>&   indices);

    void set_texture_data(
        const std::vector<std::array<float, 2>>& uvs,
        const unsigned char* tex_data, int tex_w, int tex_h, int tex_channels);

    void set_texture_render_data(
        const std::vector<std::vector<unsigned char>>& tex_pixels_rgb,
        const std::vector<int>& tex_widths,
        const std::vector<int>& tex_heights,
        const std::vector<std::array<std::array<float,2>, 3>>& face_uvs,
        const std::vector<int>& face_tex_ids);

    void set_painted_mesh_data(
        const std::vector<std::array<float, 3>>& vertices,
        const std::vector<std::array<int, 3>>&   indices);
    void set_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors);
    void set_original_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors);
    void set_filament_color_map(const std::map<std::array<std::size_t, 3>, std::array<float, 3>>& color_map);

    void set_render_mode(RenderMode mode);
    RenderMode get_render_mode() const { return m_mode; }
    void set_computing_overlay(bool show);
    void reset_view();

private:
    void on_paint(wxPaintEvent& evt);
    void on_size(wxSizeEvent& evt);
    void on_mouse(wxMouseEvent& evt);
    void ensure_gl_ready();
    void render();
    void render_mesh();
    void render_textured_original();
    void render_reset_overlay(const wxSize& logical_size, const wxSize& viewport_size);
    void upload_reset_icon_textures();
    unsigned int upload_reset_icon_texture(const std::string& icon_name);
    wxRect reset_overlay_rect() const;
    bool handle_reset_overlay_mouse(wxMouseEvent& evt);
    void upload_textures();
    void compute_smooth_normals();
    void update_bounding_box();

    wxGLContext*  m_context        = nullptr;
    bool          m_gl_initialized = false;
    RenderMode    m_mode           = RenderMode::Original;

    float   m_zoom     = 1.0f;
    float   m_rot_x    = -30.0f;
    float   m_rot_y    = 30.0f;
    float   m_pan_x    = 0.0f;
    float   m_pan_y    = 0.0f;
    wxPoint m_last_mouse_pos;
    enum class DragMode { None, Rotate, Pan };
    DragMode m_drag_mode = DragMode::None;

    std::vector<std::array<float, 3>> m_vertices;
    std::vector<std::array<int, 3>>   m_indices;
    std::vector<std::array<float, 2>> m_uvs;
    std::vector<std::array<float, 3>> m_painted_vertices;
    std::vector<std::array<int, 3>>   m_painted_indices;
    std::vector<std::array<float, 3>> m_face_colors_rgb;
    std::vector<std::array<float, 3>> m_original_face_colors_rgb;
    std::vector<std::array<float, 3>> m_filament_colors_rgb;
    std::map<std::array<std::size_t, 3>, std::array<float, 3>> m_color_map;

    unsigned int m_tex_id       = 0;
    int          m_tex_w        = 0;
    int          m_tex_h        = 0;
    int          m_tex_channels = 3;
    bool         m_tex_dirty    = false;
    std::vector<unsigned char> m_tex_data;

    std::vector<unsigned int> m_gl_tex_ids;
    std::vector<std::vector<unsigned char>> m_tex_pixels_rgb;
    std::vector<int> m_tex_widths;
    std::vector<int> m_tex_heights;
    std::vector<std::array<std::array<float,2>, 3>> m_face_uvs;
    std::vector<int> m_face_tex_ids;
    bool m_multi_tex_dirty = false;

    std::vector<std::array<float, 3>> m_vertex_normals;

    std::array<float, 3> m_center = {0, 0, 0};
    float                m_radius = 1.0f;

    unsigned int m_reset_icon_tex        = 0;
    unsigned int m_reset_icon_hover_tex  = 0;
    unsigned int m_reset_icon_dark_tex   = 0;
    unsigned int m_reset_icon_dark_hover_tex = 0;
    bool         m_reset_overlay_hovered = false;
    bool         m_reset_overlay_pressed = false;
};


class TextureImportDialog : public DPIDialog
{
public:
    TextureImportDialog(wxWindow*                        parent,
                        const Slic3r::TexturedMesh&      textured_mesh,
                        const std::vector<TextureFilamentEntry>& filament_entries,
                        std::function<bool()>            initial_cancel_callback = {},
                        std::function<bool(int)>         initial_progress_callback = {});
    ~TextureImportDialog();

    int ShowModal() override;
    void on_dpi_changed(const wxRect& suggested_rect) override;

    Slic3r::PaintedMesh               get_painted_mesh() const;
    std::vector<Slic3r::FilamentMatch> get_matches() const;
    bool                               was_skipped() const { return m_skipped; }
    bool                               fallback_to_geometry_only() const { return m_fallback_to_geometry_only; }
    // Colors of virtual filaments that need to be created after dialog confirmation.
    // Index i corresponds to filament index (m_existing_filament_count + i).
    const std::vector<std::array<float, 4>>& get_new_filament_colors() const { return m_new_filament_colors; }
    const std::vector<std::string>& get_new_filament_preset_names() const { return m_new_filament_preset_names; }
    const std::vector<TextureNewMixedFilament>& get_new_mixed_filaments() const { return m_new_mixed_filaments; }
    const std::vector<TextureFilamentEntry>& get_filament_entries() const { return m_filament_entries; }
    size_t get_existing_filament_count() const { return m_existing_filament_count; }

private:
    void build_ui();
    void build_preview_panel(wxWindow* parent, wxSizer* sizer);
    void build_params_panel(wxWindow* parent, wxSizer* sizer);
    void build_mapping_panel(wxWindow* parent, wxSizer* sizer);
    void build_bottom_buttons(wxSizer* sizer);

    void set_state(TextureImportState new_state);
    void update_ui_for_state();

    void start_computation(bool auto_color = false, bool initial = false);
    void cancel_computation();
    void on_computation_complete(wxCommandEvent& evt);
    void on_computation_progress(wxCommandEvent& evt);
    void on_computation_error(wxCommandEvent& evt);
    void on_mesh_repair_decision_required(wxCommandEvent& evt);

    void rebuild_mapping_rows();
    void do_auto_match();
    // Reorder m_current_matches into a canonical, predictable order (ascending
    // filament_index, with unmapped entries pushed to the end). Used right
    // after the initial computation so the first view the user sees has a
    // stable, intuitive layout.
    void sort_current_matches_by_filament_index();
    // Reorder m_current_matches so they appear in the same order as
    // `previous_matches` (keyed by cluster_index). Entries whose cluster_index
    // was not present before are appended at the end, preserving their current
    // relative order. Used when the user toggles auto-merge so the rows do not
    // visually jump around. Assumes each cluster_index appears at most once in
    // both vectors (this invariant is currently guaranteed by do_auto_match,
    // which produces one match per cluster).
    void restore_current_match_order(const std::vector<Slic3r::FilamentMatch>& previous_matches);
    std::vector<Slic3r::FilamentMatch> build_matches_from_rows() const;
    void update_filament_color_map();
    void show_filament_popup(size_t row_index);
    void dismiss_filament_popup();
    void dismiss_filament_popup_on_wheel(wxMouseEvent& evt);
    void show_auto_mix_popup();
    void dismiss_auto_mix_popup();
    void set_auto_mix_mode(TextureAutoMixMode mode);
    void apply_auto_standard_mix(TextureAutoMixMode mode);
    void reset_auto_mix();
    void update_auto_mix_reset_visibility();
    bool add_decomposed_mixed_filament(size_t row_index);
    int  add_virtual_filament(const std::array<float, 4>& rgba, const std::string& hex,
                              const std::string& preset_name = std::string());
    int  add_virtual_mixed_filament(const std::string& color_hex,
                                    const std::vector<int>& component_dialog_indices,
                                    const std::vector<int>& ratios);
    size_t max_filament_count() const;
    bool can_add_virtual_filament() const;
    // Recomputes m_drop_warning_label visibility from m_filaments_dropped and
    // m_state. Safe to call whether or not the label has been created yet.
    // Visibility reflects ONLY the result of the most recent do_auto_match():
    // if the latest match did not drop any cluster, the label is hidden even
    // if a previous match had dropped (no historical accumulation).
    void update_drop_warning_visibility();
    void compact_used_virtual_filaments();
    int  find_closest_filament_index(const std::array<std::size_t, 3>& color) const;

    void on_color_preset_clicked(wxCommandEvent& evt);
    void on_color_slider_changed(wxCommandEvent& evt);
    void on_color_spin_changed(wxCommandEvent& evt);
    void on_color_spin_text_changed(wxCommandEvent& evt);
    void on_smooth_slider_changed(wxCommandEvent& evt);
    void on_smooth_spin_changed(wxCommandEvent& evt);
    void on_smooth_spin_text_changed(wxCommandEvent& evt);
    void on_apply_clicked(wxCommandEvent& evt);
    void on_auto_merge_toggled(wxCommandEvent& evt);
    void highlight_view_button(int view_index);
    void on_skip_clicked(wxCommandEvent& evt);
    void on_ok_clicked(wxCommandEvent& evt);

    void set_color_count_value(int value, bool update_spin);
    void set_smooth_value(int value, bool update_spin);
    void preview_spin_text_value(SpinInput* spin, GreenSlider* slider, int& param,
                                 int min_value, int max_value, const wxString& text,
                                 std::function<void()> on_value_changed = {});
    void update_color_count_preset_buttons();

    bool has_valid_result() const;
    bool is_params_dirty() const;
    void update_confirm_button_state();

    Slic3r::TexturedMesh               m_textured_mesh;
    std::vector<std::string>           m_filament_color_strs;   // existing + virtual
    std::vector<std::string>           m_filament_names;        // existing + virtual
    std::vector<std::array<float, 4>>  m_filament_colors_rgba;  // existing + virtual
    std::vector<TextureFilamentEntry>  m_filament_entries;      // aligned with m_filament_colors_rgba
    size_t                             m_existing_filament_count = 0;
    std::vector<std::array<float, 4>>  m_new_filament_colors;   // only virtual (to be created)
    std::vector<std::string>           m_new_filament_preset_names; // only virtual, aligned with m_new_filament_colors
    std::vector<TextureNewMixedFilament> m_new_mixed_filaments;
    std::string                        m_default_virtual_filament_preset_name;

    TextureImportState                 m_state = TextureImportState::Idle;
    bool                               m_skipped = false;
    bool                               m_fallback_to_geometry_only = false;
    // True iff *the most recent* do_auto_match() ran into the global filament
    // limit and had to drop one or more clusters. Reset to false on every
    // do_auto_match() entry so it never accumulates across runs: a run that
    // does not drop anything must observe false here, regardless of whether
    // previous runs dropped. Drives the inline orange warning above the
    // bottom buttons; never affects the mapping itself.
    bool                               m_filaments_dropped = false;
    bool                               m_auto_merge_enabled = true;
    TextureAutoMixMode                 m_auto_mix_mode = TextureAutoMixMode::CMYW;
    int                                m_auto_mix_font_point_size = 10;

    Slic3r::PaintedMesh               m_painted;
    std::vector<Slic3r::FilamentMatch> m_current_matches;

    std::unique_ptr<std::thread>       m_worker;
    std::atomic<bool>                  m_cancel_flag{false};
    std::mutex                         m_result_mutex;
    Slic3r::PaintedMesh               m_pending_result;
    std::function<bool()>              m_initial_cancel_callback;
    std::function<bool(int)>           m_initial_progress_callback;
    bool                               m_current_computation_initial = false;
    bool                               m_initial_computation_pending = false;
    bool                               m_initial_computation_cancelled = false;
    bool                               m_initial_computation_failed = false;
    bool                               m_initial_tooltips_set = false;
    bool                               m_current_computation_auto_color = false;
    Slic3r::TexturePaintingSettings::MeshRepairDecision m_mesh_repair_decision =
        Slic3r::TexturePaintingSettings::MeshRepairDecision::Ask;

    Button*      m_btn_color_4    = nullptr;
    Button*      m_btn_color_8    = nullptr;
    Button*      m_btn_color_16   = nullptr;
    Button*      m_btn_color_auto = nullptr;
    GreenSlider* m_color_slider   = nullptr;
    SpinInput*   m_color_spin     = nullptr;
    GreenSlider* m_smooth_slider  = nullptr;
    SpinInput*   m_smooth_spin    = nullptr;
    Button*      m_btn_apply      = nullptr;

    wxCheckBox*           m_auto_merge_cb = nullptr;
    Button*               m_btn_auto_mix  = nullptr;
    Button*               m_btn_mix_reset = nullptr;
    bool                  m_auto_mix_applied = false;
    AutoMixSelectPopup*   m_auto_mix_popup = nullptr;
    wxScrolledWindow*     m_mapping_scroll = nullptr;
    wxBoxSizer*           m_mapping_sizer  = nullptr;
    std::vector<FilamentMappingRow> m_mapping_rows;
    FilamentSelectPopup*  m_filament_popup = nullptr;
    int                   m_filament_popup_row = -1;
    int                   m_skip_next_filament_popup_row = -1;

    TexturePreviewCanvas* m_preview_canvas      = nullptr;
    wxPanel*              m_tab_panel           = nullptr;
    Button*               m_btn_view_original   = nullptr;
    Button*               m_btn_view_multicolor = nullptr;

    ProgressDialog* m_progress_dlg = nullptr;

    Button*       m_btn_skip = nullptr;
    Button*       m_btn_ok   = nullptr;
    wxStaticText* m_drop_warning_label = nullptr;

    int   m_param_color_count = 4;
    int   m_param_smooth      = 5;

    int   m_applied_color_count = -1;
    int   m_applied_smooth      = -1;
    wxStaticText* m_hint_label  = nullptr;

    static const int ID_COLOR_4     = wxID_HIGHEST + 200;
    static const int ID_COLOR_8     = wxID_HIGHEST + 201;
    static const int ID_COLOR_16    = wxID_HIGHEST + 202;
    static const int ID_COLOR_AUTO  = wxID_HIGHEST + 203;
    static const int ID_BTN_APPLY   = wxID_HIGHEST + 204;
    static const int ID_BTN_SKIP    = wxID_HIGHEST + 205;
    static const int ID_VIEW_ORIGINAL   = wxID_HIGHEST + 206;
    static const int ID_VIEW_MULTICOLOR = wxID_HIGHEST + 207;

    wxDECLARE_EVENT_TABLE();
};

}} // namespace Slic3r::GUI
