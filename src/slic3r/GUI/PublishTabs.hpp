#ifndef slic3r_GUI_PublishTabs_hpp_
#define slic3r_GUI_PublishTabs_hpp_

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "ParamsPanel.hpp"
#include "Tab.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

// A bare ParamsPanel for the Publish 3MF dialog: builds the page view without the sidebar's
// top panel, mode switch or tab scan, and hosts one publish tab.
class PublishParamsPanel : public ParamsPanel
{
public:
    using ParamsPanel::ParamsPanel;
    ~PublishParamsPanel();

    // Lays out `tab` and activates its first page. Call after Tab::create_publish_tab().
    // Passing nullptr (or another tab) detaches and clears the previous one.
    void set_publish_tab(Tab *tab);

private:
    Tab *m_publish_tab{nullptr};
};

// A settings Tab over a dialog-owned PresetCollection holding only the default preset of the tab's
// own type, over that type's option keys. Construct, then call create_publish_tab(seed_config)
// before handing the tab to PublishParamsPanel.
//
// Teardown invariant, for all three subclasses below: the pages have to be cleared while
// m_presets_private is still alive. Every Page::m_config points into it, and because it is a member
// of this base it dies before ~Tab runs, so the destructors cannot do the clearing themselves - by
// then clear_pages() would reach a host that has already been destroyed. PublishParamsPanel
// therefore clears the pages before it lets go of the tab, in ~PublishParamsPanel and in
// set_publish_tab(). A new owner of a publish tab has to do the same.
template <class TabBase>
class PublishTabWithPresets : public TabBase
{
public:
    // `keys` and `default_name` describe the collection; `tab_args` are TabBase's own.
    template <class... TabArgs>
    PublishTabWithPresets(Preset::Type type, const std::vector<std::string> &keys, const std::string &default_name, TabArgs &&...tab_args)
        : TabBase(std::forward<TabArgs>(tab_args)...)
        , m_presets_private(type, keys, static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), default_name)
    {
    }

protected:
    PresetCollection *publish_presets() override { return &m_presets_private; }

private:
    PresetCollection m_presets_private;
};

class PublishTabFilament : public PublishTabWithPresets<TabFilament>
{
public:
    explicit PublishTabFilament(ParamsPanel *host);
    void build() override;
};

class PublishTabPrinter : public PublishTabWithPresets<TabPrinter>
{
public:
    explicit PublishTabPrinter(ParamsPanel *host);

    // TabPrinter's pages, pruned to today's published-3MF allowlist: every other option, group
    // and page is dropped rather than shown unchecked (the TabPrintModel pattern).
    void build() override;
};

class PublishTabPrint : public PublishTabWithPresets<TabPrint>
{
public:
    explicit PublishTabPrint(ParamsPanel *host);
    void build() override;
};

} // GUI
} // Slic3r

#endif // slic3r_GUI_PublishTabs_hpp_
