#include "PublishTabs.hpp"

#include <functional>

#include "OptionsGroup.hpp"
#include "ParamsPanel.hpp"
#include "Tab.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PublishSettings.hpp"

#include <algorithm>
#include <set>
#include <vector>
#include <string>
#include <wx/defs.h>
#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

namespace {

// Drops the options a predicate rejects, then the groups they leave empty, then hides a page with no
// group left. Every publish tab prunes its page set through this.
void prune_publish_pages(std::vector<PageShp> &pages, const std::function<bool(const std::string &)> &drop)
{
    for (const PageShp &page : pages) {
        for (const ConfigOptionsGroupShp &group : page->m_optgroups) {
            group->remove_option_if(drop);
            // remove_option_if() marks every survivor as an object override; a pruned row must keep
            // drawing the revert-to-saved icon, not the system lock.
            group->clear_undo_to_sys();
        }
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

PublishParamsPanel::~PublishParamsPanel()
{
    // This is the teardown the publish tabs' invariant depends on (see PublishTabs.hpp): the tab's
    // own PresetCollection is a member of its PublishTabWithPresets base, so it dies before ~Tab,
    // and every Page::m_config points into it. Clear the pages here, while the page view and the
    // host still exist, rather than leaving it to ~Tab - whose clear_pages() would reach a host
    // that is already gone. ~ParamsPanel() clears the *current* tab's pages, which is why
    // m_current_tab is nulled after.
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
    : PublishTabWithPresets(Preset::TYPE_FILAMENT, Preset::filament_options(),
                            PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER, host, false)
{
}

PublishTabPrinter::PublishTabPrinter(ParamsPanel *host)
    : PublishTabWithPresets(Preset::TYPE_PRINTER, Preset::printer_options(), "Default Printer", host, false)
{
}

void PublishTabPrinter::build()
{
    TabPrinter::build();
    init_options_list();

    // Keep only what the dialog publishes, in the pages' original order. The allowlist is the
    // printer tab's Extruder page groups (Retraction, Z-Hop), so every other group empties out.
    // The pages stay in m_pages: the Tab addresses them in place (update_fff() only toggles the
    // surviving options; it does not rebuild pages), and remove_option_if() prunes each group's
    // options. A page with no group left reports itself hidden, so rebuild_page_tree() drops it
    // from the page strip.
    const std::set<std::string> &allow = publishable_printer_keys();
    prune_publish_pages(m_pages, [&allow](const std::string &base_key) {
        return allow.count(base_key) == 0 || is_action_option(base_key);
    });
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
    : PublishTabWithPresets(Preset::TYPE_PRINT, Preset::print_options(), "Default Setting", host, Preset::TYPE_PRINT, false)
{
}

} // GUI
} // Slic3r
