#ifndef slic3r_GUI_StaticGroup_hpp_
#define slic3r_GUI_StaticGroup_hpp_

#include "../wxExtensions.hpp"

#include "LabeledStaticBox.hpp"

class StaticGroup : public LabeledStaticBox
{
public:
    StaticGroup(wxWindow *parent, wxWindowID id, const wxString &label);
    void ShowBadge(bool show);

private:
    void DrawBorderAndLabel(wxDC& dc) override;
    ScalableBitmap badge;
};

#endif // !slic3r_GUI_StaticGroup_hpp_
