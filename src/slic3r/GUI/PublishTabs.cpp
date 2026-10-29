#include "PublishTabs.hpp"

#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

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

PublishTabPrint::PublishTabPrint(ParamsPanel *host)
    : TabPrint(host)
    , m_presets_private(Preset::TYPE_PRINT, Preset::print_options(),
                        static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
{
    detach_from_tabs_list();
}

} // GUI
} // Slic3r
