#pragma once

#include "GUI_Utils.hpp"

#include <wx/wx.h>
#include <wx/clrpicker.h>
#include <wx/gauge.h>
#include <wx/wrapsizer.h>
#include <wx/scrolwin.h>

#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include "MixedColorMatchHelpers.hpp"

namespace Slic3r { namespace GUI {

// Embeddable panel that hosts the full color-match UI (color map, hex input,
// range slider, preset swatches, recipe summary, loading indicator).
// Fires wxEVT_SLIDER on its own ID whenever the selected recipe changes.
class MixedColorMatchPanel : public wxPanel
{
public:
    MixedColorMatchPanel(wxWindow                        *parent,
                         const std::vector<std::string>  &physical_colors,
                         const wxColour                  &initial_color);
    ~MixedColorMatchPanel() override;

    // Start the initial async recipe search (call after the panel is shown).
    void begin_initial_recipe_load();

    // Set target color from external UI and trigger recipe search.
    void set_target_color(const wxColour &target);
    void set_min_component_percent(int pct);

    MixedColorMatchRecipeResult selected_recipe() const { return m_selected_recipe; }
    bool                        has_valid_recipe()  const { return m_selected_recipe.valid && !m_recipe_loading; }

private:
    // ---- helpers ----
    void sync_recipe_preview(MixedColorMatchRecipeResult &recipe, const wxColour *requested = nullptr);
    void update_range_label();
    void rebuild_presets_ui();
    void set_recipe_loading(bool loading, const wxString &message);
    void sync_inputs_to_requested();
    bool apply_requested_target(const wxColour &target);
    bool apply_hex_input(bool show_invalid_error);
    void request_recipe_match(const wxColour &target, bool debounce, const wxString &loading_message);
    void refresh_selected_recipe();
    void launch_recipe_match(size_t request_token, const wxColour &target);
    void handle_recipe_result(size_t request_token, const wxColour &requested, MixedColorMatchRecipeResult recipe);
    void apply_preset(MixedColorMatchRecipeResult preset);
    void update_panel_state();

    // ---- forward-declared inner classes ----
    class ColorMapPanel;
    class StripedPreviewPanel;

    // ---- data ----
    std::vector<std::string>                 m_physical_colors;
    MixedFilamentDisplayContext              m_display_context;
    std::vector<wxColour>                    m_palette;
    std::vector<MixedColorMatchRecipeResult> m_presets;

    ColorMapPanel        *m_color_map            = nullptr;
    wxTextCtrl           *m_hex_input            = nullptr;
    wxColourPickerCtrl   *m_classic_picker       = nullptr;
    wxSlider             *m_range_slider         = nullptr;
    wxStaticText         *m_range_value          = nullptr;
    // Target color row (left column, bottom)
    wxPanel              *m_target_color_swatch  = nullptr;
    wxStaticText         *m_target_color_label   = nullptr;
    // Right column
    wxStaticText         *m_recipe_formula_label = nullptr;
    wxPanel              *m_recipe_preview       = nullptr;   // solid "混色效果" swatch
    StripedPreviewPanel  *m_striped_preview      = nullptr;   // striped "混色预览"
    // Presets
    wxStaticText         *m_presets_label        = nullptr;
    wxScrolledWindow     *m_presets_host         = nullptr;
    wxWrapSizer          *m_presets_sizer        = nullptr;
    // Loading
    wxStaticText         *m_loading_label        = nullptr;
    wxGauge              *m_loading_gauge        = nullptr;
    // Summary (kept for update_panel_state compat)
    wxPanel              *m_selected_preview     = nullptr;
    wxStaticText         *m_selected_label       = nullptr;
    wxStaticText         *m_recipe_label         = nullptr;
    wxStaticText         *m_delta_label          = nullptr;
    wxStaticText         *m_error_label          = nullptr;

    wxColour                     m_requested_target { wxColour("#26A69A") };
    wxColour                     m_selected_target  { wxColour("#26A69A") };
    MixedColorMatchRecipeResult  m_selected_recipe;
    wxTimer                      m_recipe_timer;
    wxTimer                      m_loading_timer;
    wxString                     m_loading_message;
    size_t                       m_recipe_request_token  { 0 };
    int                          m_min_component_percent { 15 };
    bool                         m_has_recipe_result     { false };
    bool                         m_recipe_loading        { false };
    bool                         m_recipe_refresh_pending{ false };
    bool                         m_syncing_inputs        { false };
    // Shared with background threads so they can detect panel destruction safely.
    std::shared_ptr<std::atomic<bool>> m_destroyed { std::make_shared<std::atomic<bool>>(false) };
};

}} // namespace Slic3r::GUI
