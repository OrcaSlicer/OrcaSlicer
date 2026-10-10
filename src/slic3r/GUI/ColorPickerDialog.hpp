#pragma once

#include "libslic3r/Color.hpp"
#include "Widgets/WebViewHostDialog.hpp"

#include <array>
#include <variant>
#include <nlohmann/json_fwd.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wx/gdicmn.h>
#include <wx/bitmap.h>

class wxWindow;

namespace Slic3r::GUI {

// Gradient endpoints retain their order; a solid has exactly one color.
using ColorSelection = std::variant<ColorRGBA, std::array<ColorRGBA, 2>>;

struct ColorPickerOptions {
    bool allow_gradient = false;
    bool allow_alpha = false;
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
    void apply_rounded_shape();
    void position_panel();
    void move_to_anchor();
    void resize_to_content(int height);
    void focus_webview();
    void repaint_webview();
    void on_dpi_changed(const wxRect& suggested_rect) override;

    ColorSelection m_initial;
    ColorPickerOptions m_options;
    std::optional<ColorSelection> m_selection;
    std::vector<ColorSelection> m_favorites;
    std::string m_page_id;
    bool m_preserve_multi_color = false;
    bool m_init_sent = false;
    bool m_page_ready = false;
    bool m_closing = false;
    bool m_favorites_writable = false;
    int m_corner_radius{8};
    wxBitmap m_shape_bmp;
    bool m_applying_shape{false};
    wxRect m_anchor_rect;
    wxRect m_work_area;
    int m_content_height = 520;
    bool m_positioning = false;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace Slic3r::GUI
