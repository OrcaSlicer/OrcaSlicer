#include "SpoolTile.hpp"

#include <algorithm>
#include <iterator>
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

#include "AmsMappingPopup.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "Widgets/Label.hpp"

#include "slic3r/Utils/SpoolProfileMap.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int CHIP_SIZE          = 22;
constexpr int CHIPS_PER_TILE     = 5;
constexpr int RADIUS_DIVISOR     = 4;
constexpr int FULLY_OPAQUE       = 255;
constexpr int ALPHA_OFFSET       = 7; // in "#RRGGBBAA"
constexpr int ITEM_GAP           = 4;
constexpr int MAPPING_CHIP_WIDTH = 65; // MATERIAL_ITEM_SIZE
constexpr int SEGMENTED_COLORS   = 1;  // MaterialItem color type: bands side by side, as the chips draw them

const wxColour EMPTY_CHIP_COLOR(206, 206, 206);

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

Label* make_tile_label(wxWindow* parent, const wxFont& font, const wxString& text)
{
    Label* label = new Label(parent, font, text);
    label->Wrap(parent->FromDIP(SPOOL_TILE_WIDTH));
    return label;
}

wxColour variant_part_color(const std::string& color)
{
    const int alpha = color.size() > ALPHA_OFFSET ? channel(color, ALPHA_OFFSET) : FULLY_OPAQUE;
    return wxColour(channel(color, 1), channel(color, 3), channel(color, 5), alpha);
}

std::vector<wxColour> variant_part_colors(const std::string& variant)
{
    const std::vector<std::string> colors = spool_variant_colors(variant);
    if (colors.empty())
        return {EMPTY_CHIP_COLOR};
    std::vector<wxColour> parts;
    std::transform(colors.begin(), colors.end(), std::back_inserter(parts), variant_part_color);
    return parts;
}

wxWindow* make_mapping_chip(wxWindow* parent, const std::string& variant, const wxString& material, size_t number)
{
    const std::vector<wxColour> parts = variant_part_colors(variant);
    MaterialItem*               chip  = new MaterialItem(parent, parts.front(), material);
    chip->set_material_cols(SEGMENTED_COLORS, parts);
    // The lower half names the printer slot the filament goes to; a link has none, so it numbers
    // the colors. Left empty it draws the "not mapped" warning.
    chip->set_ams_info(parts.front(), wxString::Format("%zu", number), SEGMENTED_COLORS, parts);
    return chip;
}

wxWindow* make_look_item(wxWindow* parent, SpoolLook look, const std::string& variant, const wxString& material, size_t number)
{
    if (look == SpoolLook::MappingChip)
        return make_mapping_chip(parent, variant, material, number);
    return new ColorSwatch(parent, variant, wxSize(parent->FromDIP(CHIP_SIZE), parent->FromDIP(CHIP_SIZE)));
}

size_t items_per_tile(SpoolLook look)
{
    if (look == SpoolLook::MappingChip)
        return SPOOL_TILE_WIDTH / (MAPPING_CHIP_WIDTH + ITEM_GAP);
    return CHIPS_PER_TILE;
}

wxSizer* make_chip_line(wxWindow* parent, const std::vector<std::string>& variants, SpoolLook look, const wxString& material)
{
    wxBoxSizer* line = new wxBoxSizer(wxHORIZONTAL);
    if (variants.empty())
        line->Add(make_look_item(parent, look, std::string(), material, 1), 0, wxRIGHT, parent->FromDIP(ITEM_GAP));

    const size_t shown = std::min(variants.size(), items_per_tile(look));
    for (size_t index = 0; index < shown; ++index)
        line->Add(make_look_item(parent, look, variants[index], material, index + 1), 0, wxRIGHT, parent->FromDIP(ITEM_GAP));
    if (variants.size() > shown)
        // TRN: %1% is how many more colors of a filament there are than the tile has room to draw.
        line->Add(new Label(parent, format_wxstr(_L("+%1%"), variants.size() - shown)), 0, wxALIGN_CENTER_VERTICAL);
    return line;
}

void bind_click_everywhere(wxWindow* window, const std::function<void()>& handler)
{
    window->SetCursor(wxCursor(wxCURSOR_HAND));
    window->Bind(wxEVT_LEFT_DOWN, [handler](wxMouseEvent&) { handler(); });
    for (wxWindow* child : window->GetChildren())
        bind_click_everywhere(child, handler);
}

} // namespace

ColorSwatch::ColorSwatch(wxWindow* parent, const std::string& variant, const wxSize& size)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, size)
    , m_colors(spool_variant_colors(variant))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &ColorSwatch::on_paint, this);
}

void ColorSwatch::on_paint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour        under = GetParent()->GetBackgroundColour();
    dc.SetBackground(wxBrush(under));
    dc.Clear();
    const wxSize size = GetSize();
    const int    box  = std::min(size.x, size.y);
    dc.SetPen(wxPen(SPOOL_LINE_COLOR));
    if (m_colors.empty()) {
        dc.SetBrush(wxBrush(EMPTY_CHIP_COLOR));
        dc.DrawRoundedRectangle(0, 0, size.x, box, box / RADIUS_DIVISOR);
    } else {
        draw_bands(dc, size.x, box, under);
    }
    if (m_marked) {
        dc.SetPen(wxPen(SPOOL_LINE_COLOR, 2));
        dc.DrawLine(0, size.y - 1, size.x, size.y - 1);
    }
}

// Each color paints the whole chip from its own band to the end, so the one after it takes over
// the rest and the chip keeps the rounded outline of a plain one.
void ColorSwatch::draw_bands(wxDC& dc, int width, int box, const wxColour& under) const
{
    const int count = static_cast<int>(m_colors.size());
    for (int band = 0; band < count; ++band) {
        dc.SetBrush(wxBrush(chip_color(m_colors[band], under)));
        wxDCClipper clip(dc, wxRect(width * band / count, 0, width, box));
        dc.DrawRoundedRectangle(0, 0, width, box, box / RADIUS_DIVISOR);
    }
}

SpoolTile::SpoolTile(wxWindow*                       parent,
                     const std::vector<std::string>& variants,
                     const wxString&                 title,
                     const wxString&                 subtitle,
                     SpoolLook                       look,
                     const wxString&                 material)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    wxBoxSizer* column = new wxBoxSizer(wxVERTICAL);
    // Always the full width, so tiles in separate grids still line up in columns.
    column->SetMinSize(wxSize(FromDIP(SPOOL_TILE_WIDTH), -1));
    column->Add(make_chip_line(this, variants, look, material), 0, wxBOTTOM, FromDIP(ITEM_GAP));
    column->Add(make_tile_label(this, Label::Body_14, title), 0, wxEXPAND);
    if (!subtitle.empty())
        column->Add(make_tile_label(this, Label::Body_13, subtitle), 0, wxEXPAND);
    SetSizerAndFit(column);
}

void SpoolTile::on_click(std::function<void()> handler)
{
    bind_click_everywhere(this, handler);
}

}} // namespace Slic3r::GUI
