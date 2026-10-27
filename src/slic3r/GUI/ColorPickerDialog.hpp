#pragma once

#include "libslic3r/Color.hpp"
#include "Widgets/WebViewHostDialog.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <variant>
#include <nlohmann/json_fwd.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wx/gdicmn.h>

class wxWindow;

namespace Slic3r::GUI {

// Shared storage limit; individual picker callers may request a lower editing limit.
inline constexpr std::size_t COLOR_PICKER_MAX_GRADIENT_COLORS = 7;

// Gradient colors retain their order (2..COLOR_PICKER_MAX_GRADIENT_COLORS); a solid has exactly one color.
using ColorGradient = std::vector<ColorRGBA>;
using ColorSelection = std::variant<ColorRGBA, ColorGradient>;

struct ColorPickerOptions {
    bool allow_gradient = false;
    bool allow_alpha = false;
    std::size_t max_gradient_colors = COLOR_PICKER_MAX_GRADIENT_COLORS;

    constexpr std::size_t gradient_color_limit() const
    {
        return std::clamp(max_gradient_colors, std::size_t{2}, COLOR_PICKER_MAX_GRADIENT_COLORS);
    }

    constexpr bool supports_gradient(std::size_t count) const
    {
        return allow_gradient && count >= 2 && count <= gradient_color_limit();
    }
};

// Project storage preserves direction; FilamentColor's set is only for the
// official palette and cannot represent ordered custom gradients.
struct FilamentColorPickerValue {
    std::vector<std::string> colors;
    bool gradient = false;
    std::string primary;
};

std::optional<ColorSelection> filament_color_picker_selection(const FilamentColorPickerValue& value);
ColorSelection filament_color_picker_initial(const FilamentColorPickerValue& value);
FilamentColorPickerValue filament_color_picker_result(const ColorSelection& selection);
bool filament_color_picker_changed(const ColorSelection& initial, const std::optional<ColorSelection>& result);

class ColorPickerDialog : public WebViewHostDialog
{
public:
    ColorPickerDialog(wxWindow* parent, const ColorSelection& initial, ColorPickerOptions options = {},
                      bool preserve_multi_color = false, wxWindow* anchor = nullptr);
    ~ColorPickerDialog() override;

    const std::optional<ColorSelection>& selection() const { return m_selection; }

private:
    void add_user_scripts() override;
    void on_script_message(const nlohmann::json& payload) override;
    void handle_web_command(const nlohmann::json& payload);
    void send_initial_state();
    void finish(int return_code);
    void sample_screen(); // Native screen eyedropper (ColorSampler); dialog only offers it where one is available
    void position_panel();
    void move_to_anchor();
    void resize_to_content(int height);
    void focus_webview();
    void repaint_webview();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

    ColorSelection m_initial;
    ColorPickerOptions m_options;
    std::optional<ColorSelection> m_selection;
    std::vector<ColorSelection> m_favorites;
    std::string m_page_id;
    bool m_preserve_multi_color = false;
    bool m_init_sent = false;
    bool m_page_ready = false;
    bool m_closing = false;
#if defined(__WXOSX__) || defined(__WXMSW__) || defined(__WXGTK__)
    bool m_sampling = false; // A screen sampler is open; only platforms with one use this.
#endif
    bool m_favorites_writable = false;
    bool m_sidebar_open = false;
    std::string m_palette;
    bool m_initial_positioned = false;
    wxRect m_anchor_rect;
    wxRect m_work_area;
    int m_content_height = 360;
    int m_content_width = 330;
    bool m_positioning = false;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace Slic3r::GUI
