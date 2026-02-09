#ifndef slic3r_GUI_PlateMoveDialog_hpp_
#define slic3r_GUI_PlateMoveDialog_hpp_

#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RadioBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "DragCanvas.hpp"
#include "wxExtensions.hpp"
namespace Slic3r { namespace GUI {

class PlateMoveDialog : public DPIDialog
{
public:
    enum ButtonStyle { ONLY_CONFIRM = 0, CONFIRM_AND_CANCEL = 1, MAX_STYLE_NUM = 2 };
    PlateMoveDialog(wxWindow *      parent,
                        wxWindowID      id    = wxID_ANY,
                        const wxString &title = wxEmptyString,
                        const wxPoint & pos   = wxDefaultPosition,
                        const wxSize &  size  = wxDefaultSize,
                        long            style = wxCLOSE_BOX | wxCAPTION);

    ~PlateMoveDialog();
    void on_dpi_changed(const wxRect &suggested_rect) override;
    int  get_swap_index() { return m_specify_plate_idx; }

private:
    void init_bitmaps();
    void update_swipe_button_state();
    void on_previous_plate(wxCommandEvent &event);
    void on_next_plate(wxCommandEvent &event);
    void on_frontmost_plate(wxCommandEvent &event);
    void on_backmost_plate(wxCommandEvent &event);
    void update_plate_combox();
    void update_ok_button_enable();

private:
    Button *   m_button_ok{nullptr};
    Button *   m_button_cancel{nullptr};

    wxArrayString m_plate_number_choices_str;
    std::vector<int> m_plate_choices;
    int              m_specify_plate_idx{0};
    ComboBox *      m_combobox_plate{nullptr};

    ScalableButton *m_swipe_left_button{nullptr};
    ScalableButton *m_swipe_right_button{nullptr};
    bool            m_swipe_left_button_enable;
    bool            m_swipe_right_button_enable;

    ScalableButton *m_swipe_frontmost_button{nullptr};
    ScalableButton *m_swipe_backmost_button{nullptr};
    bool            m_swipe_frontmost_button_enable;
    bool            m_swipe_backmost_button_enable;

    int            m_bmp_pix_cont = 32;
    ScalableBitmap m_swipe_left_bmp_disable;
    ScalableBitmap m_swipe_left_bmp_normal;
    ScalableBitmap m_swipe_left_bmp_hover;
    ScalableBitmap m_swipe_right_bmp_disable;
    ScalableBitmap m_swipe_right_bmp_normal;
    ScalableBitmap m_swipe_right_bmp_hover;

    ScalableBitmap m_swipe_frontmost_bmp_disable;
    ScalableBitmap m_swipe_frontmost_bmp_normal;
    ScalableBitmap m_swipe_frontmost_bmp_hover;
    ScalableBitmap m_swipe_backmost_bmp_disable;
    ScalableBitmap m_swipe_backmost_bmp_normal;
    ScalableBitmap m_swipe_backmost_bmp_hover;


};
}} // namespace Slic3r::GUI

#endif