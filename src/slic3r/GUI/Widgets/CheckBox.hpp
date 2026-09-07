#ifndef slic3r_GUI_CheckBox_hpp_
#define slic3r_GUI_CheckBox_hpp_

#include "../wxExtensions.hpp"

#include <wx/tglbtn.h>

class CheckBox : public wxBitmapToggleButton
{
public:
	CheckBox(wxWindow * parent, int id = wxID_ANY);

public:
	void SetValue(bool value) override;

	void SetHalfChecked(bool value = true);

	// Orca: Makes a left click anywhere on `label` toggle this box, the way the label of a
	// native wxCheckBox does. `label` is typically the wxStaticText placed next to the box.
	// Only bind labels that follow the box, as a native check box has them: a label placed
	// before the box reads as a form caption, and toggling from it invites accidental clicks.
	void BindLabel(wxWindow *label);

	// Only meant to be used by inspector, not public API
	bool IsHalfChecked() const { return m_half_checked; }

	void Rescale();

#ifdef __WXOSX__
    virtual bool Enable(bool enable = true) wxOVERRIDE;
#endif

protected:
#ifdef __WXMSW__
    virtual State GetNormalState() const wxOVERRIDE;
#endif
    
#ifdef __WXOSX__
    virtual wxBitmap DoGetBitmap(State which) const wxOVERRIDE;
    
    void updateBitmap(wxEvent & evt);
    
    bool m_disable = false;
    bool m_hover = false;
    bool m_focus = false;
#endif
    
private:
	void update();

private:
    ScalableBitmap m_on;
    ScalableBitmap m_half;
    ScalableBitmap m_off;
    ScalableBitmap m_on_disabled;
    ScalableBitmap m_half_disabled;
    ScalableBitmap m_off_disabled;
    ScalableBitmap m_on_focused;
    ScalableBitmap m_half_focused;
    ScalableBitmap m_off_focused;
    bool m_half_checked = false;
};

#endif // !slic3r_GUI_CheckBox_hpp_
