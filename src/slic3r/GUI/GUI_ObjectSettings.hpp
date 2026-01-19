#ifndef slic3r_GUI_ObjectSettings_hpp_
#define slic3r_GUI_ObjectSettings_hpp_

#include <memory>
#include <vector>
#include <wx/panel.h>
#include "wxExtensions.hpp"

#define NEW_OBJECT_SETTING 1

class wxBoxSizer;

namespace Slic3r {
class DynamicPrintConfig;
class ModelConfig;
namespace GUI {
class ConfigOptionsGroup;

class OG_Settings
{
protected:
    std::shared_ptr<ConfigOptionsGroup> m_og;
    wxWindow* m_parent;
public:
    OG_Settings(wxWindow* parent, const bool staticbox);
    virtual ~OG_Settings() {}

    virtual bool        IsShown();
    virtual void        Show(const bool show);
    virtual void        Hide();
    virtual void        UpdateAndShow(const bool show);

    virtual wxSizer*    get_sizer();
    ConfigOptionsGroup* get_og() { return m_og.get(); }
    wxWindow*           parent() const {return m_parent; }
};

class TabPrintModel;

#if !NEW_OBJECT_SETTING
class ObjectSettings : public OG_Settings
#else
class ObjectSettings
#endif
{
    // sizer for extra Object/Part's settings
#if !NEW_OBJECT_SETTING
    wxBoxSizer* m_settings_list_sizer{ nullptr };  
    // option groups for settings
    std::vector <std::shared_ptr<ConfigOptionsGroup>> m_og_settings;

    ScalableBitmap m_bmp_delete;
    ScalableBitmap m_bmp_delete_focus;
#else
    wxWindow* m_parent;
    TabPrintModel * m_tab_active;
#endif

public:
    ObjectSettings(wxWindow* parent);
    ~ObjectSettings() {}

    bool        update_settings_list();
    /* Additional check for override options: Add options, if its needed.
     * Example: if Infill is set to 100%, and Fill Pattern is missed in config_to,
     * we should add sparse_infill_pattern to avoid endless loop in update
     */
    bool        add_missed_options(ModelConfig *config_to, const DynamicPrintConfig &config_from);
    void        update_config_values(ModelConfig *config);
    void        UpdateAndShow(const bool show);
    void        msw_rescale();
    void        sys_color_changed();
};

}}

#endif // slic3r_GUI_ObjectSettings_hpp_
