#pragma once

#include "GUI_Utils.hpp"

#include <cstddef>
#include <vector>
#include <wx/gdicmn.h>
#include <wx/string.h>

class CheckBox;
class TextInput;
class wxSizer;
class wxWindow;

namespace Slic3r { namespace GUI {

// Asks for the dimensions of a primitive shape, in mm, before it is generated.
class PrimitiveSizeDialog : public DPIDialog
{
public:
    struct Field
    {
        wxString label;
        double   value;
    };

    PrimitiveSizeDialog(wxWindow* parent, const wxString& title, const std::vector<Field>& fields);

    const std::vector<double>& get_values() const { return m_values; }
    bool                       save_as_default() const;
    bool                       dont_show_again() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    ::CheckBox* add_checkbox(wxSizer* sizer, const wxString& label, const wxString& tooltip);
    void        on_input_changed(size_t idx);
    bool        read_values();

    std::vector<::TextInput*> m_inputs;
    // The last valid value of each input, which uniform scaling multiplies.
    std::vector<double>       m_last_values;
    ::CheckBox*               m_uniform_scale{nullptr};
    ::CheckBox*               m_save_default{nullptr};
    ::CheckBox*               m_dont_show{nullptr};
    std::vector<double>       m_values;
};

}} // namespace Slic3r::GUI
