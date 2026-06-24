#include "MixedColorMatchPanel.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "MixedFilamentColorMapPanel.hpp"
#include "MixedGradientSelector.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include <wx/dcbuffer.h>
#include <wx/bmpbuttn.h>
#include <wx/sizer.h>
#include <wx/statline.h>

#include <thread>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// ColorMapPanel — thin wrapper around MixedFilamentColorMapPanel
// ---------------------------------------------------------------------------

class MixedColorMatchPanel::ColorMapPanel : public wxPanel
{
public:
    ColorMapPanel(wxWindow *parent, const std::vector<wxColour> &palette, const wxSize &min_size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, min_size, wxBORDER_SIMPLE)
    {
        std::vector<unsigned int> ids;
        ids.reserve(palette.size());
        for (size_t i = 0; i < palette.size(); ++i)
            ids.emplace_back(unsigned(i + 1));

        std::vector<int> initial_weights(palette.size(), 0);
        if (!initial_weights.empty()) initial_weights[0] = 100;
        if (initial_weights.size() >= 2) { initial_weights[0] = 50; initial_weights[1] = 50; }

        m_inner = create_color_map_panel(this, ids, palette, initial_weights, min_size);

        auto *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_inner, 1, wxEXPAND);
        SetSizer(sizer);

        m_inner->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            wxCommandEvent evt(wxEVT_SLIDER, GetId());
            evt.SetEventObject(this);
            ProcessWindowEvent(evt);
        });
    }

    wxColour selected_color() const { return m_inner->selected_color(); }

    void set_normalized_weights(const std::vector<int> &weights, bool notify)
    {
        m_inner->set_normalized_weights(weights, notify);
    }

    void set_min_component_percent(int pct) { m_inner->set_min_component_percent(pct); }

private:
    MixedFilamentColorMapPanel *m_inner = nullptr;
};

// ---------------------------------------------------------------------------
// StripedPreviewPanel — draws horizontal color stripes proportional to weights
// ---------------------------------------------------------------------------

class MixedColorMatchPanel::StripedPreviewPanel : public wxPanel
{
public:
    StripedPreviewPanel(wxWindow *parent, const wxSize &size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_SIMPLE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(size);
        Bind(wxEVT_PAINT, &StripedPreviewPanel::on_paint, this);
    }

    void set_colors_and_weights(const std::vector<wxColour> &colors, const std::vector<int> &weights)
    {
        m_colors  = colors;
        m_weights = weights;
        Refresh();
    }

private:
    static constexpr int SEGMENTS = 24;

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(GetBackgroundColour().IsOk() ? GetBackgroundColour() : wxColour(245, 245, 245)));
        dc.Clear();

        if (m_colors.empty() || m_weights.empty()) return;

        // Build a Bresenham-scheduled sequence of colour indices, same as draw_strip.
        std::vector<int> ids, counts;
        int total = 0;
        for (int w : m_weights) total += std::max(0, w);
        if (total <= 0) return;
        for (size_t i = 0; i < m_weights.size(); ++i)
            if (m_weights[i] > 0) { ids.push_back((int)i); counts.push_back(m_weights[i]); }

        std::vector<int> pattern;
        pattern.reserve(SEGMENTS);
        std::vector<int> emitted(counts.size(), 0);
        for (int pos = 0; pos < SEGMENTS; ++pos) {
            int best = 0; double best_score = -1e9;
            for (int i = 0; i < (int)counts.size(); ++i) {
                double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                if (score > best_score) { best_score = score; best = i; }
            }
            ++emitted[best];
            pattern.push_back(ids[best]);
        }

        const int seg = std::max(1, sz.GetHeight() / SEGMENTS);
        for (int s = 0; s < SEGMENTS; ++s) {
            const int idx = pattern[s];
            const wxColour &c = (idx >= 0 && idx < (int)m_colors.size() && m_colors[idx].IsOk())
                ? m_colors[idx] : wxColour(200, 200, 200);
            dc.SetBrush(wxBrush(c));
            dc.SetPen(*wxTRANSPARENT_PEN);
            const int y   = s * seg;
            const int len = (s == SEGMENTS - 1) ? (sz.GetHeight() - y) : seg;
            dc.DrawRectangle(0, y, sz.GetWidth(), len);
        }
    }

    std::vector<wxColour> m_colors;
    std::vector<int>      m_weights;
};

// ---------------------------------------------------------------------------
// MixedColorMatchPanel
// ---------------------------------------------------------------------------

MixedColorMatchPanel::MixedColorMatchPanel(wxWindow *parent,
                                           const std::vector<std::string> &physical_colors,
                                           const wxColour &initial_color)
    : wxPanel(parent, wxID_ANY)
    , m_physical_colors(physical_colors)
{
    m_recipe_timer.SetOwner(this);
    m_loading_timer.SetOwner(this);
    m_display_context = build_mixed_filament_display_context(m_physical_colors);

    m_palette.reserve(m_physical_colors.size());
    for (const std::string &hex : m_physical_colors)
        m_palette.emplace_back(parse_mixed_color(hex));

    const wxColour safe_initial = initial_color.IsOk() ? initial_color :
        (m_palette.size() >= 2 ? blend_pair_filament_mixer(m_palette[0], m_palette[1], 0.5f) : wxColour("#26A69A"));

    m_requested_target = safe_initial;
    m_selected_target  = safe_initial;

    const int M  = FromDIP(8);
    const int M2 = FromDIP(4);

    // -----------------------------------------------------------------------
    // Root: vertical sizer — top half (left/right split) + bottom half
    // -----------------------------------------------------------------------
    auto *root = new wxBoxSizer(wxVERTICAL);

    // -----------------------------------------------------------------------
    // Top half: horizontal split
    // -----------------------------------------------------------------------
    auto *top_row = new wxBoxSizer(wxHORIZONTAL);

    // ---- LEFT COLUMN -------------------------------------------------------
    auto *left_col = new wxBoxSizer(wxVERTICAL);

    // Legend row: colored dots + filament labels
    auto *legend_row = new wxBoxSizer(wxHORIZONTAL);
    legend_row->Add(new wxStaticText(this, wxID_ANY, _L("Legend:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
    for (size_t i = 0; i < m_palette.size(); ++i) {
        auto *dot = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(12), FromDIP(12)), wxBORDER_SIMPLE);
        dot->SetBackgroundColour(m_palette[i]);
        legend_row->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
        legend_row->Add(new wxStaticText(this, wxID_ANY,
            wxString::Format(_L("F%zu"), i + 1)), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
    }
    left_col->Add(legend_row, 0, wxEXPAND | wxBOTTOM, M2);

    // Color map
    m_color_map = new ColorMapPanel(this, m_palette, wxSize(FromDIP(260), FromDIP(200)));
    left_col->Add(m_color_map, 1, wxEXPAND | wxBOTTOM, M);

    // Range slider row
    auto *range_row = new wxBoxSizer(wxHORIZONTAL);
    range_row->Add(new wxStaticText(this, wxID_ANY, _L("Min ratio")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
    m_range_slider = new wxSlider(this, wxID_ANY, m_min_component_percent, 0, 50);
    m_range_slider->SetToolTip(_L("Minimum percent for each participating color."));
    range_row->Add(m_range_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
    m_range_value = new wxStaticText(this, wxID_ANY, wxEmptyString);
    range_row->Add(m_range_value, 0, wxALIGN_CENTER_VERTICAL);
    left_col->Add(range_row, 0, wxEXPAND | wxBOTTOM, M2);

    // Target color row
    auto *target_row = new wxBoxSizer(wxHORIZONTAL);
    target_row->Add(new wxStaticText(this, wxID_ANY, _L("Target")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
    m_target_color_swatch = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                        wxSize(FromDIP(56), FromDIP(18)), wxBORDER_SIMPLE);
    target_row->Add(m_target_color_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M2);
    m_hex_input = new wxTextCtrl(this, wxID_ANY,
        normalize_color_match_hex(safe_initial.GetAsString(wxC2S_HTML_SYNTAX)),
        wxDefaultPosition, wxSize(FromDIP(80), -1), wxTE_PROCESS_ENTER);
    m_hex_input->SetToolTip(_L("Enter a hex color like #00FF88."));
    target_row->Add(m_hex_input, 0, wxALIGN_CENTER_VERTICAL);
    target_row->AddSpacer(FromDIP(6));
    m_classic_picker = new wxColourPickerCtrl(this, wxID_ANY, safe_initial);
    m_classic_picker->SetToolTip(_L("Classic color picker."));
    target_row->Add(m_classic_picker, 0, wxALIGN_CENTER_VERTICAL);
    left_col->Add(target_row, 0, wxEXPAND);

    top_row->Add(left_col, 1, wxEXPAND | wxRIGHT, M * 2);

    // ---- RIGHT COLUMN ------------------------------------------------------
    auto *right_col = new wxBoxSizer(wxVERTICAL);

    // Recipe formula label (e.g. "F1(40%)+F2(30%)+F3(30%)")
    m_recipe_formula_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    right_col->Add(m_recipe_formula_label, 0, wxEXPAND | wxBOTTOM, M2);

    // Large solid color swatch — "Blend result"
    m_recipe_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                   wxSize(FromDIP(120), FromDIP(100)), wxBORDER_SIMPLE);
    right_col->Add(m_recipe_preview, 0, wxEXPAND | wxBOTTOM, M2);
    right_col->Add(new wxStaticText(this, wxID_ANY, _L("Blend result")), 0, wxALIGN_CENTER | wxBOTTOM, M);

    // Striped preview — "Color preview"
    m_striped_preview = new StripedPreviewPanel(this, wxSize(FromDIP(120), FromDIP(100)));
    right_col->Add(m_striped_preview, 0, wxEXPAND | wxBOTTOM, M2);
    right_col->Add(new wxStaticText(this, wxID_ANY, _L("Color preview")), 0, wxALIGN_CENTER);

    top_row->Add(right_col, 0, wxEXPAND);

    root->Add(top_row, 0, wxEXPAND | wxBOTTOM, M);

    // -----------------------------------------------------------------------
    // Bottom half (unchanged structure)
    // -----------------------------------------------------------------------

    // Summary grid (hidden selected_preview / selected_label kept for compat)
    m_selected_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(0, 0));
    m_selected_preview->Hide();
    m_selected_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_selected_label->Hide();
    m_recipe_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_recipe_label->Hide();

    m_delta_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_delta_label, 0, wxEXPAND | wxBOTTOM, M2);

    // Presets
    m_presets_label = new wxStaticText(this, wxID_ANY, _L("Exact preset mixes"));
    root->Add(m_presets_label, 0, wxBOTTOM, M2);
    m_presets_host = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(72)),
                                          wxVSCROLL | wxBORDER_SIMPLE);
    m_presets_host->SetScrollRate(FromDIP(6), FromDIP(6));
    m_presets_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_presets_host->SetSizer(m_presets_sizer);
    root->Add(m_presets_host, 0, wxEXPAND | wxBOTTOM, M);

    m_error_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_error_label->SetForegroundColour(wxColour(196, 67, 63));
    root->Add(m_error_label, 0, wxEXPAND | wxBOTTOM, M2);

    // Loading indicator
    auto *loading_row = new wxBoxSizer(wxHORIZONTAL);
    m_loading_label = new wxStaticText(this, wxID_ANY, " ");
    loading_row->Add(m_loading_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
    m_loading_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(100), FromDIP(8)),
                                  wxGA_HORIZONTAL | wxGA_SMOOTH);
    m_loading_gauge->SetValue(0);
    m_loading_gauge->Enable(false);
    loading_row->Add(m_loading_gauge, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(loading_row, 0, wxEXPAND);

    SetSizer(root);

    if (m_color_map)
        m_color_map->set_min_component_percent(m_min_component_percent);
    update_range_label();
    rebuild_presets_ui();
    sync_inputs_to_requested();
    update_panel_state();

    // Bindings
    if (m_color_map) {
        m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            request_recipe_match(m_color_map->selected_color(), true, _L("Matching closest supported mix..."));
        });
    }
    if (m_hex_input) {
        m_hex_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { apply_hex_input(true); });
        m_hex_input->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
            apply_hex_input(false);
            evt.Skip();
        });
    }
    if (m_classic_picker) {
        m_classic_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent& evt) {
            if (m_syncing_inputs)
                return;
            apply_requested_target(evt.GetColour());
        });
    }
    if (m_range_slider) {
        m_range_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            m_min_component_percent = m_range_slider ? std::clamp(m_range_slider->GetValue(), 0, 50) : m_min_component_percent;
            update_range_label();
            if (m_color_map)
                m_color_map->set_min_component_percent(m_min_component_percent);
            rebuild_presets_ui();
            request_recipe_match(m_requested_target, true, _L("Matching closest supported mix..."));
        });
    }

    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_selected_recipe(); }, m_recipe_timer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (m_loading_gauge && m_recipe_loading)
            m_loading_gauge->Pulse();
    }, m_loading_timer.GetId());
}

MixedColorMatchPanel::~MixedColorMatchPanel()
{
    m_destroyed->store(true);
    if (m_recipe_timer.IsRunning())
        m_recipe_timer.Stop();
    if (m_loading_timer.IsRunning())
        m_loading_timer.Stop();
}

void MixedColorMatchPanel::begin_initial_recipe_load()
{
    request_recipe_match(m_requested_target, false, _L("Calculating closest supported mix..."));
}

void MixedColorMatchPanel::set_target_color(const wxColour &target)
{
    if (!target.IsOk()) return;
    m_requested_target = target;
    sync_inputs_to_requested();
    request_recipe_match(target, true, _L("Calculating closest supported mix..."));
}

void MixedColorMatchPanel::set_min_component_percent(int pct)
{
    pct = std::clamp(pct, 0, 50);
    if (pct == m_min_component_percent) return;
    m_min_component_percent = pct;
    if (m_range_slider) m_range_slider->SetValue(pct);
    update_range_label();
    if (m_color_map)
        m_color_map->set_min_component_percent(m_min_component_percent);
    rebuild_presets_ui();
    request_recipe_match(m_requested_target, true, _L("Matching closest supported mix..."));
}

void MixedColorMatchPanel::sync_recipe_preview(MixedColorMatchRecipeResult &recipe, const wxColour *requested)
{
    if (!recipe.valid)
        return;
    // Rebuild context each time so nozzle diameters and print settings stay current.
    m_display_context = build_mixed_filament_display_context(m_physical_colors);
    recipe.preview_color = compute_color_match_recipe_display_color(recipe, m_display_context);
    if (requested && requested->IsOk())
        recipe.delta_e = color_delta_e00(*requested, recipe.preview_color);
}

void MixedColorMatchPanel::update_range_label()
{
    if (m_range_value)
        m_range_value->SetLabel(wxString::Format(_L("%d%%"), m_min_component_percent));
}

void MixedColorMatchPanel::rebuild_presets_ui()
{
    if (!m_presets_host || !m_presets_sizer || !m_presets_label)
        return;

    m_presets = build_color_match_presets(m_physical_colors, m_min_component_percent);
    for (MixedColorMatchRecipeResult &preset : m_presets)
        sync_recipe_preview(preset);

    m_presets_host->Freeze();
    while (m_presets_sizer->GetItemCount() > 0) {
        wxSizerItem *item = m_presets_sizer->GetItem(size_t(0));
        wxWindow *win = item ? item->GetWindow() : nullptr;
        m_presets_sizer->Remove(0);
        if (win) win->Destroy();
    }

    for (const MixedColorMatchRecipeResult &preset : m_presets) {
        auto *btn = new wxBitmapButton(m_presets_host, wxID_ANY,
                                       make_color_match_swatch_bitmap(preset.preview_color, wxSize(FromDIP(28), FromDIP(18))),
                                       wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        const wxString tip = from_u8(summarize_color_match_recipe(preset)) + "\n" +
            normalize_color_match_hex(preset.preview_color.GetAsString(wxC2S_HTML_SYNTAX));
        btn->SetToolTip(tip);
        btn->Bind(wxEVT_BUTTON, [this, preset](wxCommandEvent &) { apply_preset(preset); });
        m_presets_sizer->Add(btn, 0, wxALL, FromDIP(2));
    }

    m_presets_host->FitInside();
    const bool show = !m_presets.empty();
    m_presets_label->Show(show);
    m_presets_host->Show(show);
    m_presets_host->Thaw();
}

void MixedColorMatchPanel::set_recipe_loading(bool loading, const wxString &message)
{
    m_recipe_loading = loading;
    if (!message.empty())
        m_loading_message = message;

    if (m_loading_label)
        m_loading_label->SetLabel(loading ? m_loading_message : wxString(" "));
    if (m_loading_gauge) {
        if (loading) {
            m_loading_gauge->Enable(true);
            m_loading_gauge->Pulse();
            if (!m_loading_timer.IsRunning())
                m_loading_timer.Start(100);
        } else {
            if (m_loading_timer.IsRunning())
                m_loading_timer.Stop();
            m_loading_gauge->SetValue(0);
            m_loading_gauge->Enable(false);
        }
    }
}

void MixedColorMatchPanel::sync_inputs_to_requested()
{
    m_syncing_inputs = true;
    if (m_hex_input)
        m_hex_input->ChangeValue(normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)));
    if (m_classic_picker)
        m_classic_picker->SetColour(m_requested_target);
    m_syncing_inputs = false;
}

bool MixedColorMatchPanel::apply_requested_target(const wxColour &target)
{
    request_recipe_match(target, false, _L("Matching closest supported mix..."));
    return true;
}

bool MixedColorMatchPanel::apply_hex_input(bool show_invalid_error)
{
    if (!m_hex_input || m_syncing_inputs)
        return false;
    wxColour parsed;
    if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed)) {
        if (show_invalid_error && m_error_label)
            m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
        return false;
    }
    return apply_requested_target(parsed);
}

void MixedColorMatchPanel::request_recipe_match(const wxColour &target, bool debounce, const wxString &loading_message)
{
    m_requested_target = target;
    m_selected_target  = target;
    sync_inputs_to_requested();

    ++m_recipe_request_token;
    set_recipe_loading(true, loading_message);

    if (m_recipe_timer.IsRunning())
        m_recipe_timer.Stop();
    m_recipe_refresh_pending = debounce;
    update_panel_state();

    if (debounce) {
        m_recipe_timer.StartOnce(120);
        return;
    }
    launch_recipe_match(m_recipe_request_token, target);
}

void MixedColorMatchPanel::refresh_selected_recipe()
{
    m_recipe_refresh_pending = false;
    launch_recipe_match(m_recipe_request_token, m_requested_target);
}

void MixedColorMatchPanel::launch_recipe_match(size_t request_token, const wxColour &target)
{
    const std::vector<std::string> physical_colors = m_physical_colors;
    const int min_pct = m_min_component_percent;
    auto destroyed = m_destroyed;
    std::thread([this, destroyed, physical_colors, target, request_token, min_pct]() {
        MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(physical_colors, target, min_pct);
        wxGetApp().CallAfter([this, destroyed, target, recipe = std::move(recipe), request_token]() mutable {
            if (destroyed->load()) return;
            handle_recipe_result(request_token, target, std::move(recipe));
        });
    }).detach();
}

void MixedColorMatchPanel::handle_recipe_result(size_t request_token, const wxColour &requested,
                                                 MixedColorMatchRecipeResult recipe)
{
    if (request_token != m_recipe_request_token)
        return;

    m_has_recipe_result = true;
    m_selected_recipe   = std::move(recipe);
    sync_recipe_preview(m_selected_recipe, &requested);
    set_recipe_loading(false, wxEmptyString);

    if (m_selected_recipe.valid) {
        m_selected_target = m_selected_recipe.preview_color;
        if (m_color_map)
            m_color_map->set_normalized_weights(
                expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
        sync_inputs_to_requested();
    } else {
        m_selected_target = requested;
    }

    update_panel_state();

    wxCommandEvent evt(wxEVT_SLIDER, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

void MixedColorMatchPanel::apply_preset(MixedColorMatchRecipeResult preset)
{
    preset.delta_e = 0.0;
    sync_recipe_preview(preset);
    ++m_recipe_request_token;
    m_requested_target = preset.preview_color;
    m_selected_target  = preset.preview_color;
    m_selected_recipe  = std::move(preset);
    m_has_recipe_result = true;
    m_recipe_refresh_pending = false;
    if (m_recipe_timer.IsRunning())
        m_recipe_timer.Stop();
    set_recipe_loading(false, wxEmptyString);
    if (m_color_map)
        m_color_map->set_normalized_weights(
            expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
    sync_inputs_to_requested();
    update_panel_state();

    wxCommandEvent evt(wxEVT_SLIDER, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

void MixedColorMatchPanel::update_panel_state()
{
    const wxColour fallback("#26A69A");

    // Target color swatch
    if (m_target_color_swatch) {
        m_target_color_swatch->SetBackgroundColour(m_requested_target.IsOk() ? m_requested_target : fallback);
        m_target_color_swatch->Refresh();
    }

    const bool valid = m_selected_recipe.valid;
    const wxColour recipe_color = (valid && m_selected_recipe.preview_color.IsOk()) ?
        m_selected_recipe.preview_color :
        (m_requested_target.IsOk() ? m_requested_target : fallback);

    // Solid blend result swatch
    if (m_recipe_preview) {
        m_recipe_preview->SetBackgroundColour(recipe_color);
        m_recipe_preview->Refresh();
    }

    // Recipe formula label (top of right column)
    if (m_recipe_formula_label) {
        if (m_recipe_loading) {
            m_recipe_formula_label->SetLabel(m_loading_message);
        } else if (valid) {
            m_recipe_formula_label->SetLabel(from_u8(summarize_color_match_recipe(m_selected_recipe)));
        } else {
            m_recipe_formula_label->SetLabel(wxEmptyString);
        }
    }

    // Striped preview — show palette colors with their recipe weights
    if (m_striped_preview) {
        const std::vector<int> weights = valid
            ? expand_color_match_recipe_weights(m_selected_recipe, m_palette.size())
            : std::vector<int>(m_palette.size(), 0);
        m_striped_preview->set_colors_and_weights(m_palette, weights);
    }

    // Delta label
    if (m_delta_label) {
        if (m_recipe_loading && m_requested_target.IsOk()) {
            m_delta_label->SetLabel(wxString::Format(_L("Matching %s..."),
                normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX))));
        } else if (valid && m_requested_target.IsOk()) {
            m_delta_label->SetLabel(wxString::Format(_L("Requested %s, closest recipe delta: %.2f"),
                normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)),
                m_selected_recipe.delta_e));
        } else {
            m_delta_label->SetLabel(wxEmptyString);
        }
    }

    // Error label
    if (m_error_label) {
        if (m_recipe_loading) {
            m_error_label->SetLabel(wxEmptyString);
        } else if (!valid && m_has_recipe_result) {
            m_error_label->SetLabel(_L("Unable to create a color mix from the current physical filament colors within the selected range."));
        } else if (m_hex_input && !m_syncing_inputs) {
            wxColour parsed;
            if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed))
                m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
            else
                m_error_label->SetLabel(wxEmptyString);
        } else {
            m_error_label->SetLabel(wxEmptyString);
        }
    }

}

}} // namespace Slic3r::GUI
