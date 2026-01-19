#ifndef slic3r_GUI_SideMenuPopup_hpp_
#define slic3r_GUI_SideMenuPopup_hpp_

#include <wx/stattext.h>
#include <wx/vlbox.h>
#include <wx/combo.h>
#include <wx/htmllbox.h>
#include <wx/frame.h>
#include "../wxExtensions.hpp"
#include "StateHandler.hpp"
#include "SideButton.hpp"
#include "PopupWindow.hpp"

class SidePopup : public PopupWindow
{
private:
	std::vector<SideButton*> btn_list;
public:
    SidePopup(wxWindow* parent);
    ~SidePopup();

    void Create();

    virtual void Popup(wxWindow *focus = NULL) wxOVERRIDE;
    virtual void OnDismiss() wxOVERRIDE;
    virtual bool ProcessLeftDown(wxMouseEvent& event) wxOVERRIDE;
    virtual bool Show(bool show = true) wxOVERRIDE;

    void append_button(SideButton* btn);

    void paintEvent(wxPaintEvent& evt);

	DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_Button_hpp_
