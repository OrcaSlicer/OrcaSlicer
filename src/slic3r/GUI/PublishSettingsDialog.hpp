#pragma once

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/PublishSelection.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include <wx/wx.h>
#include <wx/colour.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <wx/timer.h>
#include <utility>
#include <vector>
#include <string>
#include <boost/any.hpp>

class TabCtrl;
class wxBitmapButton;
class wxBoxSizer;
class wxCommandEvent;
class wxMouseEvent;
class wxPanel;
class wxScrolledWindow;
class wxSizerItem;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;
class wxWindow;
namespace Slic3r { class DynamicPrintConfig; }

// Forward declarations (all are global classes, see Widgets/TextInput.hpp and
// Widgets/StaticLine.hpp).
class TextInput;
class StaticLine;
class CheckBox;

namespace Slic3r { namespace GUI {

class Line;
class PublishFilamentPicker;
class PublishParamsPanel;
class Tab;

struct PublishMaterialIdentity
{
    std::string type;
    std::string vendor;
    std::string id;
};

// Dialog letting a model author select which settings get embedded in a 3MF. Three outer tabs
// (Printer / Filament / Process): the Printer and Process tabs host the sidebar's own settings tab,
// the Filament tab has one inner tab per used material slot and one per mixed slot. Dirty settings
// are pre-checked and drawn in the Tab's modified colour; on OK the print and printer rows become
// "orca_published_keys" and the material rows become "orca_published_material_keys".
class PublishSettingsDialog : public DPIDialog
{
public:
    // Optional published selection (Feature A/B): when the caller supplies one (either a
    // remembered session selection or the payload of a freshly loaded published 3MF) the dialog
    // is seeded from it, overriding the dirty-default pre-check. A non-null pointer to an empty
    // selection means "publish nothing" (an intentional empty state); a null pointer means "no
    // remembered selection" (keep the dirty defaults). legacy_process_selection marks a selection
    // read from a file without per-extruder process layout: its "key#N" process entries then select
    // that setting on every extruder.
    PublishSettingsDialog(wxWindow* parent = nullptr,
                          const std::vector<std::string>* published_keys = nullptr,
                          const std::vector<Slic3r::PublishedMaterialEntry>* material_keys = nullptr,
                          bool legacy_process_selection = false);
    ~PublishSettingsDialog();

    // The selected process and printer setting keys (in display order); a key of a variant-bearing
    // setting carries its variant column as a '#N' suffix.
    std::vector<std::string> GetPublishedKeys() const;

    // The selected keys grouped per material section. A variant-bearing key carries its file column
    // as a '#N' suffix; a per-slot key is the plain base key.
    std::vector<Slic3r::PublishedMaterialEntry> GetPublishedMaterialKeys() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

private:
    void fit_to_content();
    void refresh_mixed_tab_bitmaps();

    // Which part of the settings the row came from.
    enum class Section { Print, Printer, Material };

    // One option inside a row. A row is one source Line, so a multi-option row (Overhang speed,
    // Recommended nozzle temperature) stays one publish unit with one toggle.
    struct RowOption
    {
        std::string key;       // base config key
        int         index{-1}; // vector index the row's value addresses: slot, file column, or -1
    };

    // One selectable setting. This list is the dialog's durable state: it drives the filter, All
    // and None, seeding and the payload, whether or not the row's widgets have been built.
    struct Row
    {
        Slic3r::PublishRowId  id;                  // publish-ready keys, owner, column, section, kind
        std::vector<RowOption> options;            // one per published key: base key and its index
        // The hosted tab's own Line this row is drawn by; the publish checkbox is attached to it.
        Line*                 tab_line{nullptr};
        wxString              label;               // the label text the filter matches
        wxString              category;
        wxString              subcategory;         // optgroup heading, filter text only
        wxString              value;               // first option's value as shown, for the filter
        wxString              unit;
        wxString              section_title;       // outer tab title, for filter matching
        size_t                outer_index{0};
        size_t                inner_index{0};
        bool                  publishable{true};      // false for a row whose control is an action
        bool                  dirty{false};          // matches a dirty base key: pre-checked
        bool                  matches_filter{false}; // survives the active filter
    };

    // An inner TabCtrl page with its material controls and content.
    struct Category
    {
        wxString title;
        Section section{Section::Print};
        size_t group{0};        // index into m_sections / outer page
        size_t source_index{0}; // stable source page or material slot index
        wxPanel* page{nullptr};
        wxScrolledWindow* scroll{nullptr};
        wxBoxSizer* list_sizer{nullptr};
        // A physical material slot's settings are the sidebar's own Filament tab; the Printer and
        // Process sections' are the sidebar's own tabs. Null on a mixed slot.
        PublishParamsPanel* publish_host{nullptr};
        Tab*                publish_tab{nullptr};
        wxStaticText* info{nullptr};
        // The mixed definition preview, rebuilt when a component's colour changes.
        wxPanel* mixed_visual{nullptr};
        wxPoint scroll_pos{0, 0};
        // The slot's colour swatch, a button in the sidebar's style: a click writes the colour
        // to the project at once, and the badge and the tab strip show it.
        wxBitmapButton* filament_color_chip{nullptr};
        // A physical slot's two publish flags, below the picker, each a checkbox with a read-only
        // value: the colour it prints with and its normalised material type. Both start off and
        // follow the picked preset and the picked colour.
        ::CheckBox*     color_check{nullptr};
        wxStaticBitmap* color_value{nullptr};
        ::CheckBox*     type_check{nullptr};
        wxStaticText*   type_value{nullptr};
        // One publish switch per owner, never null after build. A physical slot has none: its
        // colour and type are the flag rows above, and its key rows publish independently. A mixed
        // slot's checkbox sits above its definition page and publishes the mix definition,
        // auto-Full-Publishing its component filaments.
        ::CheckBox* owner_check{nullptr};
        wxSizerItem* full_line_item{nullptr}; // sizer item of the Full Publish line (physical slots only)
        // "Full Publish": while checked, the whole slot preset is serialized and its rows are
        // disabled.
        ::CheckBox* full_check{nullptr};
        // The slot's filament preset picker (the sidebar's own combo), on its own line above the
        // colour/type rows, with the slot's colour badge on its left as the sidebar places its
        // swatch. Selecting a preset assigns it to the slot at once.
        PublishFilamentPicker* filament_picker{nullptr};
        wxSizerItem*           filament_picker_item{nullptr};
        // True for a mixed-color filament slot: no Material/Retraction rows; its publish checkbox
        // publishes the slot's gradient/ratio definition as a whole.
        bool is_mixed{false};
        // The file column this category starts on; -1 when the owner has no variant layout. The
        // hosted tab's switch picks the shown column, with this as the fallback.
        int column{-1};
        // A filament slot's file columns, in preset order (empty without a variant layout).
        std::vector<size_t> columns;
        // The Q1 notice: this printer needs a variant the filament does not define.
        wxStaticText* variant_notice{nullptr};

        // Material identity, only for Section::Material categories.
        std::string filament_type;
        std::string filament_vendor;
        std::string filament_id;
        // The author's 0-based filament slot this material section represents.
        size_t filament_slot{0};
        // The slot's preset as the project uses it, and the one the picker last picked. A pick is
        // assigned to the slot at once; `pending_filament` only names the preset the page was
        // rebuilt from.
        std::string filament;
        std::string pending_filament;
        // The colour last picked on the badge: it is written to the project at once and kept here
        // as the value the badge and the payload identity show, until another preset is picked.
        std::vector<std::string> pending_colours;
        bool                     pending_gradient{false};
        std::vector<size_t> rows; // flattened rows of this category
    };

    // Frozen snapshot of a mixed filament slot's definition for the read-only visualization
    // painted on the slot's page. Plain data only: the paint handler must never touch the
    // config. For gradient slots the curve is pre-sampled (t, ratio) pairs, where ratio is the
    // first component's share over model height; anchors carry the raw control points.
    struct MixedVisualSpec
    {
        bool valid{false};
        bool is_gradient{false};
        std::vector<wxColour> component_colours;   // colour per component, in config order
        std::vector<double> ratios;                // sublayer shares summing to ~1 (non-gradient)
        std::vector<double> tri_weights;           // 3-component mixes: barycentric shares
        std::vector<std::pair<double, double>> gradient_samples;
        std::vector<std::pair<double, double>> gradient_anchors;
    };

    // One outer TabCtrl page. Category entries are its inner tabs.
    struct SectionGroup
    {
        wxString title;               // _L("Printer") / _L("Filament") / _L("Process")
        Section kind{Section::Print}; // maps 1:1 to the display group
        std::string icon_name;        // "printer" / "filament" / "process"
        ScalableBitmap icon_bmp;      // tab icon next to the title; rescaled on DPI change
        wxPanel* page{nullptr};
        // Holds every category in order and stays the selection model. Hidden on the Printer and
        // Process sections, which have one hosted page each; there the tab's own strip (pages and
        // extruder switch) is the visible navigation.
        TabCtrl* tabs{nullptr};
        // Second tab strip, below the main one, listing only the mixed-color filament slots.
        // Present on the Material section only (null elsewhere).
        TabCtrl* mixed_tabs{nullptr};
        wxPanel* page_host{nullptr};
        wxBoxSizer* page_host_sizer{nullptr};
        int selected_inner{-1};
        // Selected mixed tab (index into mixed_categories), valid while a mixed slot page is shown.
        int selected_mixed{-1};
        std::vector<size_t> categories;      // indices into m_categories (physical slots)
        std::vector<size_t> mixed_categories; // indices into m_categories (mixed slots)
    };

    void build_option_model();
    // Seed the dialog from a published selection (print/printer keys + per-slot material keys):
    // the supplied selection is authoritative - it is applied after the dirty pre-check and
    // overrides it, so deselected dirty keys stay off. Rows/slots not present in the selection
    // are left unselected. Unknown or out-of-range entries are skipped gracefully.
    void apply_selection(const std::vector<std::string>& published_keys,
                         const std::vector<Slic3r::PublishedMaterialEntry>& material_keys);
    // Frozen snapshot of a mixed slot's definition for the page visualization, resolved from
    // the full config once at dialog-build time. Gradient slots pre-sample exactly what the
    // slicer will print: the custom curve wins over the gradient_range endpoints over the
    // 0.10 -> 0.90 default (the resolution FilamentBitmapUtils::mixed_gradient_curve mirrors).
    static MixedVisualSpec make_mixed_visual_spec(const Slic3r::DynamicPrintConfig& full, size_t slot);
    void apply_filter(const wxString& filter_text);
    // Menu-only pseudo filters: show only the checked ("Filter selected") or only the
    // unchecked ("Filter non-selected") rows. The search box keeps the user's text.
    void apply_pseudo_filter(bool selected_only);
    // Recompute row matches and visibility for the active filter mode. filter is the lowered
    // search text; it is ignored by the pseudo modes.
    void refresh_filter(const wxString& filter);
    void select_all(bool value);
    void select_visible(bool value);
    void show_menu(wxMouseEvent& evt);
    // "Full Publish" toggled: disables/enables the material's rows and its Color/Type checkboxes.
    void on_full_toggle(size_t category_index);
    // A mixed slot's publish checkbox toggled: publishes the definition and auto-selects its
    // component filaments' "Full Publish" toggles. A physical slot has no owner checkbox.
    void on_owner_toggle(size_t category_index);
    // Adds a physical slot's Color and Type rows: one flagged row each, no config keys. Called at
    // build and again whenever the slot's rows are rebuilt from a picked preset.
    void add_owner_flag_rows(size_t category_index);
    // The index of a category's Color or Type row, or size_t(-1) when it has none.
    size_t owner_flag_row(size_t category_index, Slic3r::PublishRowKind kind) const;
    // A Color/Type checkbox toggled: the row owns the flag.
    void on_owner_flag_toggle(size_t category_index, Slic3r::PublishRowKind kind);
    // Pushes the model's Color/Type flags into their checkboxes and greys them under Full Publish.
    void sync_owner_flag_widgets(size_t category_index);
    // Whether a category currently publishes something, driving its tab's indicator dot.
    // Print/Printer: any row checked. Material: the owner publishes under the model's rule.
    bool category_has_selection(const Category& cat) const;
    // Recompute the indicator dot on every outer/inner tab from the current selection state.
    void refresh_tab_indicators();
    // Unmet dependencies of publishing mixed-filament slots, one record per (mix, component)
    // pair: the component's Type row is unchecked and Full Publish is off. Either publishes the
    // component's material. Sorted by mixed slot, then component slot.
    std::vector<MixedDependencyIssue> unpublished_mixed_components() const;
    // Read-only visualization of a mixed slot's definition (a stacked ratio bar, or the
    // Material Ratio vs Model Height graph for a gradient), inserted above the info hint
    // inside the category's scroll area.
    void add_mixed_visual(size_t category_index, const MixedVisualSpec& spec);
    // Rebuilds every mixed slot's definition preview from the current project colours.
    void refresh_mixed_visuals();
    // Return/create the fixed outer page for a Section kind.
    size_t section_group_for(Section kind);
    size_t category_index_for(const wxString& title,
                              Section section,
                              size_t group,
                              size_t source_index,
                              const PublishMaterialIdentity& identity = PublishMaterialIdentity(),
                              bool is_mixed                          = false);
    // Creates the slot's private Filament tab from `pending_filament` and hands it to the
    // category's host (spec 7.4.2). Used once at build and again whenever the picker assigns
    // another preset; the previous tab is destroyed by the caller.
    void build_material_tab(Category& category);
    // Picker callback: assign `preset_name` to the physical slot at once, write the slot colour
    // its preset declares, rebuild the slot's tab and rows from it and refresh the sidebar.
    void on_filament_picked(size_t slot, const std::string& preset_name);
    // Badge callback: open the sidebar's colour picker on the slot's colour and write the choice
    // to the project's slot colour at once.
    void on_filament_color_picked(size_t slot);
    // Repaints the slot's badge and tab-strip chip from the colour the project carries (or the
    // colour last picked on the badge) and carries that colour into the payload identity.
    void refresh_slot_colour(size_t category_index);
    // The page of a physical slot, or nullptr.
    Category* material_category(size_t slot);
    // The colour the slot's badge and tab-strip chip show: the colour last picked on the badge
    // while it is set, the project's colour otherwise. `full` is the project's config.
    std::string staged_badge_color(const Category& category, const Slic3r::DynamicPrintConfig& full) const;
    // Drops a category's rows and re-indexes every other category's rows.
    void remove_category_rows(size_t category_index);
    // Recomputes the file-column layout of every material slot from the project's assigned
    // presets, remaps the rows' "#col" keys and keeps the selection state on the remapped ids.
    void refresh_material_layout();
    // Appends one row drawn by the hosted tab's own Line.
    size_t add_tab_row(Row row, size_t category_index);
    // Builds a physical slot's rows from its hosted Filament tab's lines, and attaches the publish
    // checkbox to each line they came from.
    void add_material_tab_rows(size_t category_index);
    // Builds the hosted Printer page's rows from its tab's lines: one per variant column, keyed
    // "base#column", with the checkbox attached to the shared line.
    void add_printer_tab_rows(size_t category_index);
    // Builds the hosted Process page's rows from its tab's lines: a per-variant line gets one row
    // per extruder column ("base#column"), every other line one row over its whole keys. The
    // checkbox is attached to the shared line.
    void add_print_tab_rows(size_t category_index);
    // The variant column the hosted Printer or Process tab's extruder switch shows, or -1 when
    // unknown.
    int tab_active_column(Tab* tab) const;
    // The row of `line` shown for the variant the tab's switch currently selects, or -1.
    size_t tab_row_for_line(size_t category_index, const Line* line) const;
    // Whether the tab's own rules keep the row's line shown (its hide rules run natively).
    bool tab_line_visible(const Tab& tab, const Row& row) const;
    // Whether a row belongs to the variant currently shown (slot-level rows always do).
    bool row_on_selected_variant(size_t row_index) const;
    // Pushes one row's model state into its line's row toggle, if the widgets exist.
    void sync_row_widgets(size_t row_index);
    void sync_all_row_widgets();
    // Colours the row's label the way the Tab marks a modified setting.
    void apply_row_label_colour(size_t row_index);
    void apply_row_label_colours();
    // The line a row is drawn by.
    Line* row_line(size_t row_index) const;
    // Model-backed row state: the payload and the widgets both read this, never each other.
    void set_row_checked(size_t row_index, bool checked);
    void set_row_enabled(size_t row_index, bool enabled);
    // Whether a row's value can be written to the project (spec 7.10): the slot must not have Full
    // Publish, and the row must resolve to a real write target. Colour/Type rows have no key, so
    // they are never writable; the flag rows do not gate a key row's edit.
    bool row_writable(size_t row_index) const;
    // Enables the line's fields while the row it draws is checked and writable, and disables them
    // otherwise. A row of a column the tab does not show leaves the line alone.
    void apply_line_field_gating(size_t row_index);
    // Applies the gating of every row of the category to the tab's fields.
    void apply_row_field_gating(size_t category_index);
    // Wires a hosted tab's callbacks to this dialog: repaint its rows on a variant or page change
    // and forward each edit to the real preset or project.
    void bind_publish_tab(size_t category_index);
    // One edit from a hosted tab: the row the visible field draws, checked and writable, is
    // written to its owner's real preset (or the project config) at once (spec 7.10).
    void on_publish_edit(size_t category_index, const std::string& opt_id, const boost::any& value);
    // The row of `category_index` whose published option `opt_id` names and whose column the tab
    // currently shows, or size_t(-1) when the field is not a published row.
    size_t row_for_edit(size_t category_index, const std::string& opt_id) const;
    // Re-reads the project after a write that no sidebar Tab refreshed.
    void refresh_published_plater();
    // The non-structural filament keys of a slot's preset, for a "Full Publish" entry.
    std::vector<std::string> full_keys_for_slot() const;
    void save_scroll_position(Category& category);
    void show_outer_page(size_t section_index);
    void show_inner_page(size_t section_index, int inner_index);
    void show_mixed_page(size_t section_index, int mixed_index);
    void on_outer_tab_changed(wxCommandEvent& event);
    void on_inner_tab_changed(size_t section_index, wxCommandEvent& event);
    void on_mixed_tab_changed(size_t section_index, wxCommandEvent& event);
    bool row_is_visible(size_t row_index) const;
    void apply_visibility();
    void bind_tab_events();

    TabCtrl* m_outer_tabs{nullptr};
    wxPanel* m_outer_host{nullptr};
    wxBoxSizer* m_outer_host_sizer{nullptr};
    int m_selected_outer{-1};
    wxBoxSizer* m_fb_sizer{nullptr}; // "All"/"None" buttons sizer
    // Active filter mode: free text from the search box, or one of the menu's pseudo filters.
    enum class FilterMode { Text, SelectedOnly, UnselectedOnly };
    FilterMode m_filter_mode{FilterMode::Text};
    TextInput* m_filter_box{nullptr};
    wxTextCtrl* m_filter_ctrl{nullptr};
    wxStaticBitmap* m_menu_button{nullptr};
    // Shown while a pseudo filter is active (the search box keeps the user's text, so the chip
    // carries the visible state); clicking it returns to text filtering.
    wxStaticText* m_pseudo_chip{nullptr};
    wxString m_info_nonsel;
    wxString m_info_allsel;
    wxString m_info_empty;
    wxString m_info_mix; // body hint shown for a mixed slot (published as a whole)

    ScalableBitmap m_search;
    ScalableBitmap m_menu;
    bool m_legacy_process_selection{false};

    // The coalesced plater refresh: every edit wants the plater to re-read the config, but typing
    // would otherwise refresh once per keystroke. One refresh runs after this timer expires.

    std::vector<Row> m_rows;
    std::vector<Category> m_categories;
    std::vector<SectionGroup> m_sections;
    // One entry per row, in display order, handed to the selection model's conversions.
    std::vector<Slic3r::PublishRowId> m_row_ids;
    // Every publishable value the dialog shows, read once when it opens. Declared before the
    // groups so it outlives them.
    Slic3r::DynamicPrintConfig m_snapshot;
    // The selection state: checked and enabled per row, published and Full Publish per owner, and
    // the owner identity the payload needs. The widgets are a view over it.
    Slic3r::PublishSelection m_selection;
};

}} // namespace Slic3r::GUI
