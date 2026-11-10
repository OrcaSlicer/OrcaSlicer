#pragma once

// The windows that make, show and edit the spool -> profile links, see SpoolProfileLinker.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/window.h>

#include "GUI_Utils.hpp"
#include "SpoolProfileLinker.hpp"
#include "SpoolTile.hpp"
#include "slic3r/Utils/SpoolProfileMap.hpp"

namespace Slic3r { namespace GUI {

// The sidebar's filament selector, restricted to the profiles that fit one spool. Defined in the
// .cpp: nothing outside these windows has a use for it.
class SpoolPresetComboBox;

// Asks which profile each spool on the printer takes, as part of "sync with printer".
class SpoolProfileMapDialog : public DPIDialog
{
public:
    // Called just before the spools are committed. Asks only when a filament, or a colour of it, is
    // new; otherwise gives the saved links without a window. Nullopt only when the user cancelled,
    // and then the caller must commit nothing.
    static std::optional<std::vector<SpoolChoice>> run_link(wxWindow* parent, const std::string& dev_id);

    SpoolProfileMapDialog(wxWindow* parent, std::vector<SpoolIdentity> spools);
    ~SpoolProfileMapDialog() override = default;

    const std::vector<SpoolChoice>& chosen() const { return m_chosen; }

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    struct Row
    {
        SpoolIdentity        spool;
        std::string          key;
        SpoolPresetComboBox* combo = nullptr;
    };

    void build(wxWindow* parent);
    void add_row(wxWindow* parent, wxFlexGridSizer* grid, Row& row);
    void on_confirm();

    std::vector<Row>                  m_rows;
    std::vector<SpoolChoice>          m_chosen;
    std::vector<SpoolPresetCandidate> m_candidates;
    wxScrolledWindow*                 m_scroll = nullptr;
};

// The filaments the user has linked to a profile, one spool each with its name under it. Opened
// from the filament title bar and from Preferences; independent of any printer being connected,
// since the links are read from disk. The filaments are filed by the "Group user filament presets"
// preference. Clicking a spool opens that filament's colors and profiles.
class SpoolProfileLinksDialog : public DPIDialog
{
public:
    static void run_manage(wxWindow* parent);

    explicit SpoolProfileLinksDialog(wxWindow* parent);
    ~SpoolProfileLinksDialog() override = default;

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build(wxWindow* parent);
    // Drawn again after a spool was edited or forgotten, from what is on disk.
    void fill_shelf();
    // An empty heading puts the tiles straight on the shelf.
    void add_group(wxWindow* parent, wxSizer* shelf, const std::string& group, const wxString& heading, const std::vector<SpoolLink>& links);
    void add_tile(wxWindow* parent, wxFlexGridSizer* grid, const std::string& group, const SpoolLink& link);
    void add_empty_note(wxWindow* parent, wxSizer* shelf);
    void open_detail(SpoolLink link);

    wxScrolledWindow* m_scroll = nullptr;
};

// One linked filament: its profile on top, then every color it was seen in with the profile that
// color prints with.
class SpoolDetailDialog : public DPIDialog
{
public:
    SpoolDetailDialog(wxWindow* parent, SpoolLink link);
    ~SpoolDetailDialog() override = default;

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // One color a filament has been seen in.
    struct ColorRow
    {
        std::string color;   // "#RRGGBB", or several for a multicolor spool
        std::string initial; // the profile saved for this color, empty when none
        std::string own;     // the profile it prints with now, empty when it follows the filament
        bool        removed = false; // the user asked to forget this color; written on OK
    };

    struct Row
    {
        SpoolLink             link;    // the spool, its profile and its name, as they are on disk
        std::string           initial; // what the menu showed when the panel opened
        std::string           preset;  // what the menu says now, kept across a redraw
        bool                  filled = false;
        std::vector<ColorRow> colors;
        SpoolPresetComboBox*  combo = nullptr;
    };

    static Row make_row(SpoolLink link);

    void build(wxWindow* parent);
    // Draws the list again after a profile was picked or a color removed. The
    // filament's menu is rebuilt on what it was showing, never on what is on disk.
    void                 rebuild_rows();
    void                 take_menu_choice();
    void                 settle_colors();
    wxSizer*             make_filament_line(wxWindow* parent);
    wxSizer*             make_variant_chips(wxWindow* parent);
    size_t               kept_color_count() const;
    VariantChip*         make_variant_chip(wxWindow* parent, ColorRow& color, bool can_forget);
    SpoolPresetComboBox* add_chip_menu(VariantChip* chip, ColorRow& color);
    void                 forget_link();
    void                 on_confirm();
    // Writes back each color of the filament, given the profile the filament itself ends up on.
    void remember_colors(const std::string& picked) const;
    // Moves the project's own filament selection from the profile the link left to the one it now
    // points at.
    static void follow_link_in_project(const std::string& old_name, const std::string& new_name);

    Row                               m_row;
    std::vector<SpoolPresetCandidate> m_candidates;
    wxScrolledWindow*                 m_scroll = nullptr;
};

}} // namespace Slic3r::GUI
