#pragma once

#include <memory>

#include <wx/gdicmn.h>
#include <wx/window.h>

#include "GUI_Utils.hpp"

namespace Slic3r { namespace GUI {

class Tab;

// Tunes the bone infill of the process preset: its density by depth on a graph, the other bone settings in
// a list, and an approximate preview of the infill in a sample shape. Apply writes the settings into the
// Process tab.
class BoneInfillDialog : public DPIDialog
{
public:
    BoneInfillDialog(wxWindow *parent, Tab *tab);
    ~BoneInfillDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override;

private:
    struct priv;
    std::unique_ptr<priv> p;
};

}} // namespace Slic3r::GUI
