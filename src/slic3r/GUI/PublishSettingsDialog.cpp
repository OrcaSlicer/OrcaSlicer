#include "PublishSettingsDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "Tab.hpp"
#include "ConfigValueFormatter.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PublishSettings.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <set>
#include <algorithm>

namespace Slic3r { namespace GUI {
namespace {

// Menu ids for show_menu(): dedicated range so the popup cannot collide with application-level
// bindings (e.g. MainFrame's recent-files wxID_FILE1.. range).
enum {
    kPublishSelectAll = wxID_HIGHEST + 1,
    kPublishDeselectAll,
    kPublishSelectVisible,
    kPublishDeselectVisible,
    kPublishFilterSelected,
    kPublishFilterNonSelected
};

// Tree-style flags shared by the outer tabs and every group's inner tabs.
constexpr long s_tab_style = wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS |
                             wxTR_FULL_ROW_HIGHLIGHT;

// The author's hex colour for a filament slot (empty when the slot is out of range); shared by
// the header chip, the inner-tab chip and the DPI rescale paths.
std::string filament_color_hex(const DynamicPrintConfig& full, size_t slot)
{
    if (const auto* colours = full.opt<ConfigOptionStrings>("filament_colour"))
        if (slot < colours->size())
            return colours->get_at(slot);
    return std::string();
}

PublishMaterialIdentity material_identity(size_t slot, const DynamicPrintConfig& full)
{
    PublishMaterialIdentity identity;
    if (const auto* types = full.opt<ConfigOptionStrings>("filament_type"))
        if (slot < types->size())
            identity.type = types->get_at(slot);
    if (const auto* vendors = full.opt<ConfigOptionStrings>("filament_vendor"))
        if (slot < vendors->size())
            identity.vendor = vendors->get_at(slot);
    if (const auto* ids = full.opt<ConfigOptionStrings>("filament_ids"))
        if (slot < ids->size())
            identity.id = ids->get_at(slot);
    return identity;
}

// "Generic PLA @System" -> "Generic PLA"; mirrors the alias derivation in PresetBundle.cpp.
std::string material_display_name(const std::string& preset_name)
{
    const size_t at = preset_name.find_first_of('@');
    if (at == std::string::npos)
        return preset_name;
    std::string bare = preset_name.substr(0, at);
    boost::trim_right(bare);
    return bare.empty() ? preset_name : bare;
}

// Section title for a filament slot: the resolved preset name, then the filament type, then
// the generic "Material".
wxString material_title(size_t slot, const PresetBundle* bundle, const DynamicPrintConfig& full)
{
    if (slot < bundle->filament_presets.size()) {
        const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot]);
        if (preset != nullptr && !preset->name.empty())
            return from_u8(material_display_name(preset->name));
    }
    const PublishMaterialIdentity identity = material_identity(slot, full);
    if (!identity.type.empty())
        return from_u8(identity.type);
    return _L("Material");
}

} // namespace

PublishSettingsDialog::PublishSettingsDialog(wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Publish 3MF..."),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_search(this, "search", 16)
    , m_menu(this, "filter", 16)
{
    SetBackgroundColour(*wxWHITE);

    // --- filter bar: search box, All/None, menu button ---
    wxPanel* f_bar = new wxPanel(this, wxID_ANY);
    f_bar->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* f_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_filter_box = new TextInput(f_bar, "", "", "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_filter_box->SetIcon(m_search.bmp());
    m_filter_box->SetMinSize(FromDIP(wxSize(200, 24)));
    m_filter_box->SetSize(FromDIP(wxSize(-1, 24)));
    m_filter_box->SetFocus();
    m_filter_ctrl = m_filter_box->GetTextCtrl();
    m_filter_ctrl->SetFont(Label::Body_13);
    m_filter_ctrl->SetSize(wxSize(-1, FromDIP(16))); // centers text vertically
    m_filter_ctrl->SetHint(_L("Type to filter..."));
    m_filter_ctrl->Bind(wxEVT_TEXT, [this](auto&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_filter_ctrl->Bind(wxEVT_TEXT_ENTER, [this](auto&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_filter_ctrl->Bind(wxEVT_SET_FOCUS, [this](auto& e) {
        apply_filter(m_filter_ctrl->GetValue());
        e.Skip();
    });
    m_filter_ctrl->Bind(wxEVT_KILL_FOCUS, [this](auto& e) {
        apply_filter(m_filter_ctrl->GetValue());
        e.Skip();
    });
    f_sizer->Add(m_filter_box, 1, wxEXPAND);

    m_fb_sizer      = new wxBoxSizer(wxHORIZONTAL);
    auto create_btn = [this, f_bar](wxString title, bool select) {
        auto btn = new wxStaticText(f_bar, wxID_ANY, title);
        btn->SetForegroundColour("#009687");
        btn->SetCursor(wxCURSOR_HAND);
        btn->SetFont(Label::Body_13);
        btn->Bind(wxEVT_LEFT_DOWN, [this, select](wxMouseEvent&) { select_all(select); });
        m_fb_sizer->Add(btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    };
    f_sizer->Add(m_fb_sizer, 0, wxALIGN_CENTER_VERTICAL);
    create_btn(_L("All"), true);
    create_btn(_L("None"), false);

    // Indicator for the menu's pseudo filters: sits where All/None go while they are hidden.
    // Labels reuse the menu entries (no new strings); clicking the chip returns to text
    // filtering, keeping the search box contents.
    m_pseudo_chip = new wxStaticText(f_bar, wxID_ANY, "");
    m_pseudo_chip->SetForegroundColour("#009687");
    m_pseudo_chip->SetCursor(wxCURSOR_HAND);
    m_pseudo_chip->SetFont(Label::Body_13);
    m_pseudo_chip->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_pseudo_chip->Hide();
    f_sizer->Add(m_pseudo_chip, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    m_menu_button = new wxStaticBitmap(f_bar, wxID_ANY, m_menu.bmp());
    m_menu_button->SetCursor(wxCURSOR_HAND);
    m_menu_button->Bind(wxEVT_LEFT_DOWN, &PublishSettingsDialog::show_menu, this);
    f_sizer->Add(m_menu_button, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    f_bar->SetSizerAndFit(f_sizer);

    m_outer_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
    m_outer_tabs->SetFont(Label::Body_14);
    m_outer_tabs->SetBackgroundColour(GetBackgroundColour());

    m_outer_host = new wxPanel(this, wxID_ANY);
    m_outer_host->SetBackgroundColour(GetBackgroundColour());
    m_outer_host_sizer = new wxBoxSizer(wxVERTICAL);
    m_outer_host->SetSizer(m_outer_host_sizer);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY, _L("Select which settings to embed in the 3MF file"));
    msg->SetFont(Label::Body_13);
    msg->Wrap(-1);
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(10));

    w_sizer->Add(f_bar, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_outer_tabs, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_outer_host, 1, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));

    build_option_model();

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // Publish is always allowed: no settings selected means no settings override.
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    w_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizerAndFit(w_sizer);
    SetMinSize(FromDIP(wxSize(600, 500)));
    SetSize(FromDIP(wxSize(600, 500))); // initial size only; the dialog is resizable
    wxGetApp().UpdateDlgDarkUI(this);
}

PublishSettingsDialog::~PublishSettingsDialog() {}

void PublishSettingsDialog::build_option_model()
{
    // Structural / non-publishable keys, shared with the published-3MF overlay path.
    const std::set<std::string>& denylist = publish_structural_keys();
    // Base keys already added in the print/printer sections. Printer rows share this set:
    // per-extruder "#N" variants collapse to the first occurrence (acceptable MVP; the
    // per-extruder context is lost in the UI).
    std::set<std::string> added;

    PresetBundle* bundle    = wxGetApp().preset_bundle;
    DynamicPrintConfig full = bundle->full_config();

    m_info_nonsel = _L("No selected items...");
    m_info_allsel = _L("All items selected...");
    m_info_empty  = _L("No matching items...");

    // Tab order differs from Section's enum order (Print, Printer, Material): the dialog
    // presents Printer, Filament, Process.
    m_sections.reserve(3);
    const Section tab_order[] = {Section::Printer, Section::Material, Section::Print};
    for (Section kind : tab_order)
        section_group_for(kind);
    bind_tab_events();

    // Shared per-option label/value computation; returns false when the option must be skipped
    // (denylisted / unknown / empty label). value is the stringified value; unit the translated
    // sidetext (may be empty).
    auto option_text = [&denylist, &full](const std::string& opt_id, const std::string& pure_key, wxString& label, wxString& value,
                                          wxString& unit) -> bool {
        if (denylist.count(pure_key) > 0)
            return false;
        const ConfigOptionDef* def = print_config_def.get(pure_key);
        if (def == nullptr)
            return false;
        label = _(def->full_label.empty() ? def->label : def->full_label);
        if (label.IsEmpty())
            return false;
        value = get_string_value(opt_id, full);
        unit  = _(def->sidetext);
        return true;
    };

    // --- Phase 1: printer per-extruder retraction settings (first, mirroring the sidebar's
    // Printer group), from the printer tab's "Extruder"/"Extruder N" pages.
    {
        size_t g = section_group_for(Section::Printer);
        category_index_for(_L("Extruder"), Section::Printer, g, 0);
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINTER)
                continue;
            for (const PageShp& page : tab->m_pages) {
                if (!page->title().StartsWith("Extruder"))
                    continue;
                const wxString page_title = Tab::translate_category(page->title(), tab->m_type);
                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    // Allowlist on the untranslated optgroup title; the "Retraction when
                    // switching material" group is intentionally skipped.
                    if (optgroup->title != "Retraction" && optgroup->title != "Z-Hop")
                        continue;
                    const wxString subcategory = _(optgroup->title);
                    for (const auto& opt : optgroup->opt_map()) {
                        const std::string& opt_id   = opt.first;
                        const std::string& pure_key = opt.second.first;
                        // Per-extruder "#N" variants collapse to the first base key. The row
                        // stores the base key; GetPublishedKeys() later expands it back to one
                        // "#N" entry per extruder so the load side can apply per-extruder values.
                        if (!added.insert(pure_key).second)
                            continue;
                        wxString label, value, unit;
                        if (!option_text(opt_id, pure_key, label, value, unit))
                            continue;
                        size_t cat_index = category_index_for(_L("Extruder"), Section::Printer, g, 0);
                        size_t sub_index = subcategory_index_for(cat_index, subcategory, optgroup->icon);
                        add_row_ui(pure_key, label, value, unit, cat_index, sub_index);
                    }
                }
            }
        }
    }

    // --- Phase 2: per-material sections synthesized from the filament tab's "Setting
    // Overrides" page, under the Filament group.
    {
        size_t g          = section_group_for(Section::Material);
        Tab* filament_tab = nullptr;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->m_type == Preset::TYPE_FILAMENT) {
                filament_tab = tab;
                break;
            }
        if (filament_tab != nullptr) {
            const Page* overrides_page = nullptr;
            for (const PageShp& page : filament_tab->m_pages)
                if (page->title() == "Setting Overrides") {
                    overrides_page = page.get();
                    break;
                }

            if (overrides_page != nullptr) {
                // One section per filament slot (a 4-slot printer shows 4 pages), each
                // disambiguated by its colour chip and slot identity while showing the bare name.
                for (size_t slot = 0; slot < bundle->filament_presets.size(); ++slot) {
                    const PublishMaterialIdentity identity = material_identity(slot, full);
                    const wxString title                   = material_title(slot, bundle, full);
                    const size_t category_index            = category_index_for(title, Section::Material, g, slot, identity);

                    // Material requirement rows: an optional filament colour and/or a
                    // vendor-agnostic material type for this slot, in their own optgroup so they
                    // stay visually separated from the setting rows.
                    {
                        const size_t req_sub = subcategory_index_for(category_index, _L("Material"), "custom-gcode_filament");
                        add_row_ui("filament_colour", _L("Color"), from_u8(filament_color_hex(full, slot)), wxString(), category_index,
                                   req_sub, RowKind::Color);
                        std::string type;
                        if (const auto* types = full.opt<ConfigOptionStrings>("filament_type"))
                            if (slot < types->size())
                                type = types->get_at(slot);
                        add_row_ui("filament_type", _L("Type"), from_u8(normalize_filament_type(type)), wxString(), category_index, req_sub,
                                   RowKind::Type);
                    }

                    // A material section must not repeat a key; the same key may appear in other
                    // material sections - that is intended.
                    std::set<std::string> material_added;

                    for (const ConfigOptionsGroupShp& optgroup : overrides_page->m_optgroups) {
                        // Allowlist on the untranslated optgroup title; "Ironing" is skipped.
                        if (optgroup->title != "Retraction" && optgroup->title != "Retraction when switching material")
                            continue;
                        for (const auto& opt : optgroup->opt_map()) {
                            // Row keys are base keys; the load side applies them positionally.
                            const std::string& opt_id = opt.first;
                            std::string base          = opt_id.substr(0, opt_id.find('#'));
                            if (!material_added.insert(base).second)
                                continue;
                            // Show the value of this slot; fall back to slot 0 if out of range.
                            std::string value_opt_id = base + "#" + std::to_string(slot);
                            if (const ConfigOption* opt_cfg = full.option(base))
                                if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt_cfg))
                                    if (vec->size() > 0 && slot >= vec->size())
                                        value_opt_id = base + "#0";
                            wxString label, value, unit;
                            if (!option_text(value_opt_id, base, label, value, unit))
                                continue;
                            size_t sub_index = subcategory_index_for(category_index, _(optgroup->title), optgroup->icon);
                            add_row_ui(base, label, value, unit, category_index, sub_index);
                        }
                    }
                }
            }
        }
    }

    // --- Phase 3: process (print) settings; grouping matches the process tab.
    {
        size_t g = section_group_for(Section::Print);
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINT)
                continue;
            size_t page_index = 0;
            for (const PageShp& page : tab->m_pages) {
                wxString category           = Tab::translate_category(page->title(), tab->m_type);
                const size_t category_index = category_index_for(category, Section::Print, g, page_index);

                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    for (const auto& opt : optgroup->opt_map()) {
                        // opt_map() key is the opt_id (may carry "#N"); the value is
                        // (pure_opt_key, opt_index).
                        const std::string& opt_id   = opt.first;
                        const std::string& pure_key = opt.second.first;
                        // A key may appear in more than one page/group; keep the first.
                        if (!added.insert(pure_key).second)
                            continue;
                        wxString label, value, unit;
                        if (!option_text(opt_id, pure_key, label, value, unit))
                            continue;
                        size_t sub_index = subcategory_index_for(category_index, _(optgroup->title), optgroup->icon);
                        add_row_ui(opt_id, label, value, unit, category_index, sub_index);
                    }
                }
                ++page_index;
            }
        }
    }

    // Pre-check the dirty (modified) settings and mark them bold (base-key match, across all
    // sections; collect_dirty_settings_keys unions the prints, printers and filaments).
    std::set<std::string> dirty_base;
    for (const std::string& key : collect_dirty_settings_keys(*wxGetApp().preset_bundle)) {
        auto n = key.find('#');
        dirty_base.insert(n == std::string::npos ? key : key.substr(0, n));
    }
    for (Row& row : m_rows) {
        // The Color/Type requirement rows are not "dirty overrides": never auto-checked.
        if (row.kind != RowKind::Setting)
            continue;
        std::string base = row.key.substr(0, row.key.find('#'));
        row.dirty        = dirty_base.count(base) > 0;
        if (row.dirty) {
            row.check->SetValue(true);
            set_row_bold(row, true);
        }
    }

    // Wire the "Full Publish" checkboxes: toggling one disables/enables the material's rows.
    // Bind by index so the lambda stays valid even if the vector is reallocated later.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].full_check != nullptr)
            m_categories[c].full_check->Bind(wxEVT_CHECKBOX, [this, c](wxCommandEvent&) { on_full_toggle(c); });

    // No filter is active at startup: every row matches until the user types.
    for (Row& row : m_rows)
        row.matches_filter = true;
    apply_visibility();

    for (Category& category : m_categories) {
        category.scroll->FitInside();
        category.list_sizer->Layout();
    }
    for (SectionGroup& section : m_sections)
        if (!section.categories.empty())
            section.tabs->SelectItem(0);
    if (!m_sections.empty()) {
        m_outer_tabs->SelectItem(0);
        show_outer_page(0);
    }
}

size_t PublishSettingsDialog::section_group_for(Section kind)
{
    for (size_t i = 0; i < m_sections.size(); ++i)
        if (m_sections[i].kind == kind)
            return i;

    SectionGroup section;
    section.kind           = kind;
    const size_t new_index = m_sections.size();
    switch (kind) {
    case Section::Printer:
        section.title     = _L("Printer");
        section.icon_name = "printer";
        break;
    case Section::Material:
        section.title     = _L("Filament");
        section.icon_name = "filament";
        break;
    case Section::Print:
        section.title     = _L("Process");
        section.icon_name = "process";
        break;
    }
    section.icon_bmp = ScalableBitmap(this, section.icon_name, 16);

    section.page = new wxPanel(m_outer_host, wxID_ANY);
    section.page->SetBackgroundColour(GetBackgroundColour());
    auto* page_sizer = new wxBoxSizer(wxVERTICAL);
    section.tabs     = new TabCtrl(section.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
    section.tabs->SetFont(Label::Body_14);
    section.tabs->SetBackgroundColour(GetBackgroundColour());
    page_sizer->Add(section.tabs, 0, wxEXPAND);
    section.page_host = new wxPanel(section.page, wxID_ANY);
    section.page_host->SetBackgroundColour(GetBackgroundColour());
    section.page_host_sizer = new wxBoxSizer(wxVERTICAL);
    section.page_host->SetSizer(section.page_host_sizer);
    page_sizer->Add(section.page_host, 1, wxEXPAND | wxTOP, FromDIP(4));
    section.page->SetSizer(page_sizer);

    if (section.icon_bmp.bmp().IsOk())
        m_outer_tabs->AppendItem(section.title, section.icon_bmp.bmp());
    else
        m_outer_tabs->AppendItem(section.title);
    m_outer_host_sizer->Add(section.page, 1, wxEXPAND);
    section.page->Hide();
    m_sections.push_back(std::move(section));
    return new_index;
}

size_t PublishSettingsDialog::category_index_for(
    const wxString& title, Section section, size_t group, size_t source_index, const PublishMaterialIdentity& identity)
{
    for (size_t i : m_sections[group].categories) {
        Category& existing = m_categories[i];
        if (existing.title == title && existing.section == section && existing.source_index == source_index &&
            existing.filament_id == identity.id && existing.filament_type == identity.type && existing.filament_vendor == identity.vendor)
            return i;
    }

    Category category;
    category.title           = title;
    category.section         = section;
    category.group           = group;
    category.source_index    = source_index;
    category.filament_type   = identity.type;
    category.filament_vendor = identity.vendor;
    category.filament_id     = identity.id;
    category.filament_slot   = source_index;
    category.page            = new wxPanel(m_sections[group].page_host, wxID_ANY);
    category.page->SetBackgroundColour(GetBackgroundColour());
    auto* page_sizer = new wxBoxSizer(wxVERTICAL);

    // The slot's colour chip decorates both the section header and the inner tab.
    std::string hex;
    if (section == Section::Material)
        hex = filament_color_hex(wxGetApp().preset_bundle->full_config(), source_index);

    if (section == Section::Material) {
        auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);
        if (wxBitmap* chip = get_extruder_color_icon(hex, "", FromDIP(12), FromDIP(12))) {
            category.filament_color_chip = new wxStaticBitmap(category.page, wxID_ANY, *chip);
            header_sizer->Add(category.filament_color_chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        }
        category.title_label = new wxStaticText(category.page, wxID_ANY, title);
        category.title_label->SetFont(Label::Head_14);
        header_sizer->Add(category.title_label, 0, wxALIGN_CENTER_VERTICAL);
        category.full_check = new wxCheckBox(category.page, wxID_ANY, _L("Full Publish"));
        category.full_check->SetFont(Label::Body_13);
        category.full_check->SetToolTip(_L("Embed the entire filament of this slot in the 3MF file"));
        header_sizer->Add(category.full_check, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        page_sizer->Add(header_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));
    }

    category.scroll = new wxScrolledWindow(category.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    category.scroll->SetScrollRate(0, 10);
    category.scroll->SetBackgroundColour(GetBackgroundColour());
    category.list_sizer = new wxBoxSizer(wxVERTICAL);
    category.scroll->SetSizer(category.list_sizer);
    category.scroll->DisableFocusFromKeyboard();
    category.scroll->Bind(wxEVT_RIGHT_DOWN, &PublishSettingsDialog::show_menu, this);
    category.info = new wxStaticText(category.scroll, wxID_ANY, m_info_empty);
    category.info->SetFont(Label::Body_13);
    category.list_sizer->Add(category.info, 1, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(10));
    category.info->Hide();
    page_sizer->Add(category.scroll, 1, wxEXPAND | wxALL, FromDIP(4));
    category.page->SetSizer(page_sizer);
    category.page->Hide();

    const size_t category_index = m_categories.size();
    m_categories.push_back(std::move(category));
    m_sections[group].categories.push_back(category_index);
    if (section == Section::Material) {
        if (wxBitmap* chip = get_extruder_color_icon(hex, "", FromDIP(12), FromDIP(12)))
            m_sections[group].tabs->AppendItem(title, *chip);
        else
            m_sections[group].tabs->AppendItem(title);
    } else {
        m_sections[group].tabs->AppendItem(title);
    }
    m_sections[group].page_host_sizer->Add(m_categories[category_index].page, 1, wxEXPAND);
    if (m_sections[group].selected_inner < 0)
        m_sections[group].selected_inner = 0;
    return category_index;
}

size_t PublishSettingsDialog::subcategory_index_for(size_t category_index, const wxString& title, const wxString& icon)
{
    Category& category = m_categories[category_index];
    for (size_t i = 0; i < category.subs.size(); ++i)
        if (category.subs[i].title == title)
            return i;

    Subcategory sub;
    sub.title = title;
    if (!title.IsEmpty()) {
        sub.header = new ::StaticLine(category.scroll, false, title, icon);
        sub.header->SetFont(Label::Head_14);
        sub.header->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        auto* wrap = new wxBoxSizer(wxVERTICAL);
        wrap->Add(sub.header, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
        sub.item = category.list_sizer->Add(wrap, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(22));
    }
    category.subs.push_back(std::move(sub));
    return category.subs.size() - 1;
}

void PublishSettingsDialog::add_row_ui(const std::string& key,
                                       const wxString& label,
                                       const wxString& value,
                                       const wxString& unit,
                                       size_t category_index,
                                       size_t subcategory_index,
                                       RowKind kind)
{
    Category& category = m_categories[category_index];
    Row row;
    row.key                = key;
    row.label              = label;
    row.value              = value;
    row.unit               = unit;
    row.kind               = kind;
    row.category           = category.title;
    row.subcategory        = category.subs[subcategory_index].title;
    row.section            = category.section;
    row.section_title      = m_sections[category.group].title;
    row.outer_index        = category.group;
    row.inner_index        = category_index;
    const size_t row_index = m_rows.size();
    m_rows.push_back(std::move(row));
    Row& current  = m_rows[row_index];
    current.check = new wxCheckBox(category.scroll, wxID_ANY, label);
    current.check->SetFont(Label::Body_13);
    auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
    row_sizer->Add(current.check, 0, wxALIGN_CENTER_VERTICAL);
    // The value is read-only text (incl. the Type row: the published type is the slot's
    // normalized type, not author-editable).
    current.value_label = new wxStaticText(category.scroll, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    current.value_label->SetFont(Label::Body_13);
    current.value_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    current.value_label->SetToolTip(unit.IsEmpty() ? value : value + " " + unit);
    if (kind == RowKind::Color && !value.IsEmpty()) {
        if (wxBitmap* chip = get_extruder_color_icon(value.ToStdString(), "", FromDIP(12), FromDIP(12))) {
            current.color_chip = new wxStaticBitmap(category.scroll, wxID_ANY, *chip);
            row_sizer->Add(current.color_chip, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        }
    }
    row_sizer->Add(current.value_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    if (!unit.IsEmpty()) {
        current.unit_label = new wxStaticText(category.scroll, wxID_ANY, unit);
        current.unit_label->SetFont(Label::Body_13);
        current.unit_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        row_sizer->Add(current.unit_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    }
    current.item = category.list_sizer->Add(row_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
    category.rows.push_back(row_index);
    category.subs[subcategory_index].rows.push_back(row_index);
}

void PublishSettingsDialog::on_full_toggle(size_t category_index)
{
    Category& cat   = m_categories[category_index];
    const bool full = cat.full_check->GetValue();
    for (size_t r : cat.rows)
        m_rows[r].check->Enable(!full);
}

void PublishSettingsDialog::set_row_bold(Row& row, bool bold)
{
    // Rebase on the dialog's body font so clearing bold restores the exact original font.
    row.check->SetFont(bold ? Label::Body_13.Bold() : Label::Body_13);
}

void PublishSettingsDialog::save_scroll_position(Category& category)
{
    if (category.scroll != nullptr)
        category.scroll->GetViewStart(&category.scroll_pos.x, &category.scroll_pos.y);
}

void PublishSettingsDialog::show_outer_page(size_t section_index)
{
    if (section_index >= m_sections.size())
        return;
    if (m_selected_outer >= 0 && m_selected_outer < static_cast<int>(m_sections.size())) {
        SectionGroup& old_section = m_sections[m_selected_outer];
        if (old_section.selected_inner >= 0 && old_section.selected_inner < static_cast<int>(old_section.categories.size()))
            save_scroll_position(m_categories[old_section.categories[old_section.selected_inner]]);
        m_sections[m_selected_outer].page->Hide();
    }
    m_selected_outer      = static_cast<int>(section_index);
    SectionGroup& section = m_sections[section_index];
    section.page->Show();
    if (section.selected_inner >= 0)
        show_inner_page(section_index, section.selected_inner);
    m_outer_host_sizer->Layout();
}

void PublishSettingsDialog::show_inner_page(size_t section_index, int inner_index)
{
    if (section_index >= m_sections.size())
        return;
    SectionGroup& section = m_sections[section_index];
    if (inner_index < 0 || inner_index >= static_cast<int>(section.categories.size()))
        return;
    if (section.selected_inner >= 0 && section.selected_inner < static_cast<int>(section.categories.size())) {
        save_scroll_position(m_categories[section.categories[section.selected_inner]]);
        m_categories[section.categories[section.selected_inner]].page->Hide();
    }
    section.selected_inner = inner_index;
    Category& category     = m_categories[section.categories[inner_index]];
    category.page->Show();
    category.scroll->FitInside();
    category.scroll->Scroll(category.scroll_pos.x, category.scroll_pos.y);
    section.page_host_sizer->Layout();
}

void PublishSettingsDialog::on_outer_tab_changed(wxCommandEvent& event)
{
    const int selection = event.GetInt();
    if (selection >= 0 && selection < static_cast<int>(m_sections.size()))
        show_outer_page(static_cast<size_t>(selection));
}

void PublishSettingsDialog::on_inner_tab_changed(size_t section_index, wxCommandEvent& event)
{
    const int selection = event.GetInt();
    if (section_index < m_sections.size() && selection >= 0 && selection < static_cast<int>(m_sections[section_index].categories.size()))
        show_inner_page(section_index, selection);
}

void PublishSettingsDialog::bind_tab_events()
{
    m_outer_tabs->Bind(wxEVT_TAB_SEL_CHANGED, &PublishSettingsDialog::on_outer_tab_changed, this);
    for (size_t section_index = 0; section_index < m_sections.size(); ++section_index)
        m_sections[section_index].tabs->Bind(wxEVT_TAB_SEL_CHANGED,
                                             [this, section_index](wxCommandEvent& event) { on_inner_tab_changed(section_index, event); });
}

void PublishSettingsDialog::apply_filter(const wxString& filter_text)
{
    m_filter_mode = FilterMode::Text;
    refresh_filter(filter_text.Lower());
}

void PublishSettingsDialog::apply_pseudo_filter(bool selected_only)
{
    m_filter_mode = selected_only ? FilterMode::SelectedOnly : FilterMode::UnselectedOnly;
    refresh_filter(wxString()); // the pseudo modes ignore the search text
}

void PublishSettingsDialog::refresh_filter(const wxString& filter)
{
    Freeze();
    const bool pseudo       = m_filter_mode != FilterMode::Text;
    const bool want_checked = m_filter_mode == FilterMode::SelectedOnly;
    m_fb_sizer->Show(!pseudo);
    if (pseudo) {
        // "×" marks the chip as a dismissible filter state (U+00D7, present in all UI fonts).
        m_pseudo_chip->SetLabel((want_checked ? _L("Filter selected") : _L("Filter non-selected")) + " " + wxString::FromUTF8("\u00d7"));
    }
    m_pseudo_chip->Show(pseudo);

    // Row matches are computed first; page and optgroup visibility is applied below.
    if (pseudo) {
        for (Row& row : m_rows)
            row.matches_filter = row.check->IsEnabled() && row.check->GetValue() == want_checked;
    } else {
        const bool clear = filter.IsEmpty();
        for (Row& row : m_rows) {
            row.matches_filter = clear || (row.section_title + " " + row.category + " " + row.subcategory + " " + row.label + " " +
                                           row.value + " " + row.unit)
                                              .Lower()
                                              .Contains(filter);
        }
    }

    size_t first_outer    = 0;
    int first_inner       = -1;
    bool active_has_match = false;
    for (size_t s = 0; s < m_sections.size(); ++s) {
        for (size_t inner = 0; inner < m_sections[s].categories.size(); ++inner) {
            Category& category = m_categories[m_sections[s].categories[inner]];
            bool has_match     = false;
            for (size_t r : category.rows)
                has_match = has_match || m_rows[r].matches_filter;
            category.info->Show(!has_match);
            if (!has_match)
                category.info->SetLabel(pseudo ? (want_checked ? m_info_nonsel : m_info_allsel) : m_info_empty);
            if (has_match && first_inner < 0) {
                first_outer = s;
                first_inner = static_cast<int>(inner);
            }
            if (static_cast<int>(s) == m_selected_outer && static_cast<int>(inner) == m_sections[s].selected_inner)
                active_has_match = has_match;
        }
    }
    if (!active_has_match && first_inner >= 0 &&
        (m_selected_outer != static_cast<int>(first_outer) || m_sections[first_outer].selected_inner != first_inner)) {
        if (m_selected_outer != static_cast<int>(first_outer)) {
            m_outer_tabs->SelectItem(static_cast<int>(first_outer));
            show_outer_page(first_outer);
        }
        m_sections[first_outer].tabs->SelectItem(first_inner);
        show_inner_page(first_outer, first_inner);
    }

    apply_visibility();
    for (Category& category : m_categories) {
        save_scroll_position(category);
        category.scroll->FitInside();
        category.list_sizer->Layout();
        category.scroll->Scroll(category.scroll_pos.x, category.scroll_pos.y);
    }
    Layout();
    Thaw();
}

void PublishSettingsDialog::apply_visibility()
{
    Freeze();
    for (Category& category : m_categories) {
        bool category_any = false;
        for (size_t r : category.rows)
            category_any = category_any || m_rows[r].matches_filter;
        category.info->Show(!category_any);
        for (Subcategory& sub : category.subs) {
            bool sub_any = false;
            for (size_t r : sub.rows)
                sub_any = sub_any || m_rows[r].matches_filter;
            if (sub.header != nullptr)
                sub.item->Show(sub_any);
            for (size_t r : sub.rows)
                m_rows[r].item->Show(m_rows[r].matches_filter);
        }
        category.list_sizer->Layout();
        category.scroll->FitInside();
    }
    Thaw();
}

void PublishSettingsDialog::select_all(bool value)
{
    // "All" skips disabled (gated) rows; "None" leaves a gated row's preserved value.
    for (Row& row : m_rows)
        if (row.check->IsEnabled())
            row.check->SetValue(value);
}

bool PublishSettingsDialog::row_is_visible(const Row& row) const
{
    if (m_selected_outer < 0 || m_selected_outer >= static_cast<int>(m_sections.size()) ||
        row.outer_index != static_cast<size_t>(m_selected_outer) || m_sections[m_selected_outer].selected_inner < 0 ||
        m_sections[m_selected_outer].selected_inner >= static_cast<int>(m_sections[m_selected_outer].categories.size()) ||
        row.inner_index != m_sections[m_selected_outer].categories[m_sections[m_selected_outer].selected_inner] || !row.matches_filter ||
        !row.check->IsEnabled())
        return false;
    return row.item->IsShown();
}

void PublishSettingsDialog::select_visible(bool value)
{
    // In a pseudo-filter view the rows being toggled would all disappear; drop the filter
    // afterwards so the result stays visible.
    const bool clear_pseudo = (m_filter_mode == FilterMode::UnselectedOnly && !value) ||
                              (m_filter_mode == FilterMode::SelectedOnly && value);

    // Toggle the rows visible under the *current* filter.
    for (Row& row : m_rows)
        if (row_is_visible(row))
            row.check->SetValue(value);

    if (clear_pseudo) {
        // Note: SetValue() may fire wxEVT_TEXT on some platforms, re-entering apply_filter() -
        // that is fine; the rows above were already toggled and the trailing call is idempotent.
        m_filter_ctrl->ChangeValue("");
        apply_filter(""); // resync visibility and the All/None bar
    }
}

void PublishSettingsDialog::show_menu(wxMouseEvent& evt)
{
    bool filtering  = !m_filter_ctrl->GetValue().IsEmpty() || m_filter_mode != FilterMode::Text;
    bool list_empty = true;
    if (m_selected_outer >= 0) {
        for (const Row& row : m_rows)
            if (row_is_visible(row)) {
                list_empty = false;
                break;
            }
    }

    wxMenu m;
    m.Append(kPublishSelectAll, _L("Select All"))->Enable(!filtering);
    m.Append(kPublishDeselectAll, _L("Deselect All"))->Enable(!filtering);
    m.AppendSeparator();
    m.Append(kPublishSelectVisible, _L("Select visible"))->Enable(!list_empty && filtering);
    m.Append(kPublishDeselectVisible, _L("Deselect visible"))->Enable(!list_empty && filtering);
    m.AppendSeparator();
    m.AppendCheckItem(kPublishFilterSelected, _L("Filter selected"));
    m.AppendCheckItem(kPublishFilterNonSelected, _L("Filter non-selected"));
    m.Check(kPublishFilterSelected, m_filter_mode == FilterMode::SelectedOnly);
    m.Check(kPublishFilterNonSelected, m_filter_mode == FilterMode::UnselectedOnly);

    m.Bind(
        wxEVT_MENU,
        [this](wxCommandEvent& e) {
            switch (e.GetId()) {
            case kPublishSelectAll: select_all(true); break;
            case kPublishDeselectAll: select_all(false); break;
            case kPublishSelectVisible: select_visible(true); break;
            case kPublishDeselectVisible: select_visible(false); break;
            case kPublishFilterSelected:
                // Clicking the active entry again clears the pseudo filter.
                if (m_filter_mode == FilterMode::SelectedOnly)
                    apply_filter(m_filter_ctrl->GetValue());
                else
                    apply_pseudo_filter(true);
                break;
            case kPublishFilterNonSelected:
                if (m_filter_mode == FilterMode::UnselectedOnly)
                    apply_filter(m_filter_ctrl->GetValue());
                else
                    apply_pseudo_filter(false);
                break;
            default: break;
            }
        },
        kPublishSelectAll, kPublishFilterNonSelected);

    wxWindow* src = dynamic_cast<wxWindow*>(evt.GetEventObject());
    if (!src)
        return;
    wxPoint screen_pos = src->ClientToScreen(evt.GetPosition());
    wxPoint local_pos  = ScreenToClient(screen_pos);
    PopupMenu(&m, local_pos);
}

std::vector<std::string> PublishSettingsDialog::GetPublishedKeys() const
{
    std::vector<std::string> out;
    // Process and printer sections both travel through published_keys (the load-side overlay
    // applies process keys to the prints edited preset and the allowlisted printer keys to the
    // printers edited preset); material keys use a separate API.
    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
    for (const Row& row : m_rows) {
        if ((row.section != Section::Print && row.section != Section::Printer) || !row.check->GetValue())
            continue;
        if (row.section == Section::Printer) {
            // Printer rows store the base key (per-extruder "#N" variants collapsed during
            // build). Publish every extruder element so the load side can apply per-extruder
            // values even when the receiver has a different extruder count; a scalar printer
            // key is published as-is.
            const std::string base_key = row.key.substr(0, row.key.find('#'));
            if (const ConfigOption* opt = full.option(base_key)) {
                if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt)) {
                    for (size_t i = 0; i < vec->size(); ++i)
                        out.push_back(base_key + "#" + std::to_string(i));
                } else {
                    out.push_back(base_key);
                }
            }
        } else {
            out.push_back(row.key);
        }
    }
    return out;
}

std::vector<Slic3r::PublishedMaterialEntry> PublishSettingsDialog::GetPublishedMaterialKeys() const
{
    std::vector<Slic3r::PublishedMaterialEntry> out;
    for (const Category& cat : m_categories) {
        if (cat.section != Section::Material)
            continue;
        Slic3r::PublishedMaterialEntry entry;
        entry.filament_type   = cat.filament_type;
        entry.filament_vendor = cat.filament_vendor;
        entry.filament_id     = cat.filament_id;
        entry.slot            = static_cast<int>(cat.filament_slot);
        // The author's preset id distinguishes exact variants that share filament_id
        // ("Generic PLA" vs "Generic PLA Matte"), so the receiver can match precisely; the
        // preset name is the most direct identity and is matched first on load.
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle != nullptr && cat.filament_slot < bundle->filament_presets.size()) {
            if (const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[cat.filament_slot], false, true)) {
                entry.setting_id  = preset->setting_id;
                entry.preset_name = preset->name;
            }
        }
        // "Full Publish": the whole filament preset is embedded; type and colour are implicitly
        // published, and the per-key rows are disabled / their state ignored.
        if (cat.full_check != nullptr && cat.full_check->GetValue()) {
            entry.full               = true;
            entry.full_keys          = full_keys_for_slot();
            entry.publish_type       = true;
            entry.publish_type_value = normalize_filament_type(cat.filament_type);
            for (size_t r : cat.rows) {
                const Row& row = m_rows[r];
                if (row.kind == RowKind::Color && !row.value.IsEmpty()) {
                    entry.publish_color = true;
                    entry.color         = row.value.ToStdString();
                }
            }
            out.push_back(std::move(entry));
            continue;
        }
        for (size_t r : cat.rows) {
            const Row& row = m_rows[r];
            if (!row.check->GetValue())
                continue;
            if (row.kind == RowKind::Color) {
                entry.publish_color = true;
                entry.color         = row.value.ToStdString();
            } else if (row.kind == RowKind::Type) {
                entry.publish_type       = true;
                entry.publish_type_value = row.value.ToStdString();
            } else {
                entry.keys.push_back(row.key);
            }
        }
        // Nothing checked at all -> nothing to write.
        if (!entry.keys.empty() || entry.publish_type || entry.publish_color)
            out.push_back(std::move(entry));
    }
    return out;
}

std::vector<std::string> PublishSettingsDialog::full_keys_for_slot() const
{
    // The canonical filament preset keys minus the structural keys the published overlay must
    // never touch (inherits, compatibility, *_settings_id, ...), plus filament_colour (not a
    // member of Preset::filament_options). Values travel in the exported config, masked to
    // this slot, and are applied on load onto the receiver's slot.
    const std::set<std::string>& denylist = publish_structural_keys();
    std::vector<std::string> keys;
    for (const std::string& key : Preset::filament_options())
        if (denylist.count(key) == 0)
            keys.emplace_back(key);
    keys.emplace_back("filament_colour");
    return keys;
}

void PublishSettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    // Rescale toolbar bitmaps and icons; collapse chevrons are vector-drawn and repaint.
    m_search.msw_rescale();
    m_menu.msw_rescale();
    m_filter_box->SetIcon(m_search.bmp());
    m_menu_button->SetBitmap(m_menu.bmp());
    m_outer_tabs->Rescale();

    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();

    for (Category& cat : m_categories) {
        if (cat.full_check != nullptr)
            cat.full_check->Refresh();
        if (cat.title_label != nullptr)
            cat.title_label->Refresh();
        if (cat.filament_color_chip != nullptr) {
            if (wxBitmap* chip = get_extruder_color_icon(filament_color_hex(full, cat.filament_slot), "", FromDIP(12), FromDIP(12)))
                cat.filament_color_chip->SetBitmap(*chip);
        }
        cat.scroll->FitInside();
        cat.list_sizer->Layout();
    }

    for (size_t s = 0; s < m_sections.size(); ++s) {
        SectionGroup& section = m_sections[s];
        section.icon_bmp.msw_rescale();
        if (section.icon_bmp.bmp().IsOk())
            m_outer_tabs->SetItemBitmap(s, section.icon_bmp.bmp());
        section.tabs->Rescale();
    }

    // Refresh the per-row Color chips at the new DPI.
    for (Row& row : m_rows) {
        if (row.color_chip != nullptr && !row.value.IsEmpty()) {
            if (wxBitmap* chip = get_extruder_color_icon(row.value.ToStdString(), "", FromDIP(12), FromDIP(12)))
                row.color_chip->SetBitmap(*chip);
        }
    }

    for (size_t category_index = 0; category_index < m_categories.size(); ++category_index) {
        const Category& category = m_categories[category_index];
        if (category.section != Section::Material)
            continue;
        if (wxBitmap* chip = get_extruder_color_icon(filament_color_hex(full, category.filament_slot), "", FromDIP(12), FromDIP(12))) {
            const SectionGroup& section = m_sections[category.group];
            const auto iter             = std::find(section.categories.begin(), section.categories.end(), category_index);
            if (iter != section.categories.end())
                m_sections[category.group].tabs->SetItemBitmap(static_cast<unsigned int>(iter - section.categories.begin()), *chip);
        }
    }

    SetMinSize(FromDIP(wxSize(600, 500)));
    Refresh();
}

}} // namespace Slic3r::GUI
