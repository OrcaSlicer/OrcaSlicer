#include "UnitConversion.hpp"
#include "GUI_App.hpp"

namespace Slic3r::GUI {

wxDEFINE_EVENT(wxEVT_UNIT_SYSTEM_CHANGED, wxCommandEvent);

// ── Singleton ─────────────────────────────────────────────────────────────

UnitSystem& UnitSystem::Get()
{
    static UnitSystem instance;
    return instance;
}

UnitSystem::UnitSystem()
{
    // Load persisted preference on first access
    if (wxGetApp().app_config)
        m_imperial = wxGetApp().app_config->get("use_inches") == "1";
}

// ── Core setter ───────────────────────────────────────────────────────────

void UnitSystem::Apply(bool imperial, bool save)
{
    if (m_imperial == imperial) return;
    m_imperial = imperial;

    if (save && wxGetApp().app_config)
        wxGetApp().app_config->set("use_inches", imperial ? "1" : "0");

    wxCommandEvent evt(wxEVT_UNIT_SYSTEM_CHANGED);
    evt.SetInt(imperial ? 1 : 0);
    for (wxWindow* w : m_listeners)
        if (w) wxPostEvent(w, evt);
}

// ── Public API ────────────────────────────────────────────────────────────

void UnitSystem::SetImperial(bool imperial)
{
    // Called from Preferences dialog — always permanent
    m_temp_active            = false;
    m_perm_toggled_this_hold = false;
    Apply(imperial, /*save=*/true);
}

void UnitSystem::TogglePermanent()
{
    m_perm_toggled_this_hold = true;
    m_temp_active            = false;

    // The value is already correct (BeginTemporarySwap set it),
    // but Apply() would early-return since m_imperial hasn't changed.
    // Force the save directly:
    if (wxGetApp().app_config)
        wxGetApp().app_config->set("use_inches", m_imperial ? "1" : "0");

    // Fire the event so all listeners know this is now permanent
    wxCommandEvent evt(wxEVT_UNIT_SYSTEM_CHANGED);
    evt.SetInt(m_imperial ? 1 : 0);
    for (wxWindow* w : m_listeners)
        if (w) wxPostEvent(w, evt);
}

void UnitSystem::BeginTemporarySwap()
{
    if (m_temp_active) return; // guard against key repeat
    m_pre_swap_state         = m_imperial;
    m_perm_toggled_this_hold = false;
    m_temp_active            = true;
    Apply(!m_imperial, /*save=*/false);
}

void UnitSystem::EndTemporarySwap()
{
    if (!m_temp_active) return;
    m_temp_active = false;

    if (!m_perm_toggled_this_hold) {
        // No permanent toggle during this hold — revert
        Apply(m_pre_swap_state, /*save=*/false);
    }
    // If TogglePermanent() fired, current state is already correct and saved
}

// ── Listener registration ─────────────────────────────────────────────────

void UnitSystem::Register(wxWindow* listener)
{
    m_listeners.push_back(listener);
}

void UnitSystem::Unregister(wxWindow* listener)
{
    m_listeners.erase(
        std::remove(m_listeners.begin(), m_listeners.end(), listener),
        m_listeners.end());
}

} // namespace Slic3r::GUI