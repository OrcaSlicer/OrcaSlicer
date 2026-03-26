#ifndef slic3r_SettingsTransferDialog_hpp_
#define slic3r_SettingsTransferDialog_hpp_

#include <string>
#include <unordered_map>
#include <vector>

#include "GUI_Utils.hpp"
#include "libslic3r/ProcessSettingsMerger.hpp"
#include "Widgets/Button.hpp"
#include "wxExtensions.hpp"

class wxCheckBox;
class wxCheckListBox;
class wxChoice;

namespace Slic3r {
namespace GUI {

struct SettingsTransferOption
{
    std::string option_key;
    std::string source_value;
    std::string target_value;
};

struct SettingsTransferTargetProfile
{
    std::string profile_name;
    std::vector<SettingsTransferOption> transferable_options;
};

class SettingsTransferDialog : public DPIDialog
{
public:
    SettingsTransferDialog(wxWindow *parent, const std::vector<SettingsTransferTargetProfile> &target_profiles, const std::string &initial_target_profile, bool preselect_conditional);
    ~SettingsTransferDialog() override = default;

    std::vector<ProcessSettingsCategory> selected_categories() const;
    std::vector<std::string> selected_option_keys() const;
    std::string selected_target_profile_name() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override;

private:
    void on_option_toggled(wxCommandEvent &event);
    void refresh_transferable_options();

    std::vector<SettingsTransferTargetProfile> m_target_profiles;
    std::vector<SettingsTransferOption> m_displayed_options;
    std::unordered_map<std::string, bool> m_manual_option_overrides;
    wxChoice *m_target_profile_choice { nullptr };
    wxCheckListBox *m_option_checklist { nullptr };
    wxCheckBox *m_safe_checkbox { nullptr };
    wxCheckBox *m_conditional_checkbox { nullptr };
    wxCheckBox *m_hardware_checkbox { nullptr };
    ::Button *m_confirm_button { nullptr };
    ::Button *m_cancel_button { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif