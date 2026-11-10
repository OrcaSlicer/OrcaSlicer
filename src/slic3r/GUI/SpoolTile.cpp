#include "SpoolTile.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <wx/brush.h>
#include <wx/cursor.h>
#include <wx/dc.h>
#include <wx/dcbuffer.h>
#include <wx/event.h>
#include <wx/pen.h>
#include <wx/sizer.h>

#include "I18N.hpp"
#include "format.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include "slic3r/Utils/SpoolProfileMap.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int FULLY_OPAQUE   = 255;
constexpr int ALPHA_OFFSET   = 7; // in "#RRGGBBAA"
constexpr int ITEM_GAP       = 4;

constexpr int SPOOL_PICTURE_HEIGHT = 80;
// The spool picture's view box, and where the filament shows between its flanges, in the same
// units. The fill reaches under the flanges; the picture is drawn over it and hides that part.
constexpr double SPOOL_VIEW_WIDTH     = 53.0;
constexpr double SPOOL_VIEW_HEIGHT    = 76.0;
constexpr double FILAMENT_LEFT        = 14.5;
constexpr double FILAMENT_RIGHT       = 43.5;
constexpr double FILAMENT_TOP         = 9.0;
constexpr double FILAMENT_BOTTOM      = 67.3;
// The squares stay clear of the flanges, so they are drawn whole over the picture.
constexpr double SQUARES_LEFT   = 18.0;
constexpr double SQUARES_RIGHT  = 40.0;
constexpr double SQUARES_TOP    = 12.0;
constexpr double SQUARES_BOTTOM = 64.0;
constexpr int    SQUARE_COLUMNS = 2;
constexpr int    SQUARE_ROWS    = 3;
constexpr int    SQUARE_GAP     = 3;
constexpr int    SQUARE_MAX     = 12;
constexpr int    SQUARE_RADIUS  = 2;

constexpr int VARIANT_COLOR_HEIGHT = 18;
constexpr int VARIANT_MIN_WIDTH    = 65; // MATERIAL_ITEM_SIZE, the width of the send-print mapping chip
constexpr int VARIANT_PADDING      = 6;
constexpr int VARIANT_RADIUS       = 4;
constexpr int FORGET_ICON_SIZE     = 14;

const wxColour EMPTY_CHIP_COLOR(206, 206, 206);
const wxColour CHIP_TEXT_COLOR(0x26, 0x2E, 0x30);

int channel(const std::string& color, size_t offset)
{
    return std::stoi(color.substr(offset, 2), nullptr, 16);
}

int blend_channel(int over, int under, int alpha)
{
    return (over * alpha + under * (FULLY_OPAQUE - alpha)) / FULLY_OPAQUE;
}

// A translucent color is drawn as it would look over the panel behind it.
wxColour chip_color(const std::string& color, const wxColour& under)
{
    const int alpha = color.size() > ALPHA_OFFSET ? channel(color, ALPHA_OFFSET) : FULLY_OPAQUE;
    return wxColour(blend_channel(channel(color, 1), under.Red(), alpha), blend_channel(channel(color, 3), under.Green(), alpha),
                    blend_channel(channel(color, 5), under.Blue(), alpha));
}

// Each color paints the whole shape from its own band to the end, so the one after it takes over
// the rest and the shape keeps the rounded outline of a plain one. No color at all is the gray of a
// color nobody has reported.
void fill_colors(wxDC& dc, const std::vector<std::string>& colors, const wxRect& area, int radius, const wxColour& under)
{
    if (colors.empty()) {
        dc.SetBrush(wxBrush(EMPTY_CHIP_COLOR));
        dc.DrawRoundedRectangle(area, radius);
        return;
    }
    const int count = static_cast<int>(colors.size());
    for (int band = 0; band < count; ++band) {
        dc.SetBrush(wxBrush(chip_color(colors[band], under)));
        wxDCClipper clip(dc, wxRect(area.x + area.width * band / count, area.y, area.width, area.height));
        dc.DrawRoundedRectangle(area, radius);
    }
}

wxRect view_box_rect(const wxSize& size, double left, double top, double right, double bottom)
{
    const double scale_x = size.x / SPOOL_VIEW_WIDTH;
    const double scale_y = size.y / SPOOL_VIEW_HEIGHT;
    return wxRect(wxPoint(int(left * scale_x), int(top * scale_y)), wxPoint(int(right * scale_x), int(bottom * scale_y)));
}

Label* make_tile_label(wxWindow* parent, const wxFont& font, const wxString& text)
{
    Label* label = new Label(parent, font, text, wxALIGN_CENTER_HORIZONTAL);
    label->Wrap(parent->FromDIP(SPOOL_TILE_WIDTH));
    return label;
}

void bind_click_everywhere(wxWindow* window, const std::function<void()>& handler)
{
    window->SetCursor(wxCursor(wxCURSOR_HAND));
    window->Bind(wxEVT_LEFT_DOWN, [handler](wxMouseEvent&) { handler(); });
    for (wxWindow* child : window->GetChildren())
        bind_click_everywhere(child, handler);
}

} // namespace

SpoolPicture::SpoolPicture(wxWindow* parent, const std::vector<std::string>& variants)
    : wxWindow(parent, wxID_ANY)
    , m_variants(variants)
    , m_spool(this, "extra_ams_tray_mid", SPOOL_PICTURE_HEIGHT)
    , m_spool_hover(this, "extra_ams_tray_mid_hover", SPOOL_PICTURE_HEIGHT)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(m_spool.GetBmpSize());
    SetSize(m_spool.GetBmpSize());
    Bind(wxEVT_PAINT, &SpoolPicture::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { set_hover(true); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { set_hover(false); });
}

void SpoolPicture::set_hover(bool hover)
{
    m_hover = hover;
    Refresh();
}

void SpoolPicture::on_paint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour        under = GetParent()->GetBackgroundColour();
    dc.SetBackground(wxBrush(under));
    dc.Clear();
    const wxSize size = GetSize();
    dc.SetPen(*wxTRANSPARENT_PEN);
    if (m_variants.size() == 1)
        fill_colors(dc, spool_variant_colors(m_variants.front()),
                    view_box_rect(size, FILAMENT_LEFT, FILAMENT_TOP, FILAMENT_RIGHT, FILAMENT_BOTTOM), 0, under);
    dc.DrawBitmap((m_hover ? m_spool_hover : m_spool).bmp(), 0, 0, true);
    if (m_variants.size() > 1)
        draw_color_squares(dc, under);
}

// Every square while they fit; past that the last one gives way to how many more there are.
void SpoolPicture::draw_color_squares(wxDC& dc, const wxColour& under) const
{
    const wxRect area  = view_box_rect(GetSize(), SQUARES_LEFT, SQUARES_TOP, SQUARES_RIGHT, SQUARES_BOTTOM);
    const int    gap   = FromDIP(SQUARE_GAP);
    const int    side  = std::min({(area.width - gap) / SQUARE_COLUMNS, (area.height - 2 * gap) / SQUARE_ROWS, FromDIP(SQUARE_MAX)});
    const int    left  = area.x + (area.width - SQUARE_COLUMNS * side - gap) / 2;
    const int    top   = area.y + (area.height - SQUARE_ROWS * side - 2 * gap) / 2;
    const size_t cells = SQUARE_COLUMNS * SQUARE_ROWS;
    const size_t shown = m_variants.size() <= cells ? m_variants.size() : cells - 1;

    dc.SetPen(wxPen(SPOOL_LINE_COLOR));
    for (size_t index = 0; index < shown; ++index) {
        const wxRect square(left + int(index % SQUARE_COLUMNS) * (side + gap), top + int(index / SQUARE_COLUMNS) * (side + gap), side, side);
        fill_colors(dc, spool_variant_colors(m_variants[index]), square, FromDIP(SQUARE_RADIUS), under);
    }
    if (shown == m_variants.size())
        return;

    const wxRect more(left + int(shown % SQUARE_COLUMNS) * (side + gap), top + int(shown / SQUARE_COLUMNS) * (side + gap), side, side);
    dc.SetFont(Label::Body_10);
    dc.SetTextForeground(SPOOL_LINE_COLOR);
    // TRN: %1% is how many more colors of a filament there are than the spool has room to draw.
    dc.DrawLabel(format_wxstr(_L("+%1%"), m_variants.size() - shown), more, wxALIGN_CENTER);
}

VariantChip::VariantChip(wxWindow* parent, const std::string& variant, const wxString& profile, bool own_profile, bool can_forget)
    : wxWindow(parent, wxID_ANY)
    , m_colors(spool_variant_colors(variant))
    , m_profile(profile)
    , m_can_forget(can_forget)
    , m_forget_icon(this, "delete", FORGET_ICON_SIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFont(own_profile ? Label::Head_12 : Label::Body_12);
    const wxSize text    = GetTextExtent(profile);
    const int    padding = FromDIP(VARIANT_PADDING);
    const int    icon    = can_forget ? m_forget_icon.GetBmpWidth() + padding : 0;
    const wxSize size(std::max(text.x + icon + 2 * padding, FromDIP(VARIANT_MIN_WIDTH)), FromDIP(VARIANT_COLOR_HEIGHT) + text.y + 2 * padding);
    SetMinSize(size);
    SetSize(size);
    Bind(wxEVT_PAINT, &VariantChip::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &VariantChip::on_left_down, this);
}

wxRect VariantChip::profile_area() const
{
    const int color_height = FromDIP(VARIANT_COLOR_HEIGHT);
    return wxRect(0, color_height, GetSize().x, GetSize().y - color_height);
}

void VariantChip::on_pick(std::function<void()> handler)
{
    m_on_pick = std::move(handler);
    SetCursor(wxCursor(wxCURSOR_HAND));
}

void VariantChip::on_forget(std::function<void()> handler)
{
    m_on_forget = std::move(handler);
}

wxRect VariantChip::forget_icon_rect() const
{
    const wxRect area = profile_area();
    const wxSize icon = m_forget_icon.GetBmpSize();
    return wxRect(wxPoint(area.GetRight() - FromDIP(VARIANT_PADDING) - icon.x, area.y + (area.height - icon.y) / 2), icon);
}

void VariantChip::on_left_down(wxMouseEvent& event)
{
    if (m_can_forget && m_on_forget && forget_icon_rect().Contains(event.GetPosition())) {
        m_on_forget();
        return;
    }
    if (m_on_pick)
        m_on_pick();
}

// Painted in layers so only the outer corners are round: the color is drawn a radius taller than
// its band, and the lower part is painted back over that overlap.
void VariantChip::on_paint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour        under = GetParent()->GetBackgroundColour();
    dc.SetBackground(wxBrush(under));
    dc.Clear();
    const wxSize size         = GetSize();
    const int    radius       = FromDIP(VARIANT_RADIUS);
    const int    color_height = FromDIP(VARIANT_COLOR_HEIGHT);

    dc.SetPen(*wxTRANSPARENT_PEN);
    fill_colors(dc, m_colors, wxRect(0, 0, size.x, color_height + radius), radius, under);
    dc.SetBrush(wxBrush(under));
    dc.DrawRectangle(0, color_height, size.x, radius);

    dc.SetPen(wxPen(SPOOL_LINE_COLOR));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
    dc.DrawLine(0, color_height, size.x, color_height);

    dc.SetFont(GetFont());
    dc.SetTextForeground(StateColor::darkModeColorFor(CHIP_TEXT_COLOR));
    wxRect text_area = profile_area();
    if (m_can_forget) {
        const wxRect icon = forget_icon_rect();
        dc.DrawBitmap(m_forget_icon.bmp(), icon.GetPosition(), true);
        text_area.SetRight(icon.x - 1);
    }
    dc.DrawLabel(m_profile, text_area, wxALIGN_CENTER);
}

SpoolTile::SpoolTile(wxWindow* parent, const std::vector<std::string>& variants, const wxString& title)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    wxBoxSizer* column = new wxBoxSizer(wxVERTICAL);
    // Always the full width, so tiles in separate grids still line up in columns.
    column->SetMinSize(wxSize(FromDIP(SPOOL_TILE_WIDTH), -1));
    column->Add(new SpoolPicture(this, variants), 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(ITEM_GAP));
    column->Add(make_tile_label(this, Label::Body_13, title), 0, wxALIGN_CENTER_HORIZONTAL);
    SetSizerAndFit(column);
}

void SpoolTile::on_click(std::function<void()> handler)
{
    bind_click_everywhere(this, handler);
}

}} // namespace Slic3r::GUI
