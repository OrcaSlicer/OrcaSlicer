#ifndef slic3r_GUI_PublishTabs_hpp_
#define slic3r_GUI_PublishTabs_hpp_

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "ParamsPanel.hpp"
#include "Tab.hpp"

namespace Slic3r {
namespace GUI {

// A bare ParamsPanel for the Publish 3MF dialog: builds the page view without the sidebar's
// top panel, mode switch or tab scan, and hosts one publish tab.
class PublishParamsPanel : public ParamsPanel
{
public:
    explicit PublishParamsPanel(wxWindow *parent);
    ~PublishParamsPanel();

    // Lays out `tab` and activates its first page. Call after Tab::create_publish_tab().
    // Passing nullptr (or another tab) detaches and clears the previous one.
    void set_publish_tab(Tab *tab);

private:
    Tab *m_publish_tab{nullptr};
};

// A settings Tab over a dialog-owned PresetCollection. Construct, then call
// create_publish_tab(seed_config) before handing the tab to PublishParamsPanel.
class PublishTabFilament : public TabFilament
{
public:
    explicit PublishTabFilament(ParamsPanel *host);

protected:
    PresetCollection *publish_presets() override { return &m_presets_private; }

private:
    PresetCollection m_presets_private;
};

class PublishTabPrinter : public TabPrinter
{
public:
    explicit PublishTabPrinter(ParamsPanel *host);

protected:
    PresetCollection *publish_presets() override { return &m_presets_private; }

private:
    PresetCollection m_presets_private;
};

class PublishTabPrint : public TabPrint
{
public:
    explicit PublishTabPrint(ParamsPanel *host);

protected:
    PresetCollection *publish_presets() override { return &m_presets_private; }

private:
    PresetCollection m_presets_private;
};

} // GUI
} // Slic3r

#endif // slic3r_GUI_PublishTabs_hpp_
