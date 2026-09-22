#ifndef WGTMSGBOX_H
#define WGTMSGBOX_H

#include <wx/wx.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/button.h>

#include "slic3r/GUI/Widgets/StaticBox.hpp"

class Label;
namespace Slic3r::GUI
{
class wgtMsgBox : public StaticBox
{
public:
    wgtMsgBox(wxWindow* parent);

public:
    Label* GetTextLabel() const { return m_txt_label; }

private:
    Label* m_txt_label;

private:
    void CreateGUI();
    void OnCloseClicked(wxCommandEvent& evt);
};
}

#endif // WGTMSGBOX_H