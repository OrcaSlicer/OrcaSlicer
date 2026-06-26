#ifndef slic3r_NozzlePickerPanel_hpp_
#define slic3r_NozzlePickerPanel_hpp_

#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>

class wxBoxSizer;

// ComboBox is declared at global scope (not in Slic3r::GUI), so forward-declare it here, before
// the namespace -- as Plater.hpp / AMSSetting.hpp do. Declaring it inside the namespace would make
// a distinct Slic3r::GUI::ComboBox that shadows the real one and never completes.
class ComboBox;

namespace Slic3r { namespace GUI {

// Sidebar panel that shows one nozzle-diameter dropdown per nozzle (dynamic N, e.g. the
// Snapmaker U1 toolchanger's 4 toolheads) plus an Apply button. Mirrors the Filament section's
// layout: a gradient title bar (nozzle icon + "Nozzles" label, bracketed by StaticLine separators)
// above a two-column grid of flush, compact inline rows -- each row is "Nozzle N" followed by its
// diameter dropdown, filled row-major (1 2 / 3 4 ...).
// Selections are independent and silent: changing a dropdown only updates pending state. Clicking Apply
// invokes the on_apply callback with the per-nozzle diameters; the owner (Sidebar) performs the actual
// config write, extruder reconcile, and the mixed-nozzle line-width conversion offer. This widget owns
// no print config and no conversion logic.
class NozzlePickerPanel : public wxPanel
{
public:
    NozzlePickerPanel(wxWindow* parent);

    // Rebuild the dropdown grid to N = current.size(). Each combo offers `allowed` diameter strings
    // (e.g. "0.4") and is initialised from `current`. Safe to call repeatedly (printer switch /
    // nozzle-count change); existing combos are destroyed and recreated.
    void rebuild(const std::vector<double>& current, const std::vector<std::string>& allowed);

    // The per-nozzle diameters currently shown in the dropdowns (size == nozzle count).
    std::vector<double> pending_diameters() const;

    // Called when Apply is clicked, with pending_diameters(). The owner applies the change.
    void set_on_apply(std::function<void(const std::vector<double>&)> cb) { m_on_apply = std::move(cb); }

    size_t extruder_count() const { return m_selectors.size(); }

private:
    void on_apply_clicked();

    wxBoxSizer*              m_grid_sizer{nullptr}; // flush (no frame); holds NOZZLE_COLUMNS column sizers of compact "Nozzle N + combo" rows
    std::vector<ComboBox*>   m_selectors;           // one dropdown per nozzle (independent)
    std::vector<std::string> m_allowed;             // diameter strings offered in every combo
    std::function<void(const std::vector<double>&)> m_on_apply;
};

}} // namespace Slic3r::GUI

#endif // slic3r_NozzlePickerPanel_hpp_
