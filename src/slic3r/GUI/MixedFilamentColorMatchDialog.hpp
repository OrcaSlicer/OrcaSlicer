#pragma once
#include <wx/dialog.h>
#include <wx/clrpicker.h>
#include <wx/colour.h>
#include <wx/gauge.h>
#include <wx/scrolwin.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/wrapsizer.h>
#include <limits>
#include <string>
#include <vector>
#include "GUI_Utils.hpp"
#include "libslic3r/MixedFilament.hpp"

namespace Slic3r { namespace GUI {

class MixedFilamentColorMapPanel; // Task 17

// ---------------------------------------------------------------------------
// MixedColorMatchRecipeResult
//
// Verbatim from FullSpectrum Plater.cpp:230-242.
// Holds the result of a brute-force C(N,2)/C(N,3)/C(N,4) ΔE₀₀ search.
// ---------------------------------------------------------------------------
struct MixedColorMatchRecipeResult
{
    bool         cancelled                  = false;
    bool         valid                      = false;
    unsigned int component_a                = 1;
    unsigned int component_b                = 2;
    int          mix_b_percent              = 50;
    std::string  manual_pattern;
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    wxColour     preview_color              = wxColour("#26A69A");
    double       delta_e                    = std::numeric_limits<double>::infinity();
};

// Free helper (was declared at Plater.cpp:244-246): launch dialog, return recipe.
MixedColorMatchRecipeResult prompt_best_color_match_recipe(
    wxWindow *parent,
    const std::vector<std::string> &physical_colors,
    const wxColour &initial_color);

// Free helper (was declared at Plater.cpp:251):
// build a MixedFilamentDisplayContext from a flat color vector.
MixedFilamentDisplayContext build_mixed_filament_display_context(
    const std::vector<std::string> &physical_colors);

// Free helper (was declared in Plater.cpp anon namespace at 252):
// map a recipe to a swatch colour using the display context.
wxColour compute_color_match_recipe_display_color(
    const MixedColorMatchRecipeResult &recipe,
    const MixedFilamentDisplayContext &context);

// Free helper (was declared at Plater.cpp:247): ΔE₀₀ between two wxColours.
double color_delta_e00(const wxColour &lhs, const wxColour &rhs);

// ---------------------------------------------------------------------------
// MixedFilamentColorMatchDialog
//
// Extracted verbatim from FullSpectrum Plater.cpp:3782-4288.
// The user types/picks an arbitrary target colour and a brute-force search
// finds the C(N,2)/C(N,3)/C(N,4) recipe that minimises ΔE₀₀ to that target.
// ---------------------------------------------------------------------------
class MixedFilamentColorMatchDialog : public DPIDialog
{
public:
    MixedFilamentColorMatchDialog(wxWindow *parent,
                                  const std::vector<std::string> &physical_colors,
                                  const wxColour &initial_color);

    ~MixedFilamentColorMatchDialog() override;

    // Kick off the initial background recipe search (called after ShowModal starts).
    void begin_initial_recipe_load();

    MixedColorMatchRecipeResult selected_recipe() const { return m_selected_recipe; }

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    // UI helpers
    void update_range_label();
    void rebuild_presets_ui();
    void set_recipe_loading(bool loading, const wxString &message);
    void sync_inputs_to_requested();
    bool apply_requested_target(const wxColour &requested_target);
    bool apply_hex_input(bool show_invalid_error);
    void request_recipe_match(const wxColour &requested_target, bool debounce, const wxString &loading_message);
    void refresh_selected_recipe();
    void launch_recipe_match(size_t request_token, const wxColour &requested_target);
    void update_dialog_state();

    // Declared in the task spec's public API
    void sync_recipe_preview(MixedColorMatchRecipeResult &recipe, const wxColour *requested_target = nullptr);
    void handle_recipe_result(size_t request_token, const wxColour &requested_target, MixedColorMatchRecipeResult recipe);
    void apply_preset(MixedColorMatchRecipeResult preset);

    // Data
    std::vector<std::string>                 m_physical_colors;
    MixedFilamentDisplayContext              m_display_context;
    std::vector<wxColour>                    m_palette;
    std::vector<MixedColorMatchRecipeResult> m_presets;
    MixedFilamentColorMapPanel              *m_color_map        = nullptr;

    // Widgets
    wxTextCtrl        *m_hex_input        = nullptr;
    wxColourPickerCtrl *m_classic_picker  = nullptr;
    wxSlider          *m_range_slider     = nullptr;
    wxStaticText      *m_range_value      = nullptr;
    wxStaticText      *m_presets_label    = nullptr;
    wxScrolledWindow  *m_presets_host     = nullptr;
    wxWrapSizer       *m_presets_sizer    = nullptr;
    wxPanel           *m_loading_panel    = nullptr;
    wxStaticText      *m_loading_label    = nullptr;
    wxGauge           *m_loading_gauge    = nullptr;
    wxPanel           *m_selected_preview = nullptr;
    wxStaticText      *m_selected_label   = nullptr;
    wxPanel           *m_recipe_preview   = nullptr;
    wxStaticText      *m_recipe_label     = nullptr;
    wxStaticText      *m_delta_label      = nullptr;
    wxStaticText      *m_error_label      = nullptr;

    // State
    wxColour                    m_requested_target { wxColour("#26A69A") };
    wxColour                    m_selected_target  { wxColour("#26A69A") };
    MixedColorMatchRecipeResult m_selected_recipe;
    wxTimer                     m_recipe_timer;
    wxTimer                     m_loading_timer;
    wxString                    m_loading_message;
    size_t                      m_recipe_request_token   { 0 };
    int                         m_min_component_percent  { 15 };
    bool                        m_has_recipe_result      { false };
    bool                        m_recipe_loading         { false };
    bool                        m_recipe_refresh_pending { false };
    bool                        m_syncing_inputs         { false };
};

} } // namespace Slic3r::GUI
