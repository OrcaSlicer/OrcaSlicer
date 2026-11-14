#include "AssetGallery.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/control.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/window.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp" // the fallback parent for a dialog opened with none
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "format.hpp"

namespace Slic3r::GUI {

namespace {

// The palette, written as the light colours the dark map knows (StateColor.cpp's gDarkColors) and
// mapped at use time, which is what every other Orca window does: one set of literals, both themes.
const wxColour BG_COLOUR          = wxColour("#FFFFFF"); // the grid's own sheet
const wxColour TILE_COLOUR        = wxColour("#F8F8F8");
const wxColour TILE_BORDER_COLOUR = wxColour("#DBDBDB");
const wxColour SELECTED_COLOUR    = wxColour("#EBF9F0"); // the chosen tile's fill
const wxColour ACCENT_COLOUR      = wxColour("#009688");
const wxColour TEXT_COLOUR        = wxColour("#262E30");
const wxColour DIM_TEXT_COLOUR    = wxColour("#6B6B6B");
const wxColour SEPARATOR_COLOUR   = wxColour("#EEEEEE");

// Thumbnails the grid asks the library for per timer tick. A library decodes an image here, on the
// GUI thread, so the number is a budget on one tick rather than on one window: the rest fill in over
// the following ticks. The texture picker decoded its whole expanded library in a single frame and
// stalled on open for it.
constexpr int LOADS_PER_TICK = 2;

// How often the loader ticks while there is anything left to load. Short enough that a library fills
// in while the user is still looking at the first row, long enough that the event loop gets between
// two decodes.
constexpr int LOADER_INTERVAL_MS = 15;

// Which sections the user collapsed, per library, for the length of the app run. A gallery is opened
// and closed over and over, and a section the user folded away should stay folded; it is deliberately
// not in the app config, so nothing a user does here outlives the session they did it in.
std::map<std::string, bool> &collapsed_state(const std::string &library_key)
{
    static std::map<std::string, std::map<std::string, bool>> s_state;
    return s_state[library_key];
}

// Case-insensitive substring match for the search box, on the name the user can actually see.
bool matches_filter(const wxString &name, const wxString &filter)
{
    return filter.empty() || name.Lower().Find(filter.Lower()) != wxNOT_FOUND;
}

} // namespace

// ----------------------------------------------------------------------------------------------
// The grid
// ----------------------------------------------------------------------------------------------

// A drawn grid rather than a panel of child windows: a library of a few hundred assets would be a few
// hundred windows, and every one of them would want its own paint, hover and hit testing anyway.
// Everything the grid shows is laid out into `m_items` in content coordinates, and both painting and
// hit testing read that one list, so a tile can never be drawn somewhere it cannot be clicked.
class AssetGalleryGrid : public wxScrolled<wxWindow>
{
public:
    AssetGalleryGrid(wxWindow *parent, AssetLibrary &library);

    // Set by the window that owns the grid. The grid only reports; opening a file dialog or a
    // confirmation is the window's business, not a mouse handler's.
    std::function<void()>                   on_selection_changed;
    std::function<void()>                   on_activated; // a double click, or Enter on a tile
    std::function<void(const AssetEntry &)> on_remove;
    std::function<void()>                   on_add_file;

    void set_hidden_categories(std::vector<std::string> hidden);
    void set_filter(const wxString &filter);
    // Re-reads the library (sections and entries), keeps or moves the selection, and optionally
    // scrolls it into view once the grid has a width to lay out in.
    void reload(bool force_rescan, const std::string &select_id, bool reveal_selected);
    // Theme change: the colours and every drawn thumbnail (a frame's thumbnail is drawn in the
    // theme's own colours, so a cached one is wrong after a switch).
    void apply_theme();
    // DPI change: the metrics are all FromDIP, and the thumbnails were rendered for the old scale.
    void rescale();

    const std::string &selection() const { return m_selection; }
    const AssetEntry  *selected_entry() const;
    // True while any visible section takes files, which is what enables the window's Add button.
    bool accepts_files() const;

protected:
    void OnDraw(wxDC &) override {} // the grid paints itself; see on_paint

private:
    struct Item
    {
        enum class Kind { Header, Tile, AddTile, Hint, Empty };

        Kind              kind    = Kind::Tile;
        wxRect            rect; // content coordinates, i.e. unscrolled
        size_t            section = 0;
        const AssetEntry *entry   = nullptr;
    };

    struct Section
    {
        AssetCategory                   category;
        std::vector<const AssetEntry *> entries;
    };

    void update_metrics();
    void rebuild_sections();
    void layout();
    void on_paint(wxPaintEvent &event);
    void on_size(wxSizeEvent &event);
    void on_mouse_down(wxMouseEvent &event);
    void on_double_click(wxMouseEvent &event);
    void on_motion(wxMouseEvent &event);
    void on_leave(wxMouseEvent &event);
    void on_key_down(wxKeyEvent &event);
    void on_loader_tick(wxTimerEvent &event);

    void draw_header(wxGCDC &gdc, const Item &item);
    void draw_tile(wxGCDC &gdc, const Item &item);
    void draw_add_tile(wxGCDC &gdc, const Item &item);

    int  hit_test(const wxPoint &content_pos) const; // index into m_items, or -1
    // What the pointer is over, or nullptr. The index is checked against the list it indexes: a
    // relayout can shorten that list between the motion event that set it and the next paint.
    const Item *hovered_item() const;
    bool is_collapsed(const Section &section) const;
    // The remove button's hotspot on a tile, which is also where it is drawn.
    wxRect remove_rect(const wxRect &tile) const;
    void   select_entry(const AssetEntry *entry);
    void   move_selection(int dx, int dy);
    void   reveal(const std::string &id);
    void   queue_thumbnail(const AssetEntry *entry);
    wxBitmap thumbnail_for(const AssetEntry &entry);

    AssetLibrary            &m_library;
    std::vector<std::string> m_hidden;
    wxString                 m_filter;
    std::vector<Section>     m_sections;
    std::vector<Item>        m_items;
    std::string              m_selection;

    // id -> tile picture, cached whether it loaded or not: a file that turned out unreadable is
    // remembered as an invalid bitmap, so it is drawn as a placeholder once instead of being decoded
    // again on every tick.
    std::map<std::string, wxBitmap> m_thumbnails;
    std::vector<const AssetEntry *> m_pending; // visible and not cached yet, newest paint's view
    wxTimer                         m_loader;

    int  m_hover        = -1;
    bool m_hover_remove = false;
    // Set by reload(), honoured by the first layout that has a real width: the window is fitted after
    // its content exists, so the grid has no size to scroll inside while the constructor runs.
    std::string m_reveal_id;

    // Metrics, all FromDIP and re-derived on a DPI change.
    int m_margin      = 0;
    int m_spacing     = 0;
    int m_tile_w      = 0;
    int m_thumb_h     = 0;
    int m_caption_h   = 0;
    int m_tile_h      = 0;
    int m_header_h    = 0;
    int m_line_h      = 0;
    int m_scroll_unit = 0;
    int m_radius      = 0;
    int m_content_h   = 0;

    // Resolved for the current theme by apply_theme().
    wxColour m_bg, m_tile, m_tile_border, m_selected, m_accent, m_text, m_dim, m_separator;
};

AssetGalleryGrid::AssetGalleryGrid(wxWindow *parent, AssetLibrary &library)
    : wxScrolled<wxWindow>(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxFULL_REPAINT_ON_RESIZE)
    , m_library(library)
    , m_loader(this)
{
    // Every pixel of the client area is covered by the blit at the end of on_paint, so the erase
    // step is pure flicker here.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    // Taken from the parent explicitly rather than left to inheritance, because update_metrics()
    // measures text with it: the row heights have to be the dialog's font, not the system default.
    if (parent != nullptr)
        SetFont(parent->GetFont());
    apply_theme();
    update_metrics();
    // Vertical only: a horizontal rate of 0 is what actually forbids sideways scrolling, and the
    // tiles reflow to the width instead.
    SetScrollRate(0, m_scroll_unit);

    Bind(wxEVT_PAINT, &AssetGalleryGrid::on_paint, this);
    Bind(wxEVT_SIZE, &AssetGalleryGrid::on_size, this);
    Bind(wxEVT_LEFT_DOWN, &AssetGalleryGrid::on_mouse_down, this);
    Bind(wxEVT_LEFT_DCLICK, &AssetGalleryGrid::on_double_click, this);
    Bind(wxEVT_MOTION, &AssetGalleryGrid::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &AssetGalleryGrid::on_leave, this);
    Bind(wxEVT_KEY_DOWN, &AssetGalleryGrid::on_key_down, this);
    Bind(wxEVT_TIMER, &AssetGalleryGrid::on_loader_tick, this, m_loader.GetId());
}

void AssetGalleryGrid::apply_theme()
{
    m_bg          = StateColor::darkModeColorFor(BG_COLOUR);
    m_tile        = StateColor::darkModeColorFor(TILE_COLOUR);
    m_tile_border = StateColor::darkModeColorFor(TILE_BORDER_COLOUR);
    m_selected    = StateColor::darkModeColorFor(SELECTED_COLOUR);
    m_accent      = StateColor::darkModeColorFor(ACCENT_COLOUR);
    m_text        = StateColor::darkModeColorFor(TEXT_COLOUR);
    m_dim         = StateColor::darkModeColorFor(DIM_TEXT_COLOUR);
    m_separator   = StateColor::darkModeColorFor(SEPARATOR_COLOUR);
    SetBackgroundColour(m_bg);
    // A drawn thumbnail is drawn in the theme's colours, and an image thumbnail was composed on the
    // old tile colour, so none of them survive a theme switch.
    m_thumbnails.clear();
    Refresh();
}

void AssetGalleryGrid::rescale()
{
    if (GetParent() != nullptr)
        SetFont(GetParent()->GetFont()); // the dialog's font was re-derived for the new DPI
    update_metrics();
    SetScrollRate(0, m_scroll_unit);
    m_thumbnails.clear(); // rendered for the old scale factor
    layout();
    Refresh();
}

void AssetGalleryGrid::update_metrics()
{
    m_margin   = FromDIP(14);
    m_spacing  = FromDIP(10);
    m_tile_w   = FromDIP(104);
    m_thumb_h  = FromDIP(92);
    m_radius   = FromDIP(6);
    // Measured rather than guessed: the caption and the headings have to clear the dialog's font,
    // which changes with the theme and the DPI.
    m_line_h      = GetTextExtent(wxT("Wg")).y;
    m_caption_h   = m_line_h + FromDIP(8);
    m_tile_h      = m_thumb_h + m_caption_h;
    m_header_h    = m_line_h + FromDIP(14);
    m_scroll_unit = FromDIP(16);
}

void AssetGalleryGrid::set_hidden_categories(std::vector<std::string> hidden)
{
    m_hidden = std::move(hidden);
}

void AssetGalleryGrid::set_filter(const wxString &filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    rebuild_sections();
    layout();
    Scroll(-1, 0); // the old scroll position means nothing in the new, shorter list
    Refresh();
}

void AssetGalleryGrid::reload(bool force_rescan, const std::string &select_id, bool reveal_selected)
{
    m_library.entries(force_rescan);
    rebuild_sections();
    if (!select_id.empty())
        m_selection = select_id;
    // A selection whose entry is gone (the user removed it, or a rescan dropped it) is no selection.
    if (!m_selection.empty() && selected_entry() == nullptr)
        m_selection.clear();
    if (reveal_selected)
        m_reveal_id = m_selection;
    layout();
    if (!m_reveal_id.empty() && GetClientSize().x > 0) {
        reveal(m_reveal_id);
        m_reveal_id.clear();
    }
    Refresh();
}

void AssetGalleryGrid::rebuild_sections()
{
    m_sections.clear();
    // The queue holds pointers into the library's own vector, which a rescan replaces. The next paint
    // refills it from whatever is visible by then, so dropping it costs nothing and keeps the loader
    // from reading an entry that no longer exists.
    m_pending.clear();
    // The entries first: a library whose sections come from the files it found (a frame library's do)
    // can only answer categories() once it has scanned.
    const std::vector<AssetEntry>   &entries    = m_library.entries();
    const std::vector<AssetCategory> categories = m_library.categories();

    for (const AssetCategory &category : categories) {
        if (std::find(m_hidden.begin(), m_hidden.end(), category.key) != m_hidden.end())
            continue;
        Section section;
        section.category = category;
        for (const AssetEntry &entry : entries)
            if (entry.category == category.key && matches_filter(entry.name, m_filter))
                section.entries.push_back(&entry);
        // While the user is searching, a section with no hit is noise: it says nothing and pushes
        // the hits off the screen. With no search term every visible section is shown, empty or not,
        // because an empty section is where the Add tile lives.
        if (m_filter.empty() || !section.entries.empty())
            m_sections.push_back(std::move(section));
    }
}

bool AssetGalleryGrid::is_collapsed(const Section &section) const
{
    // A search shows what it found: collapsing is the user's answer to a long list, not to a short one
    // they asked for by name.
    if (!m_filter.empty())
        return false;
    std::map<std::string, bool> &state = collapsed_state(m_library.key());
    const auto                   it    = state.find(section.category.key);
    return it != state.end() && it->second;
}

void AssetGalleryGrid::layout()
{
    m_items.clear();
    // The hover index points into the list being thrown away here, and the pointer has not moved, so
    // there is nothing hovered until the next motion event says what is.
    m_hover        = -1;
    m_hover_remove = false;
    const int width = GetClientSize().x;
    if (width <= 0) {
        // No width to lay out in yet: wxEVT_SIZE lays the grid out again as soon as there is one, so
        // committing a layout from a width that is not real would only have to be thrown away.
        m_content_h = 0;
        return;
    }

    const int avail = std::max(m_tile_w, width - 2 * m_margin);
    const int cols  = std::max(1, (avail + m_spacing) / (m_tile_w + m_spacing));

    int y = m_margin;
    for (size_t s = 0; s < m_sections.size(); ++s) {
        const Section &section = m_sections[s];
        Item           header;
        header.kind    = Item::Kind::Header;
        header.section = s;
        header.rect    = wxRect(m_margin, y, avail, m_header_h);
        m_items.push_back(header);
        y += m_header_h;

        if (is_collapsed(section)) {
            y += m_spacing;
            continue;
        }

        // The Add tile sits after the user's own entries, where it reads as "and one more", and it is
        // hidden while a search is running: it is not a search result.
        const bool add_tile = section.category.accepts_files && !m_library.file_wildcard().empty() && m_filter.empty();
        const int  count    = int(section.entries.size()) + (add_tile ? 1 : 0);
        for (int i = 0; i < count; ++i) {
            Item tile;
            tile.kind    = i < int(section.entries.size()) ? Item::Kind::Tile : Item::Kind::AddTile;
            tile.section = s;
            tile.entry   = i < int(section.entries.size()) ? section.entries[size_t(i)] : nullptr;
            tile.rect    = wxRect(m_margin + (i % cols) * (m_tile_w + m_spacing),
                                  y + (i / cols) * (m_tile_h + m_spacing), m_tile_w, m_tile_h);
            m_items.push_back(tile);
        }
        if (count > 0) {
            const int rows = (count + cols - 1) / cols;
            y += rows * m_tile_h + (rows - 1) * m_spacing;
        } else {
            Item empty;
            empty.kind    = Item::Kind::Empty;
            empty.section = s;
            empty.rect    = wxRect(m_margin, y, avail, m_line_h);
            m_items.push_back(empty);
            y += m_line_h;
        }

        if (!section.category.hint.empty() && m_filter.empty()) {
            Item hint;
            hint.kind    = Item::Kind::Hint;
            hint.section = s;
            hint.rect    = wxRect(m_margin, y + m_spacing, avail, m_line_h);
            m_items.push_back(hint);
            y += m_spacing + m_line_h;
        }
        y += m_margin;
    }

    m_content_h = y;
    // The virtual width is the client width and the horizontal rate is 0, so this never produces a
    // horizontal scrollbar; only the height is really being set here.
    SetVirtualSize(width, m_content_h);
}

void AssetGalleryGrid::on_size(wxSizeEvent &event)
{
    layout();
    if (!m_reveal_id.empty() && GetClientSize().x > 0) {
        // The constructor asked for the selection to be shown before the grid had a width; this is
        // the first moment it can be honoured.
        reveal(m_reveal_id);
        m_reveal_id.clear();
    }
    Refresh();
    event.Skip();
}

wxRect AssetGalleryGrid::remove_rect(const wxRect &tile) const
{
    const int d = std::max(FromDIP(16), m_line_h);
    return wxRect(tile.GetRight() - d - FromDIP(4), tile.y + FromDIP(4), d, d);
}

wxBitmap AssetGalleryGrid::thumbnail_for(const AssetEntry &entry)
{
    AssetThumbnailStyle style;
    style.size       = wxSize(m_tile_w - FromDIP(12), m_thumb_h - FromDIP(10));
    style.scale      = GetDPIScaleFactor();
    style.dark       = wxGetApp().dark_mode();
    style.background = m_tile;
    return m_library.thumbnail(entry, style);
}

void AssetGalleryGrid::queue_thumbnail(const AssetEntry *entry)
{
    if (std::find(m_pending.begin(), m_pending.end(), entry) == m_pending.end())
        m_pending.push_back(entry);
}

void AssetGalleryGrid::on_loader_tick(wxTimerEvent &event)
{
    (void) event;
    for (int i = 0; i < LOADS_PER_TICK && !m_pending.empty(); ++i) {
        const AssetEntry *entry = m_pending.front();
        m_pending.erase(m_pending.begin());
        m_thumbnails[entry->id] = thumbnail_for(*entry);
    }
    // The next paint refills the queue from what is visible then, so a tile the user scrolled past
    // while this was running is never decoded.
    Refresh();
}

void AssetGalleryGrid::on_paint(wxPaintEvent &event)
{
    (void) event;
    wxPaintDC dc(this);

    const wxSize client = GetClientSize();
    if (client.x <= 0 || client.y <= 0)
        return;

    // A back buffer rather than a buffered paint DC, because everything below is drawn through
    // wxGCDC: on MSW a plain paint DC is GDI, with no alpha and no anti-aliasing, and the tiles are
    // rounded rectangles.
    wxBitmap buffer;
    if (!buffer.CreateWithLogicalSize(client, GetDPIScaleFactor()))
        return; // nothing to draw into, and a memory DC over an invalid bitmap draws nowhere
    wxMemoryDC mdc(buffer);
    mdc.SetBackground(wxBrush(m_bg));
    mdc.Clear();
    {
        wxGCDC gdc(mdc);
        gdc.SetFont(GetFont()); // a memory DC starts with the system default font, not the window's
        gdc.SetTextForeground(m_text);

        // Drawn in client coordinates, offset by the scroll position: the item rects are content
        // coordinates, and the same shift in reverse is what hit_test() undoes.
        const wxPoint origin  = CalcScrolledPosition(wxPoint(0, 0));
        const wxRect  visible(CalcUnscrolledPosition(wxPoint(0, 0)), client);

        m_pending.clear();
        for (const Item &item : m_items) {
            if (!item.rect.Intersects(visible))
                continue;
            Item shifted = item;
            shifted.rect.Offset(origin);
            switch (item.kind) {
            case Item::Kind::Header: draw_header(gdc, shifted); break;
            case Item::Kind::Tile: draw_tile(gdc, shifted); break;
            case Item::Kind::AddTile: draw_add_tile(gdc, shifted); break;
            case Item::Kind::Hint:
                gdc.SetTextForeground(m_dim);
                gdc.DrawText(wxControl::Ellipsize(m_sections[item.section].category.hint, gdc, wxELLIPSIZE_END,
                                                  shifted.rect.width, wxELLIPSIZE_FLAGS_NONE),
                             shifted.rect.x, shifted.rect.y);
                break;
            case Item::Kind::Empty:
                gdc.SetTextForeground(m_dim);
                gdc.DrawText(_L("Nothing here yet."), shifted.rect.x, shifted.rect.y);
                break;
            }
        }
    }
    mdc.SelectObject(wxNullBitmap); // the buffer cannot be blitted while it is still selected
    dc.DrawBitmap(buffer, 0, 0);

    // Starting the timer is not drawing and not an invalidation, so it is safe from inside the paint
    // path; the decode itself happens on the tick, off this stack.
    if (!m_pending.empty() && !m_loader.IsRunning())
        m_loader.Start(LOADER_INTERVAL_MS, wxTIMER_ONE_SHOT);
}

void AssetGalleryGrid::draw_header(wxGCDC &gdc, const Item &item)
{
    const Section &section   = m_sections[item.section];
    const bool     collapsed = is_collapsed(section);
    const int      inset     = FromDIP(2);
    const int      arrow     = std::max(FromDIP(5), m_line_h / 3);
    const int      cx        = item.rect.x + inset + arrow;
    const int      cy        = item.rect.y + item.rect.height / 2;

    // A filled triangle, pointing right when the section is folded away and down when it is open:
    // the same signal the texture picker's sections give, and the only part of the row that is not
    // text.
    gdc.SetBrush(wxBrush(m_accent));
    gdc.SetPen(wxPen(m_accent, 1));
    wxPoint triangle[3];
    if (collapsed) {
        triangle[0] = wxPoint(cx - arrow / 2, cy - arrow);
        triangle[1] = wxPoint(cx + arrow / 2, cy);
        triangle[2] = wxPoint(cx - arrow / 2, cy + arrow);
    } else {
        triangle[0] = wxPoint(cx - arrow, cy - arrow / 2);
        triangle[1] = wxPoint(cx + arrow, cy - arrow / 2);
        triangle[2] = wxPoint(cx, cy + arrow / 2);
    }
    gdc.DrawPolygon(3, triangle);

    const wxString count = wxString::Format("%d", int(section.entries.size()));
    const int      count_w = gdc.GetTextExtent(count).x;
    const int      title_x = cx + arrow + m_spacing;
    const int      title_w = std::max(0, item.rect.GetRight() - count_w - m_spacing - title_x);
    gdc.SetTextForeground(m_text);
    // No mnemonic or tab processing: these are names, and an '&' in one is an '&', not an underline.
    gdc.DrawText(wxControl::Ellipsize(section.category.title, gdc, wxELLIPSIZE_END, title_w, wxELLIPSIZE_FLAGS_NONE),
                 title_x, item.rect.y + (item.rect.height - m_line_h) / 2);
    gdc.SetTextForeground(m_dim);
    gdc.DrawText(count, item.rect.GetRight() - count_w, item.rect.y + (item.rect.height - m_line_h) / 2);

    // A hairline under the heading, so a folded section still reads as a section.
    gdc.SetPen(wxPen(m_separator, 1));
    gdc.DrawLine(item.rect.x, item.rect.GetBottom(), item.rect.GetRight(), item.rect.GetBottom());
}

void AssetGalleryGrid::draw_tile(wxGCDC &gdc, const Item &item)
{
    const AssetEntry &entry    = *item.entry;
    const bool        current  = entry.id == m_selection;
    const Item       *over     = hovered_item();
    const bool        hovered  = over != nullptr && over->entry == item.entry;
    const bool        user     = entry.origin == AssetOrigin::User;

    gdc.SetBrush(wxBrush(current ? m_selected : m_tile));
    gdc.SetPen(wxPen(current ? m_accent : (hovered ? m_accent : m_tile_border), current ? FromDIP(2) : 1));
    gdc.DrawRoundedRectangle(item.rect, m_radius);

    const wxRect thumb(item.rect.x, item.rect.y, item.rect.width, m_thumb_h);
    const auto   cached = m_thumbnails.find(entry.id);
    if (cached == m_thumbnails.end()) {
        // Not decoded yet: a placeholder now, the picture on one of the next ticks.
        queue_thumbnail(item.entry);
        gdc.SetBrush(wxBrush(m_separator));
        gdc.SetPen(*wxTRANSPARENT_PEN);
        const int d = m_thumb_h / 3;
        gdc.DrawRoundedRectangle(thumb.x + (thumb.width - d) / 2, thumb.y + (thumb.height - d) / 2, d, d,
                                 FromDIP(3));
    } else if (cached->second.IsOk()) {
        const wxSize size = cached->second.GetLogicalSize();
        gdc.DrawBitmap(cached->second, thumb.x + (thumb.width - size.x) / 2, thumb.y + (thumb.height - size.y) / 2,
                       true);
    } else {
        // The library could not make a picture of it. Say so in the tile rather than leaving a blank
        // one: the file is still listed, and the user is the only one who can fix it.
        gdc.SetTextForeground(m_dim);
        const wxString mark = _L("No preview");
        const wxSize   ext  = gdc.GetTextExtent(mark);
        gdc.DrawText(wxControl::Ellipsize(mark, gdc, wxELLIPSIZE_END, thumb.width - FromDIP(8),
                                          wxELLIPSIZE_FLAGS_NONE),
                     thumb.x + std::max(FromDIP(4), (thumb.width - ext.x) / 2),
                     thumb.y + (thumb.height - ext.y) / 2);
    }

    // The user's own entries are told apart by their caption colour, not only by which section they
    // are in: the sections collapse, and a search mixes every section's hits into one list.
    gdc.SetTextForeground(user ? m_accent : m_text);
    const wxString name = wxControl::Ellipsize(entry.name, gdc, wxELLIPSIZE_MIDDLE, item.rect.width - FromDIP(8),
                                               wxELLIPSIZE_FLAGS_NONE);
    const int      name_w = gdc.GetTextExtent(name).x;
    gdc.DrawText(name, item.rect.x + (item.rect.width - name_w) / 2,
                 item.rect.y + m_thumb_h + (m_caption_h - m_line_h) / 2);

    if (user && m_sections[item.section].category.removable && hovered) {
        const wxRect r = remove_rect(item.rect);
        gdc.SetBrush(wxBrush(m_hover_remove ? wxColour(214, 72, 64) : m_tile_border));
        gdc.SetPen(*wxTRANSPARENT_PEN);
        gdc.DrawEllipse(r);
        gdc.SetPen(wxPen(m_hover_remove ? *wxWHITE : m_text, std::max(1, FromDIP(1))));
        const int k = r.width / 5;
        const wxPoint c(r.x + r.width / 2, r.y + r.height / 2);
        gdc.DrawLine(c.x - k, c.y - k, c.x + k, c.y + k);
        gdc.DrawLine(c.x - k, c.y + k, c.x + k, c.y - k);
    }
}

void AssetGalleryGrid::draw_add_tile(wxGCDC &gdc, const Item &item)
{
    const Item *over    = hovered_item();
    const bool  hovered = over != nullptr && over->kind == Item::Kind::AddTile && over->section == item.section;
    gdc.SetBrush(wxBrush(m_bg));
    gdc.SetPen(wxPen(hovered ? m_accent : m_tile_border, 1));
    gdc.DrawRoundedRectangle(item.rect, m_radius);

    const wxPoint c(item.rect.x + item.rect.width / 2, item.rect.y + m_thumb_h / 2);
    const int     k = m_thumb_h / 6;
    gdc.SetPen(wxPen(m_accent, std::max(1, FromDIP(2))));
    gdc.DrawLine(c.x - k, c.y, c.x + k, c.y);
    gdc.DrawLine(c.x, c.y - k, c.x, c.y + k);

    gdc.SetTextForeground(m_dim);
    const wxString label  = _L("Add from file");
    const wxString shown  = wxControl::Ellipsize(label, gdc, wxELLIPSIZE_END, item.rect.width - FromDIP(8),
                                                 wxELLIPSIZE_FLAGS_NONE);
    const int      text_w = gdc.GetTextExtent(shown).x;
    gdc.DrawText(shown, item.rect.x + (item.rect.width - text_w) / 2,
                 item.rect.y + m_thumb_h + (m_caption_h - m_line_h) / 2);
}

const AssetGalleryGrid::Item *AssetGalleryGrid::hovered_item() const
{
    return m_hover >= 0 && m_hover < int(m_items.size()) ? &m_items[size_t(m_hover)] : nullptr;
}

int AssetGalleryGrid::hit_test(const wxPoint &content_pos) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].rect.Contains(content_pos))
            return int(i);
    return -1;
}

const AssetEntry *AssetGalleryGrid::selected_entry() const
{
    for (const Section &section : m_sections)
        for (const AssetEntry *entry : section.entries)
            if (entry->id == m_selection)
                return entry;
    return nullptr;
}

bool AssetGalleryGrid::accepts_files() const
{
    if (m_library.file_wildcard().empty())
        return false;
    for (const Section &section : m_sections)
        if (section.category.accepts_files)
            return true;
    return false;
}

void AssetGalleryGrid::select_entry(const AssetEntry *entry)
{
    if (entry == nullptr || entry->id == m_selection)
        return;
    m_selection = entry->id;
    Refresh();
    if (on_selection_changed)
        on_selection_changed();
}

void AssetGalleryGrid::on_mouse_down(wxMouseEvent &event)
{
    // The grid takes the focus so the arrow keys work from here on; the search box has it until the
    // user has picked something to move from.
    SetFocus();

    const wxPoint pos   = CalcUnscrolledPosition(event.GetPosition());
    const int     index = hit_test(pos);
    if (index < 0) {
        event.Skip();
        return;
    }
    const Item &item = m_items[size_t(index)];
    if (item.kind == Item::Kind::Header) {
        const bool collapsed                     = is_collapsed(m_sections[item.section]);
        collapsed_state(m_library.key())[m_sections[item.section].category.key] = !collapsed;
        layout();
        Refresh();
        return;
    }
    if (item.kind == Item::Kind::AddTile) {
        // Never from inside a mouse handler: the file dialog is modal, and a window opened on this
        // stack would run its own event loop under ours. CallAfter on this window is dropped if the
        // window is destroyed first, which is what makes it safe to capture `this`.
        CallAfter([this]() {
            if (on_add_file)
                on_add_file();
        });
        return;
    }
    if (item.kind == Item::Kind::Tile && item.entry != nullptr) {
        if (item.entry->origin == AssetOrigin::User && m_sections[item.section].category.removable &&
            remove_rect(item.rect).Contains(pos)) {
            const AssetEntry entry = *item.entry; // by value: removing rebuilds the library's vector
            CallAfter([this, entry]() {
                if (on_remove)
                    on_remove(entry);
            });
            return;
        }
        select_entry(item.entry);
        return;
    }
    event.Skip();
}

void AssetGalleryGrid::on_double_click(wxMouseEvent &event)
{
    const int index = hit_test(CalcUnscrolledPosition(event.GetPosition()));
    if (index < 0 || m_items[size_t(index)].kind != Item::Kind::Tile) {
        event.Skip();
        return;
    }
    select_entry(m_items[size_t(index)].entry);
    // Closing the window is work for the window, and not on a mouse handler's stack.
    CallAfter([this]() {
        if (on_activated)
            on_activated();
    });
}

void AssetGalleryGrid::on_motion(wxMouseEvent &event)
{
    const wxPoint pos    = CalcUnscrolledPosition(event.GetPosition());
    const int     index  = hit_test(pos);
    const bool    is_tile = index >= 0 && m_items[size_t(index)].kind == Item::Kind::Tile;
    const bool    remove  = is_tile && m_items[size_t(index)].entry != nullptr &&
                         m_items[size_t(index)].entry->origin == AssetOrigin::User &&
                         m_sections[m_items[size_t(index)].section].category.removable &&
                         remove_rect(m_items[size_t(index)].rect).Contains(pos);
    if (index != m_hover || remove != m_hover_remove) {
        m_hover        = index;
        m_hover_remove = remove;
        if (remove)
            SetToolTip(_L("Remove this from your own files."));
        else if (is_tile)
            SetToolTip(m_items[size_t(index)].entry->tooltip.empty() ? m_items[size_t(index)].entry->name
                                                                     : m_items[size_t(index)].entry->tooltip);
        else
            UnsetToolTip();
        Refresh();
    }
    event.Skip();
}

void AssetGalleryGrid::on_leave(wxMouseEvent &event)
{
    if (m_hover != -1 || m_hover_remove) {
        m_hover        = -1;
        m_hover_remove = false;
        Refresh();
    }
    event.Skip();
}

void AssetGalleryGrid::move_selection(int dx, int dy)
{
    // The tiles in the order they are laid out, which is also the order the eye reads them; a vertical
    // step is a whole row, so it has to know how many tiles a row holds.
    std::vector<const AssetEntry *> tiles;
    int                             cols = 1;
    int                             row_y = -1;
    int                             in_row = 0;
    for (const Item &item : m_items) {
        if (item.kind != Item::Kind::Tile || item.entry == nullptr)
            continue;
        if (item.rect.y != row_y) {
            row_y  = item.rect.y;
            in_row = 0;
        }
        cols = std::max(cols, ++in_row);
        tiles.push_back(item.entry);
    }
    if (tiles.empty())
        return;

    int at = 0;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i]->id == m_selection)
            at = int(i);
    const int next = std::clamp(at + dx + dy * cols, 0, int(tiles.size()) - 1);
    select_entry(tiles[size_t(next)]);
    reveal(m_selection);
}

void AssetGalleryGrid::on_key_down(wxKeyEvent &event)
{
    switch (event.GetKeyCode()) {
    case WXK_LEFT: move_selection(-1, 0); break;
    case WXK_RIGHT: move_selection(1, 0); break;
    case WXK_UP: move_selection(0, -1); break;
    case WXK_DOWN: move_selection(0, 1); break;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
        if (!m_selection.empty() && on_activated)
            on_activated();
        break;
    default: event.Skip(); return;
    }
}

void AssetGalleryGrid::reveal(const std::string &id)
{
    if (id.empty() || m_scroll_unit <= 0)
        return;
    for (const Item &item : m_items) {
        if (item.kind != Item::Kind::Tile || item.entry == nullptr || item.entry->id != id)
            continue;
        const wxRect visible(CalcUnscrolledPosition(wxPoint(0, 0)), GetClientSize());
        if (visible.Contains(item.rect))
            return; // already in view: scrolling it to the middle would only move the grid under the user
        const int centred = item.rect.y + item.rect.height / 2 - visible.height / 2;
        Scroll(-1, std::max(0, centred) / m_scroll_unit);
        return;
    }
}

// ----------------------------------------------------------------------------------------------
// The window
// ----------------------------------------------------------------------------------------------

AssetGallery::AssetGallery(wxWindow *parent, AssetLibrary &library, const AssetGalleryOptions &options)
    : DPIDialog(parent != nullptr ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                options.heading.empty() ? library.title() : options.heading, wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_library(library)
    , m_selected(options.selected_id)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    // The search box and the Add button on one row: the two things that change what the grid holds.
    auto *top = new wxBoxSizer(wxHORIZONTAL);
    m_search  = new TextInput(this, wxEmptyString, wxEmptyString, "search", wxDefaultPosition,
                              wxSize(FromDIP(240), -1), wxTE_PROCESS_ENTER);
    m_search->GetTextCtrl()->SetHint(_L("Search by name"));
    top->Add(m_search, 1, wxALIGN_CENTER_VERTICAL);
    if (!library.file_wildcard().empty()) {
        m_add = new Button(this, _L("Add from file"));
        m_add->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        top->Add(m_add, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    }
    sizer->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    m_grid = new AssetGalleryGrid(this, library);
    // A scrolled window reports only its min size in a scrolling direction, so without both of these
    // the grid collapses to the width of its scrollbar.
    m_grid->SetMinSize(wxSize(FromDIP(560), FromDIP(420)));
    sizer->Add(m_grid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    m_footer = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_footer->SetForegroundColour(StateColor::darkModeColorFor(DIM_TEXT_COLOUR));
    sizer->Add(m_footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    // Before the selection is first reported, so the caller's controls are there to be told about it.
    if (options.build_extra)
        if (wxWindow *extra = options.build_extra(this); extra != nullptr)
            sizer->Add(extra, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
    m_on_selection_changed = options.on_selection_changed;

    m_buttons = new DialogButtons(this, {"OK", "Cancel"});
    sizer->Add(m_buttons, 0, wxEXPAND);

    m_buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { accept(); });
    m_buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    if (m_add != nullptr)
        m_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_from_file(); });

    m_search->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &event) {
        m_grid->set_filter(m_search->GetTextCtrl()->GetValue());
        update_footer();
        event.Skip();
    });
    // Enter in the search box hands the keyboard to the grid rather than accepting: what the user
    // typed is a filter, and nothing has been chosen yet.
    m_search->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { m_grid->SetFocus(); });

    m_grid->on_selection_changed = [this]() {
        m_selected = m_grid->selection();
        update_footer();
    };
    m_grid->on_activated = [this]() { accept(); };
    m_grid->on_add_file  = [this]() { add_from_file(); };
    m_grid->on_remove    = [this](const AssetEntry &entry) { remove_entry(entry); };

    m_grid->set_hidden_categories(options.hidden_categories);
    rebuild(options.selected_id, /* reveal */ true);

    SetSizerAndFit(sizer);
    CenterOnParent();
    m_search->SetFocus();
    wxGetApp().UpdateDlgDarkUI(this);
}

void AssetGallery::rebuild(const std::string &select_id, bool reveal)
{
    m_grid->reload(/* force_rescan */ true, select_id, reveal);
    m_selected = m_grid->selection();
    if (m_add != nullptr)
        m_add->Enable(m_grid->accepts_files());
    update_footer();
}

void AssetGallery::update_footer()
{
    const AssetEntry *entry = m_grid->selected_entry();
    // The name of what OK would take, because a tile's caption is ellipsized and the ring on it is
    // easy to lose in a long grid. No count here: the section headings carry those, and a bare number
    // would have to be a plural form for no gain.
    m_footer->SetLabel(entry != nullptr ? format_wxstr(_L("Selected: %1%"), entry->name)
                                        : _L("Nothing selected yet."));
    if (Button *ok = m_buttons->GetOK(); ok != nullptr)
        ok->Enable(entry != nullptr);
    // Here rather than in the grid's own callback, so extra content is told about a selection a
    // rebuild moved as well as one the user clicked.
    if (m_on_selection_changed)
        m_on_selection_changed(entry != nullptr ? entry->id : std::string{});
    Layout();
}

void AssetGallery::accept()
{
    const AssetEntry *entry = m_grid->selected_entry();
    if (entry == nullptr)
        return; // nothing picked: the OK button is disabled, and Enter on an empty grid does nothing
    m_selected = entry->id;
    m_library.touch(*entry);
    EndModal(wxID_OK);
}

void AssetGallery::add_from_file()
{
    const wxString wildcard = m_library.file_wildcard();
    if (wildcard.empty())
        return;
    wxFileDialog dialog(this, m_library.file_prompt().empty() ? _L("Choose a file") : m_library.file_prompt(),
                        wxEmptyString, wxEmptyString, wildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    std::string                      error;
    const std::optional<std::string> id = m_library.add_from_file(into_u8(dialog.GetPath()), error);
    if (!id) {
        show_error(this, error.empty() ? _u8L("Could not add that file.") : error);
        return;
    }
    // Selected as well as added: the user picked this file, and making them find it again in the grid
    // would be the window asking twice.
    rebuild(*id, /* reveal */ true);
}

void AssetGallery::remove_entry(const AssetEntry &entry)
{
    MessageDialog confirm(this, format_wxstr(_L("Remove \"%1%\" from your own files?"), entry.name),
                          _L("Remove file"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    std::string error;
    if (!m_library.remove(entry, error)) {
        show_error(this, error.empty() ? _u8L("Could not remove that file.") : error);
        return;
    }
    rebuild(/* select_id */ {}, /* reveal */ false);
}

void AssetGallery::on_dpi_changed(const wxRect &suggested_rect)
{
    (void) suggested_rect;
    m_search->Rescale();
    if (m_add != nullptr)
        m_add->Rescale();
    m_grid->SetMinSize(wxSize(FromDIP(560), FromDIP(420)));
    m_grid->rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void AssetGallery::on_sys_color_changed()
{
    m_footer->SetForegroundColour(StateColor::darkModeColorFor(DIM_TEXT_COLOUR));
    m_grid->apply_theme();
    Refresh();
}

std::optional<std::string> pick_asset(wxWindow *parent, AssetLibrary &library, const AssetGalleryOptions &options)
{
    AssetGallery gallery(parent, library, options);
    if (gallery.ShowModal() != wxID_OK || gallery.selected_id().empty())
        return std::nullopt;
    return gallery.selected_id();
}

} // namespace Slic3r::GUI
