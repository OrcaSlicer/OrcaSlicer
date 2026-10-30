#include "PublishTabs.hpp"

#include <functional>

#include "OptionsGroup.hpp"
#include "ParamsPanel.hpp"
#include "Tab.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PublishSettings.hpp"

#include <algorithm>
#include <set>
#include <wx/defs.h>
#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

namespace {

// Drops the options a predicate rejects, then the groups they leave empty, then hides a page with no
// group left. The same shape the printer allowlist prune uses.
void prune_publish_pages(std::vector<PageShp> &pages, const std::function<bool(const std::string &)> &drop)
{
    for (const PageShp &page : pages) {
        for (const ConfigOptionsGroupShp &group : page->m_optgroups)
            group->remove_option_if(drop);
        page->m_optgroups.erase(std::remove_if(page->m_optgroups.begin(), page->m_optgroups.end(),
                                               [](const ConfigOptionsGroupShp &group) { return group->get_lines().empty(); }),
                                page->m_optgroups.end());
        page->update_visibility(comDevelop, false);
    }
}

bool is_action_option(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    return def != nullptr && publish_is_action_control(*def);
}

} // namespace

PublishParamsPanel::PublishParamsPanel(wxWindow *parent)
    : ParamsPanel(parent)
{
}

PublishParamsPanel::~PublishParamsPanel()
{
    // ~ParamsPanel() clears the current tab's pages, and the page view must still exist while
    // their Page/OptionsGroup destructors run. Run that teardown here, before dropping the
    // pointer, so it is not skipped: the tab is a child of this panel and dies with it.
    if (m_publish_tab != nullptr)
        m_publish_tab->clear_pages();
    m_current_tab = nullptr;
}

void PublishParamsPanel::set_publish_tab(Tab *tab)
{
    if (m_publish_tab == tab)
        return;

    if (m_left_sizer == nullptr)
        create_layout();

    if (m_publish_tab != nullptr) {
        // The page view is shared: run the sidebar's teardown on the outgoing tab so two tabs
        // cannot stack their rows in it, and so a later re-show rebuilds its pages.
        m_publish_tab->Hide();
        m_left_sizer->Detach(m_publish_tab);
        m_publish_tab->clear_pages();
        m_publish_tab->unselect_tree_item();
        m_current_tab = nullptr;
        m_publish_tab = nullptr;
    }

    if (tab == nullptr) {
        m_left_sizer->Layout();
        Layout();
        return;
    }

    m_publish_tab = tab;

    // Above the page view, as the sidebar places its tabs.
    m_left_sizer->Insert(0, m_publish_tab, 0, wxEXPAND);
    m_current_tab = m_publish_tab;
    update_mode();
    m_left_sizer->Layout();
    Layout();
    m_publish_tab->restore_last_select_item();
}

void PublishTabFilament::build()
{
    TabFilament::build();
    // A control that performs an action has no value to publish per key: drop it rather than show a
    // row the dialog cannot check.
    prune_publish_pages(m_pages, is_action_option);
    // The model keys and free-text notes are not published settings: the dialog has no rows for
    // them, so the pages would only show an empty or uncheckable section (spec 7.4.4).
    m_pages.erase(std::remove_if(m_pages.begin(), m_pages.end(), [](const PageShp &page) {
                      return page->title() == "Dependencies" || page->title() == "Notes";
                  }),
                  m_pages.end());
}

PublishTabFilament::PublishTabFilament(ParamsPanel *host)
    : TabFilament(host)
    , m_presets_private(Preset::TYPE_FILAMENT, Preset::filament_options(),
                        static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()),
                        PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER)
{
    detach_from_tabs_list();
}

PublishTabPrinter::PublishTabPrinter(ParamsPanel *host)
    : TabPrinter(host)
    , m_presets_private(Preset::TYPE_PRINTER, Preset::printer_options(),
                        static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()),
                        "Default Printer")
{
    detach_from_tabs_list();
}

void PublishTabPrinter::build()
{
    TabPrinter::build();
    init_options_list();

    // Keep only what the dialog publishes, in the pages' original order. The allowlist is the
    // printer tab's Extruder page groups (Retraction, Z-Hop), so every other group empties out.
    // The pages stay in m_pages: the Tab addresses them in place (update_fff() only toggles the
    // surviving options; it does not rebuild pages), and remove_option_if() prunes lines without
    // touching the group's m_options. A page with no group left reports itself hidden, so
    // rebuild_page_tree() drops it from the page strip.
    const std::set<std::string> &allow = publishable_printer_keys();
    auto is_pruned = [&allow](const std::string &base_key) {
        return allow.count(base_key) == 0 || is_action_option(base_key);
    };
    for (const PageShp &page : m_pages) {
        for (const ConfigOptionsGroupShp &group : page->m_optgroups)
            group->remove_option_if(is_pruned);
        page->m_optgroups.erase(std::remove_if(page->m_optgroups.begin(), page->m_optgroups.end(),
                                               [](const ConfigOptionsGroupShp &group) {
                                                   return group->get_lines().empty();
                                               }),
                                page->m_optgroups.end());
        page->update_visibility(comDevelop, false);
    }
}

void PublishTabPrint::build()
{
    TabPrint::build();
    // The note is free text and the bed type is project state the published import deliberately
    // leaves to the receiver, so neither is a setting this file can carry. Everything else on the
    // process pages is a print preset value.
    prune_publish_pages(m_pages, [](const std::string &key) {
        return is_action_option(key) || key == "notes" || key == "curr_bed_type";
    });
}

PublishTabPrint::PublishTabPrint(ParamsPanel *host)
    : TabPrint(host)
    , m_presets_private(Preset::TYPE_PRINT, Preset::print_options(),
                        static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
{
    detach_from_tabs_list();
}

} // GUI
} // Slic3r
