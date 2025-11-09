#ifndef slic3r_FilamentMapDialog_hpp_
#define slic3r_FilamentMapDialog_hpp_

#include "FilamentMapPanel.hpp"
#include <vector>
#include "CapsuleButton.hpp"
#include "Widgets/CheckBox.hpp"

class Button;

namespace Slic3r {
class DynamicPrintConfig;

namespace GUI {
class DragDropPanel;
class Plater;
class PartPlate;

/**
 * @brief Try to pop up the filament map dialog before slicing.
 * 
 * Only pop up in multi extruder machines. If user don't want the pop up, we
 * pop up if the applied filament map mode in manual
 * 
 * @param is_slice_all  In slice all
 * @param plater_ref Plater to get/set global filament map
 * @param partplate_ref Partplate to get/set plate filament map mode
 * @return whether continue slicing
*/
bool try_pop_up_before_slice(bool is_slice_all, Plater* plater_ref, PartPlate* partplate_ref, bool force_pop_up = false);


class FilamentMapDialog : public wxDialog
{
    enum PageType {
        ptAuto,
        ptManual,
        ptDefault
    };
public:
    FilamentMapDialog(wxWindow *parent,
        const std::vector<std::string>& filament_color,
        const std::vector<std::string>& filament_type,
        const std::vector<int> &filament_map,
        const std::vector<int> &filaments,
        const FilamentMapMode mode,
        bool machine_synced,
        bool show_default=true,
        bool with_checkbox = false
    );

    FilamentMapMode get_mode();
    std::vector<int> get_filament_maps() const {
        if (m_page_type == PageType::ptManual)
            return m_filament_map;
        return {};
    }

    int ShowModal();
    void set_modal_btn_labels(const wxString& left_label, const wxString& right_label);
private:
    void on_ok(wxCommandEvent &event);
    void on_cancle(wxCommandEvent &event);
    void on_switch_mode(wxCommandEvent &event);
    void on_checkbox(wxCommandEvent &event);

    void update_panel_status(PageType page);

 private:
    FilamentMapManualPanel* m_manual_map_panel;
    FilamentMapAutoPanel* m_auto_map_panel;
    FilamentMapDefaultPanel* m_default_map_panel;

    CapsuleButton* m_auto_btn;
    CapsuleButton* m_manual_btn;
    CapsuleButton* m_default_btn;

    Button* m_ok_btn;
    Button* m_cancel_btn;
    CheckBox* m_checkbox;

    PageType m_page_type;

private:
    std::vector<int> m_filament_map;
    std::vector<std::string> m_filament_color;
    std::vector<std::string> m_filament_type;
};

}} // namespace Slic3r::GUI

#endif /* slic3r_FilamentMapDialog_hpp_ */
