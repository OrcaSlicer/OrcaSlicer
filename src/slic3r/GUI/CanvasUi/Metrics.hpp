#pragma once

#include <cmath>

#include "Corners.hpp"
#include "Tokens.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Sizes of the toolkit in screen pixels: the logical sizes of the design rules times the scale of the
// interface. Heights of rows and controls and the vertical gaps between them are whole pixels: ImGui floors
// the cursor after each item, so a fractional height would leave the drawn stack shorter than the measured
// one, and stacked rows keep their lines sharp.
struct Metrics
{
    explicit Metrics(float scale = 1.f) : scale(scale) {}

    float scale;

    // The steps are added before scaling, so that a height made of a line and its padding rounds once.
    template<class... Steps> float px(Steps... steps) const { return (steps.logical() + ...) * scale; }
    template<class... Steps> float whole_px(Steps... steps) const { return std::round((steps.logical() + ...) * scale); }

    float card_radius{px(Radius::lg)};
    float card_padding{px(space<12>())};
    float card_gap{px(space<8>())};
    float scrollbar{px(space<6>())};
    // The shadow that settings opened over content cast down on it, and how much narrower it is.
    float shadow_below_height{px(space<8>())};
    // Outlines and dividers stay one physical pixel whatever the scale.
    float outline{1.f};

    float control_radius{px(Radius::sm)};
    float control_height{whole_px(TextRole::body.leading, space<4>())};
    float control_padding{px(space<8>())};
    // Each side of a button's label.
    float button_padding{px(space<12>())};
    // A small button, as the reset in a row of tabs.
    float small_button{whole_px(TextRole::body.leading)};
    float small_button_padding{px(space<6>())};
    float small_button_radius{px(Radius::sm)};
    float small_button_icon{px(icon_size<12>())};
    float button_icon_gap{px(space<4>())};
    float focus_ring{px(Stroke::md)};
    // Between components stacked in a card.
    float item_gap{whole_px(space<4>())};
    // Between components side by side in a row, and from a row to the card's edge.
    float inline_gap{px(space<4>())};

    float toggle_gap{whole_px(space<2>())};
    float toggle_radius{px(Radius::md)};
    // Each side of a segment's label.
    float toggle_padding{px(space<8>())};
    // How far the shadow of the selected segment drops under it.
    float toggle_pressed_drop{px(space<1>())};
    float field_height{whole_px(TextRole::body.leading, space<8>())};
    float field_gap{px(space<12>())};
    float field_label_top{px(space<2>())};
    float field_label_gap{whole_px(space<6>())};
    float field_stack_bottom{whole_px(space<4>())};
    // Folder tabs on the bottom edge of a row.
    float tabs_list_height{whole_px(TextRole::body.leading, space<12>())};
    float tab_height{whole_px(TextRole::body.leading, space<6>())};
    float tab_padding{px(space<12>())};
    float tab_gap{px(space<2>())};
    float tab_radius{px(Radius::md)};
    // From the ends of a tabs bar to its tabs and to its reset button.
    float tabs_bar_left{px(space<4>())};
    float tabs_bar_right{px(space<6>())};
    float switch_width{px(space<28>())};
    float switch_height{px(space<16>())};
    float switch_text_padding{whole_px(space<4>())};
    float switch_thumb_inset{px(space<2>())};
    float switch_thumb_drop{px(space<1>())};

    float icon_button{px(space<32>())};
    float icon_button_radius{px(Radius::md)};
    float button_icon{px(icon_size<18>())};
    float icon_button_gap{px(space<2>())};
    float menu_indicator{px(space<6>())};
    float menu_indicator_inset{px(space<4>())};

    float menu_radius{px(Radius::lg)};
    float menu_padding_x{px(space<4>())};
    float menu_padding_y{px(space<6>())};
    float menu_row{whole_px(TextRole::body.leading, space<8>())};
    float menu_row_radius{nested_radius(menu_radius, menu_padding_x)};
    float menu_mark_center{px(space<16>())};
    float menu_text_left{px(space<32>())};
    float menu_key_gap{px(space<24>())};
    float menu_key_right{px(space<10>())};
    // From the edge of a row to a group label.
    float menu_label_inset{px(space<10>())};
    // From the edge of a row to its text, on the right and, when no item has a mark, on the left.
    float menu_text_inset{px(space<14>())};
    float menu_group_label{whole_px(TextRole::body.leading, space<4>())};
    // From the bottom of a group label to the bottom of its text.
    float group_label_baseline{px(space<2>())};
    float menu_icon{px(icon_size<16>())};
    float radio_outer{px(space<6>())};
    float radio_inner{.5f * px(space<6>())};
    float radio_stroke{px(Stroke::md)};
    float menu_separator{whole_px(space<8>())};
    float popup_side_offset{px(space<8>())};

    float swatch_radius{px(Radius::sm)};

    // A panel of a column over the scene: its width, the height of its header and the side of the header's buttons.
    float panel_width{std::round(px(space<300>()))};
    float panel_header{whole_px(TextRole::title.leading, space<12>())};
    float panel_header_button{px(space<24>())};
    // Above the first component of a panel's body and under its last, and between the body's sections.
    float panel_body_padding{whole_px(space<4>())};
    float panel_section_gap{whole_px(space<8>())};
    // A line of caption text, as under a scale or a table.
    float caption_line{whole_px(TextRole::caption.leading)};

    float popover_padding{px(space<12>())};
    float popover_title{whole_px(TextRole::title.leading)};
    float popover_radius{px(Radius::lg)};
    float popover_title_gap{whole_px(space<8>())};

    float collapsible_content_top{px(space<4>())};
    float collapsible_content_bottom{px(space<6>())};

    float table_row{whole_px(space<24>())};
    float table_row_radius{px(Radius::sm)};
    float table_padding_left{px(space<10>())};
    float table_padding_right{px(space<6>())};
    float table_swatch{px(space<10>())};
    float table_swatch_numbered{px(space<16>())};
    float table_swatch_gap{px(space<8>())};
    float table_cell_gap{px(space<8>())};

    float alert_padding{px(space<10>())};
    float alert_radius{px(Radius::md)};
    float alert_icon{px(icon_size<16>())};
    float alert_icon_gap{px(space<8>())};
    float alert_line_gap{px(space<2>())};
    float alert_actions_gap{px(space<8>())};
    float alert_action_gap{px(space<8>())};

    float tooltip_text{px(TextRole::label.size)};
    float tooltip_max_width{px(space<260>())};
    float tooltip_padding_x{px(space<12>())};
    float tooltip_padding_y{px(space<6>())};
    float tooltip_radius{px(Radius::md)};
    float tooltip_key_gap{px(space<8>())};
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
