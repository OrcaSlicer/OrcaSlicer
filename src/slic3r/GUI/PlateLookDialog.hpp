#pragma once

#include <vector>
#include <wx/button.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include "GUI_Utils.hpp"
#include "PlateLook.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

// Lets the user rename the plate types of the current printer and give them other icons and skins.
class PlateLookDialog : public DPIDialog
{
public:
    PlateLookDialog(wxWindow *parent, const std::vector<BedType> &bed_types);

    const std::vector<PlateLook> &looks() const { return m_looks; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    BedType selected_bed_type() const { return m_bed_types[m_selected]; }
    void    select_plate(size_t index);
    void    show_look();
    void    choose_image(std::string &image_path);
    void    reset_selected_plate();

    std::vector<BedType>   m_bed_types;
    std::vector<PlateLook> m_looks;
    size_t                 m_selected { 0 };

    ComboBox       *m_plate_combo { nullptr };
    TextInput      *m_label_input { nullptr };
    wxStaticBitmap *m_icon_preview { nullptr };
    wxStaticText   *m_icon_path   { nullptr };
    wxStaticBitmap *m_skin_preview { nullptr };
    wxStaticText   *m_skin_path   { nullptr };
};

// Opens the dialog for the current printer's plate types and saves what the user changed.
void edit_plate_looks(wxWindow *parent);

}} // namespace Slic3r::GUI
