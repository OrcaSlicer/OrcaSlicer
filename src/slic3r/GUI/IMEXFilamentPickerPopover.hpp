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
    // wxPopupTransientWindow::Dismiss() only hides the window; it does not destroy it, and it
    // does not call OnDismiss() either -- only DismissAndNotify() does. The ghost-click handler
    // creates one popover per click and keeps no reference, so without destroying here every
    // click would leak a live top-level window (with its BitmapComboBox, bitmaps and event
    // bindings) parented to the GL canvas. The filament-selected handler therefore calls
    // DismissAndNotify(), not Dismiss(), or it would bypass this entirely.
    void OnDismiss() override;
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
