#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <boost/iostreams/device/mapped_file.hpp>

#include "GCodePanelText.hpp"
#include "PreviewPanelSlots.hpp"
#include "CanvasUi/PanelCard.hpp"
#include "CanvasUi/PositionField.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
class AppConfig;

namespace GUI {

namespace CanvasUi {
class Card;
struct SettingsFormOptions;
class Painter;
} // namespace CanvasUi

// The lines of the G-code file around the current move, in a card over the preview.
class GCodePanel
{
public:
    using Settings = GCodePanelSettings;

    using Row = GCodeRow;

    // The color of the move a line makes in the line type view, or nothing for a line without a move.
    std::function<std::optional<CanvasUi::Color>(uint64_t)> line_color;
    CanvasUi::Color wipe_color;
    std::function<void(uint64_t)> go_to_line;
    std::function<void(const Settings&)> save_settings;
    CanvasUi::PanelCollapse       collapse;

    GCodePanel() = default;
    ~GCodePanel() { stop_mapping_file(); }
    void load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends);
    void reset() {
        stop_mapping_file();
        m_lines_ends.clear();
        m_lines_ends.shrink_to_fit();
        m_rows.clear();
        m_filename.clear();
        m_filename.shrink_to_fit();
    }

    void render(const PreviewPanelPlacement& place, float bottom, uint64_t curr_line_id);
    void               hide() { m_last_rect = CanvasUi::Rect(); }
    const CanvasUi::Rect& last_rect() const { return m_last_rect; }
    void load_settings(const AppConfig& config);

    void stop_mapping_file();

private:
    Settings    m_settings;
    bool        m_settings_open{false};
    int         m_settings_tab{0};
    CanvasUi::PositionField m_goto;
    uint64_t    m_menu_line{0};
    CanvasUi::Point m_menu_pos;
    CanvasUi::Rect m_last_rect;
    std::string m_filename;
    boost::iostreams::mapped_file_source m_file;
    // Where each line of the file ends, so that a line is read straight from the mapping.
    std::vector<size_t> m_lines_ends;
    std::vector<Row>    m_rows;
    uint64_t            m_rows_line{0};
    size_t              m_rows_count{0};
    size_t              m_chars{0};
    size_t              m_tag_padding{0};

    struct Sizes;

    struct RowGeometry
    {
        float  left{0.f};
        float  right{0.f};
        float  number_right{0.f};
        float  stripe_x{0.f};
        float  text_left{0.f};
        size_t chars{0};
        float  row_height{0.f};
        float  char_width{0.f};
        float  rounding{0.f};
    };

    std::string read_line(uint64_t id) const;
    void        update_rows(uint64_t curr_line_id, size_t count);
    std::string copy_text(bool visible) const;
    void        render_copy_menu();
    void        open_copy_menu_at(uint64_t line, const CanvasUi::Point& pos);
    static RowGeometry row_geometry(const Sizes& s, float left, float right, float number_right, float text_right, float row_height,
                                    float char_width);
    void        render_header(const CanvasUi::PanelCard& frame, const Sizes& s, uint64_t curr_line_id, std::vector<GCodeSpan> current_spans,
                              const RowGeometry& body);
    CanvasUi::SettingsFormOptions settings_form(float width);
    void        render_body(const CanvasUi::Card& card, const Sizes& s, const RowGeometry& g, float body_top, float body_h,
                            uint64_t curr_line_id);
    struct StripeRing
    {
        CanvasUi::Rect  rect;
        CanvasUi::Color color;
    };
    // The background of a row of the body, through the card so that it follows the card's corners, and of the
    // collapsed line; each returns the colour under the text.
    CanvasUi::Color body_row_background(const CanvasUi::Card& card, const Row& row, float top, const RowGeometry& g, uint64_t curr_line_id,
                                        const Sizes& s) const;
    CanvasUi::Color collapsed_line_background(const CanvasUi::Painter& paint, float top, const RowGeometry& g, const Sizes& s) const;
    // The number, the stripe and the code of a row over its background. It returns its stripe when that blends into
    // the background, for the caller to ring together with the stripes of the same colour next to it.
    std::optional<StripeRing> render_row(const CanvasUi::Painter& paint, const Row& row, float top, const RowGeometry& g, uint64_t curr_line_id,
                                         const CanvasUi::Color& background, const Sizes& s) const;
    void        draw_tag_badge(const CanvasUi::Painter& paint, const GCodeSpan& span, float x, float top, const RowGeometry& g, const Sizes& s,
                               const CanvasUi::Color& background) const;
};

} // namespace GUI
} // namespace Slic3r
