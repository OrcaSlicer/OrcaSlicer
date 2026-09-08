#ifndef slic3r_IMEXFilamentPickerPopover_hpp_
#define slic3r_IMEXFilamentPickerPopover_hpp_

#include <wx/popupwin.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include <functional>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

class PartPlate;

class IMEXFilamentPickerPopover : public wxPopupTransientWindow
{
public:
    using CommitCallback = std::function<void()>;

    IMEXFilamentPickerPopover(wxWindow* parent, PartPlate* plate,
                              const ConfigOptionInts& pem, int physical_head,
                              CommitCallback on_commit);

    void popup_at_cursor();

private:
    void build_row();
    void on_filament_selected(int slot_1_based);

    PartPlate*              m_plate;
    // Owned by value: callers may pass a stack-local derived pem
    // (e.g. effective_physical_extruder_map result) that won't outlive
    // the popover's async lifetime.
    ConfigOptionInts        m_pem;
    int                     m_physical_head;
    CommitCallback          m_on_commit;

    wxSizer* m_root_sizer = nullptr;
};

}} // namespace Slic3r::GUI

#endif
