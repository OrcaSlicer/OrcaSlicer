#pragma once
#include <string>
#include <vector>
#include <wx/wx.h>

namespace Slic3r::GUI {

// ── Event fired on all registered listeners when the unit system changes ──
wxDECLARE_EVENT(wxEVT_UNIT_SYSTEM_CHANGED, wxCommandEvent);

// ── Singleton managing the current unit system preference ─────────────────
class UnitSystem
{
public:
    static UnitSystem& Get();

    bool IsImperial()      const { return m_imperial; }
    bool IsTempSwapActive() const { return m_temp_active; }

    // Permanent toggle (Alt+Shift). Saves to AppConfig.
    void TogglePermanent();

    // Alt down — flip temporarily without saving.
    void BeginTemporarySwap();

    // Alt up — revert unless a permanent toggle happened during this hold.
    void EndTemporarySwap();

    // Direct set from Preferences dialog — always saves.
    void SetImperial(bool imperial);

    void Register(wxWindow* listener);
    void Unregister(wxWindow* listener);

private:
    UnitSystem();

    bool m_imperial               = false;
    bool m_temp_active            = false;
    bool m_perm_toggled_this_hold = false;
    bool m_pre_swap_state         = false;

    std::vector<wxWindow*> m_listeners;

    // save=true writes to AppConfig
    void Apply(bool imperial, bool save);
};

} // namespace Slic3r::GUI