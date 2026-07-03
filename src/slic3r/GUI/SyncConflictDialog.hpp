#pragma once

#include "GUI_Utils.hpp"
#include "slic3r/Utils/SyncBackend.hpp"

#include <wx/dialog.h>
#include <wx/checkbox.h>

namespace Slic3r { namespace GUI {

class SyncConflictDialog : public DPIDialog
{
public:
    SyncConflictDialog(wxWindow* parent, const SyncConflict& conflict);

    ConflictResolution get_resolution() const { return m_resolution; }
    bool               apply_to_all() const { return m_apply_to_all; }

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    ConflictResolution m_resolution{ConflictResolution::Skip};
    bool               m_apply_to_all{false};
    wxCheckBox*        m_apply_all_checkbox{nullptr};

    void build_ui(const SyncConflict& conflict);
    static std::string format_time(long long timestamp);
};

}} // namespace Slic3r::GUI
