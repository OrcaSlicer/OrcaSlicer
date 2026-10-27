#ifndef slic3r_ConfigWizard_hpp_
#define slic3r_ConfigWizard_hpp_

#include <memory>

#include <wx/dialog.h>
#include <wx/string.h>

#include "ConfigWizardTypes.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r {

class PresetBundle;
class PresetUpdater;

namespace GUI {


class ConfigWizard: public DPIDialog
{
public:
    ConfigWizard(wxWindow *parent);
    ConfigWizard(ConfigWizard &&) = delete;
    ConfigWizard(const ConfigWizard &) = delete;
    ConfigWizard &operator=(ConfigWizard &&) = delete;
    ConfigWizard &operator=(const ConfigWizard &) = delete;
    ~ConfigWizard();

    // Run the Wizard. Return whether it was completed.
    bool run(RunReason reason, StartPage start_page = StartPage::SP_WELCOME);

    static const wxString& name(const bool from_menu = false);
protected:
    void on_dpi_changed(const wxRect &suggested_rect) override ;
    void on_sys_color_changed() override;

private:
    struct priv;
    std::unique_ptr<priv> p;

    friend struct ConfigWizardPage;
};



}
}

#endif
