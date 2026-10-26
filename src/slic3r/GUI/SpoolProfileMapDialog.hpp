#pragma once

// Picker that couples the spools a printer reports to filament profiles.
//
// Runs as the last step of the stock "sync with printer" flow, right before the spools are
// committed, so the user picks a real profile instead of letting every unknown spool fall back to
// a Generic one. Only offered for a printer that actually published its spools through
// SpoolProfileMap; anything else takes the path it always took.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/window.h>

#include "GUI_Utils.hpp"
#include "SpoolTile.hpp"
#include "slic3r/Utils/SpoolProfileMap.hpp"

class CheckBox;
class Label;

namespace Slic3r { namespace GUI {

// The sidebar's filament selector, restricted to the profiles that fit one spool. Defined in the
// .cpp: nothing outside the picker has a use for it.
class SpoolPresetComboBox;

class SpoolProfileMapDialog : public DPIDialog
{
public:
    // One filament as the user left it: which lane it came from, which profile it got.
    struct Chosen
    {
        int         slot_index = 0;
        std::string preset_name;
        std::string filament_id;
    };

    // Step 1, called just before the spools are committed. Asks which profile belongs on which
    // spool and writes that answer into the AMS list the commit reads, so the commit resolves the
    // chosen profile instead of a Generic one. Nullopt only when the user cancelled the picker, and
    // then the caller must commit nothing. Empty when there is nothing to link: the printer
    // published no spools, the feature is switched off, or the sync only takes colors.
    static std::optional<std::vector<Chosen>> run_link(wxWindow* parent, const std::string& dev_id);

    // Step 2, called right after the commit with what step 1 returned. A profile the user made
    // themselves carries the filament id of the system profile it came from, so the commit can only
    // land on that parent; this puts the user's own profile back in its place.
    static void apply_links(const std::vector<Chosen>& chosen);

    SpoolProfileMapDialog(wxWindow* parent, std::vector<SpoolIdentity> spools);
    ~SpoolProfileMapDialog() override = default;

    const std::vector<Chosen>& chosen() const { return m_chosen; }

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
    void refill_combos();
    void remember_row(const Row& row, const std::string& preset_name) const;
    void on_confirm();

    std::vector<Row>                  m_rows;
    std::vector<Chosen>               m_chosen;
    std::vector<SpoolPresetCandidate> m_candidates;
    bool                              m_show_all = false;
    bool                              m_remember = true;
    wxScrolledWindow*                 m_scroll   = nullptr;
};

// The spools the user has linked to a profile, one tile each with its profile as the label. Opened
// from the filament title bar; independent of any printer being connected, since the links are read
// from disk. The spools are filed by material. Clicking a tile opens that spool's details.
class SpoolProfileLinksDialog : public DPIDialog
{
public:
    static void run_manage(wxWindow* parent, SpoolLook look);

    SpoolProfileLinksDialog(wxWindow* parent, SpoolLook look);
    ~SpoolProfileLinksDialog() override = default;

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build(wxWindow* parent);
    // Drawn again after a spool was edited or forgotten, from what is on disk.
    void fill_shelf();
    void add_material_group(wxWindow* parent, wxSizer* shelf, const std::string& material, const std::vector<SpoolLink>& links);
    void add_tile(wxWindow* parent, wxFlexGridSizer* grid, const SpoolLink& link, const wxString& material);
    void add_empty_note(wxWindow* parent, wxSizer* shelf);
    void open_detail(SpoolLink link);
    void on_confirm();

    SpoolLook         m_look;
    SpoolMapSettings  m_settings;
    ::CheckBox*       m_only_new_check = nullptr;
    Label*            m_only_new_label = nullptr;
    wxScrolledWindow* m_scroll         = nullptr;
};

// One linked spool: every color it was seen in, its profile, and the colors that print on a
// profile of their own.
class SpoolDetailDialog : public DPIDialog
{
public:
    SpoolDetailDialog(wxWindow* parent, SpoolLink link);
    ~SpoolDetailDialog() override = default;

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // One color a filament has been seen in. It gets a row of its own under the filament once the
    // user asks for it, and keeps its own profile from then on.
    struct ColorRow
    {
        std::string          color;            // "#RRGGBB", or several for a multicolor spool
        std::string          initial;          // the profile saved for this color, empty when none
        std::string          preset;           // what its menu says now, kept across a redraw
        bool                 split   = false;  // shown as a row of its own
        bool                 pending = false;  // just clicked, still on the filament's profile
        bool                 removed = false;
        bool                 filled  = false;  // its menu has been built once, so initial is known
        SpoolPresetComboBox* combo   = nullptr;
    };

    struct Row
    {
        SpoolLink             link;    // the spool, its profile and its name, as they are on disk
        std::string           initial; // what the menu showed when the panel opened
        std::string           preset;  // what the menu says now, kept across a redraw
        bool                  collapsed = false;
        bool                  filled    = false;
        std::vector<ColorRow> colors;
        SpoolPresetComboBox*  combo = nullptr;
    };

    static Row make_row(SpoolLink link);

    void build(wxWindow* parent);
    // Draws the list again after it changed shape: a color split out, a row folded. The menus are
    // rebuilt on what they were showing, never on what is on disk.
    void      rebuild_rows();
    void      take_menu_choices();
    void      settle_color_rows();
    wxWindow* make_row_head(wxWindow* parent);
    void      add_chips(wxWindow* head, wxBoxSizer* head_sizer);
    void      add_row(wxWindow* parent, wxFlexGridSizer* grid);
    void      add_color_rows(wxWindow* parent, wxFlexGridSizer* grid);
    void      add_color_row(wxWindow* parent, wxFlexGridSizer* grid, ColorRow& color, bool last);
    void      split_color(ColorRow& color);
    void      forget_link();
    void      on_confirm();
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
