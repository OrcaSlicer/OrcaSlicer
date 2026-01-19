#ifndef slic3r_GUI_SingleChoice_hpp_
#define slic3r_GUI_SingleChoice_hpp_

#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"

namespace Slic3r { namespace GUI {

class SingleChoiceDialog : public DPIDialog
{
public:
    SingleChoiceDialog(const wxString &message, const wxString &caption, const wxArrayString &choices, int initialSelectionwx, wxWindow *parent = nullptr);
    ~SingleChoiceDialog();

    int       GetSingleChoiceIndex();
    ComboBox *GetTypeComboBox() { return type_comboBox; };

    void on_dpi_changed(const wxRect &suggested_rect) override;

protected:
    ComboBox *type_comboBox   = nullptr;
};

}} // namespace Slic3r::GUI

#endif