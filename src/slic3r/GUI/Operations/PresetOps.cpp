#include "PresetOps.hpp"

#include "Outcome.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

Preset::Type preset_type(PresetKind kind)
{
    switch (kind) {
    case PresetKind::Printer: return Preset::TYPE_PRINTER;
    case PresetKind::Filament: return Preset::TYPE_FILAMENT;
    default: return Preset::TYPE_PRINT;
    }
}

PresetCollection& collection(PresetBundle& bundle, PresetKind kind)
{
    switch (kind) {
    case PresetKind::Printer: return bundle.printers;
    case PresetKind::Filament: return bundle.filaments;
    default: return bundle.prints;
    }
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The presets `name` names, by name or alias, that `keep` accepts. The real presets, not the edited copy.
template<class Keep> std::vector<Preset*> presets_named(PresetCollection& presets, const std::string& name, Keep keep)
{
    const std::string    want = Preset::remove_suffix_modified(name);
    std::vector<Preset*> found;
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (Preset& p = presets.preset(i, true); (p.name == want || p.alias == want) && keep(p))
            found.push_back(&p);
    return found;
}

std::vector<std::string> names_of(const std::vector<Preset*>& presets)
{
    std::vector<std::string> names;
    for (const Preset* p : presets)
        names.push_back(p->name);
    return names;
}

// Shows the hidden preset `name` names in the selectors, as they show the preset selected and the filaments an AMS
// holds: in memory, so the setup wizard's list, and with it what the next start shows, stays as it is. A hidden
// preset is one the wizard has not added (a filament, a printer model), not a refusal, so selecting one by name
// needs no detour through the wizard. Only a compatible one, as the selectors list no other. Returns its name, empty
// when `name` names no hidden compatible preset.
Outcome<std::string> show_hidden_preset(PresetCollection& presets, const std::string& name)
{
    std::vector<Preset*> hidden = presets_named(presets, name, [](const Preset& p) { return !p.is_visible && p.is_compatible; });
    // A preset's own name wins over another's alias that reads the same ("Generic PLA").
    const std::string want  = Preset::remove_suffix_modified(name);
    const auto        exact = std::find_if(hidden.begin(), hidden.end(), [&want](const Preset* p) { return p->name == want; });
    if (exact != hidden.end())
        hidden = {*exact};
    if (hidden.size() > 1)
        return Refusal{RefusalKind::Precondition, "ambiguous_hidden_preset", L("\"%1%\" names several hidden presets. Use the preset's internal name."),
                       {name}, names_of(hidden)};
    if (hidden.empty())
        return std::string();
    hidden.front()->is_visible = true;
    return hidden.front()->name;
}

// Shows the default filaments of a printer show_hidden_preset() showed when no shown filament suits it, as adding the
// printer in the setup wizard does (PresetBundle::load_installed_filaments()), so selecting the printer lands on
// one of them rather than on the default filament. In memory, as the printer is.
void show_default_filaments(PresetBundle& bundle, const std::string& printer_name)
{
    const Preset*                      printer = bundle.printers.find_preset(printer_name, false);
    const VendorProfile::PrinterModel* model   = printer != nullptr ? PresetUtils::system_printer_model(*printer) : nullptr;
    if (model == nullptr)
        return;
    const PresetWithVendorProfile with_vendor = bundle.printers.get_preset_with_vendor_profile(*printer);
    for (const Preset& filament : bundle.filaments)
        if (filament.is_visible && is_compatible_with_printer(bundle.filaments.get_preset_with_vendor_profile(filament), with_vendor))
            return;
    for (const std::string& name : model->default_materials)
        if (Preset* filament = bundle.filaments.find_preset(name, false, true); filament != nullptr && filament->is_system)
            filament->is_visible = true;
}

// The refusal of a name a selector does not list: incompatible with the printer, or no preset's name. `filament_slot`
// names the slot whose selector it is, 0 for a tab's.
Refusal not_offered(PresetCollection& presets, const std::string& name, int filament_slot)
{
    if (const std::vector<Preset*> incompatible = presets_named(presets, name, [](const Preset& p) { return !p.is_compatible; });
        !incompatible.empty())
        return {RefusalKind::Precondition, "preset_incompatible", L("\"%1%\" is not compatible with the selected printer."), {name},
                names_of(incompatible)};
    if (filament_slot > 0)
        // TRN %1% is a preset name, %2% a filament slot number.
        return {RefusalKind::Precondition, "preset_not_offered", L("The selector of filament slot %2% does not offer \"%1%\"."),
                {name, std::to_string(filament_slot)}, similar_preset_names(presets, name)};
    return {RefusalKind::Precondition, "preset_not_offered", L("The preset selector does not offer \"%1%\"."), {name},
            similar_preset_names(presets, name)};
}

Refusal ambiguous(const std::string& name, int filament_slot)
{
    if (filament_slot > 0)
        // TRN %1% is a preset name, %2% a filament slot number.
        return {RefusalKind::Precondition, "ambiguous_preset_name",
                L("The name \"%1%\" is ambiguous in the selector of filament slot %2%. Use the preset's internal name."),
                {name, std::to_string(filament_slot)}};
    return {RefusalKind::Precondition, "ambiguous_preset_name",
            L("The name \"%1%\" is ambiguous in the preset selector. Use the preset's internal name."), {name}};
}

Refusal tab_unavailable() { return {RefusalKind::Failed, "tab_unavailable", L("The settings tab is not available.")}; }

// The filament slots a caller can name: the sidebar's selectors, one per physical filament.
std::size_t filament_slots(Sidebar& sidebar, const PresetBundle& bundle)
{
    const std::size_t selectors = sidebar.combos_filament().size();
    return selectors > 0 ? selectors : bundle.num_physical_filaments();
}

Refusal no_such_slot(int slot, std::size_t slots)
{
    // TRN %1% is the filament slot number asked for, %2% the last slot's number.
    return {RefusalKind::Precondition, "no_such_slot", L("Filament slot %1% does not exist (the slots are 1..%2%)"),
            {std::to_string(slot), std::to_string(slots)}};
}

// The prompt about a tab's own unsaved changes offers Transfer only where Tab::can_transfer_changes_to() says;
// refused before anything changes.
std::optional<Refusal> transfer_refusal(const Tab& tab, const PresetCollection& presets, PresetKind kind, const std::string& preset,
                                        PresetChanges changes)
{
    if (changes != PresetChanges::Transfer || !presets.current_is_dirty() || tab.can_transfer_changes_to(preset))
        return std::nullopt;
    if (kind == PresetKind::Printer)
        return Refusal{RefusalKind::Precondition, "transfer_not_offered",
                       L("The unsaved changes cannot be transferred: a printer's changes stay with it.")};
    return Refusal{RefusalKind::Precondition, "transfer_not_offered",
                   L("The unsaved changes cannot be transferred: \"%1%\" is a filament of another type."), {preset}};
}

// Gives a tab the caller's answer to its unsaved-changes prompt for one switch.
class AnswerScope
{
public:
    AnswerScope(Tab& tab, PresetChanges changes) : m_tab(tab)
    {
        m_tab.m_dirty_preset_answer = changes == PresetChanges::Transfer ? Tab::DirtyPresetAnswer::Transfer :
                                      changes == PresetChanges::Discard  ? Tab::DirtyPresetAnswer::Discard :
                                                                           Tab::DirtyPresetAnswer::Ask;
    }
    ~AnswerScope() { m_tab.m_dirty_preset_answer = Tab::DirtyPresetAnswer::Ask; }
    AnswerScope(const AnswerScope&)            = delete;
    AnswerScope& operator=(const AnswerScope&) = delete;

private:
    Tab& m_tab;
};

} // namespace

std::optional<Refusal> mixed_slot_refusal(int filament_slot)
{
    assert(wxThread::IsMain());
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    if (filament_slot < 1 || std::size_t(filament_slot) > bundle.filament_presets.size() ||
        !bundle.is_mixed_filament(std::size_t(filament_slot) - 1))
        return std::nullopt;
    // TRN %1% is a filament slot number.
    return Refusal{RefusalKind::Precondition, "mixed_filament_slot",
                   L("Filament slot %1% is a mixed-color virtual slot, which has no filament preset of its own."),
                   {std::to_string(filament_slot)}};
}

std::vector<std::string> similar_preset_names(const PresetCollection& presets, const std::string& name)
{
    std::vector<std::string> words;
    std::istringstream       in(lower(name));
    for (std::string w; in >> w;)
        words.push_back(w);
    std::vector<std::pair<int, std::string>> scored;
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const Preset& p = presets.preset(i);
        if (p.is_default)
            continue;
        const std::string n       = lower(p.name);
        const int         matched = int(std::count_if(words.begin(), words.end(), [&n](const std::string& w) { return n.find(w) != std::string::npos; }));
        if (matched > 0)
            scored.emplace_back(-(2 * matched + (p.is_compatible ? 1 : 0)), p.name);
    }
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    for (std::size_t i = 0; i < scored.size() && i < 5; ++i)
        out.push_back(scored[i].second);
    return out;
}

Outcome<ResolvedPreset> resolve_preset(const ResolvePresetArgs& args)
{
    assert(wxThread::IsMain());
    PresetBundle&     bundle  = *wxGetApp().preset_bundle;
    PresetCollection& presets = collection(bundle, args.kind);
    Tab*              tab     = wxGetApp().get_tab(preset_type(args.kind));

    // The selector whose list decides: the slot's for a filament, else, or where that one is not
    // populated, the tab's.
    PresetComboBox* selector = nullptr;
    int             slot     = 0; // the slot whose selector it is, 0 for the tab's
    if (args.kind == PresetKind::Filament) {
        if (std::optional<Refusal> refusal = mixed_slot_refusal(args.filament_slot))
            return *refusal;
        Sidebar&                            sidebar = wxGetApp().plater()->sidebar();
        std::vector<PlaterPresetComboBox*>& combos  = sidebar.combos_filament();
        const std::size_t                   slots   = filament_slots(sidebar, bundle);
        if (args.filament_slot < 1 || std::size_t(args.filament_slot) > slots)
            return no_such_slot(args.filament_slot, slots);
        PlaterPresetComboBox* combo = std::size_t(args.filament_slot) <= combos.size() ? combos[std::size_t(args.filament_slot) - 1] : nullptr;
        if (combo != nullptr && combo->GetCount() > 0) {
            selector = combo;
            slot     = args.filament_slot;
        }
    }
    if (selector == nullptr && tab != nullptr)
        if (TabPresetComboBox* combo = tab->get_combo_box(); combo != nullptr && combo->GetCount() > 0)
            selector = combo;
    if (selector == nullptr) {
        // Nothing is laid out to resolve against: internal names.
        const Preset* preset = presets.find_preset(args.name);
        if (preset == nullptr)
            return not_offered(presets, args.name, 0);
        return ResolvedPreset{preset->name, {}};
    }

    ResolvedPreset            resolved;
    PresetComboBox::NameMatch match = selector->find_by_name(args.name);
    if (match.item < 0 && !match.ambiguous) {
        const Outcome<std::string> shown = show_hidden_preset(presets, args.name);
        if (!shown)
            return shown.refusal();
        if (!shown.value().empty()) {
            resolved.shown = shown.value();
            if (args.kind == PresetKind::Printer)
                show_default_filaments(bundle, resolved.shown);
            if (slot > 0) {
                // Every slot's selector lists the filaments.
                for (PlaterPresetComboBox* combo : wxGetApp().plater()->sidebar().combos_filament())
                    if (combo != nullptr)
                        combo->update();
            } else {
                selector->update();
            }
            match = selector->find_by_name(resolved.shown);
        }
    }
    if (match.ambiguous)
        return ambiguous(args.name, slot);
    if (match.item < 0)
        return not_offered(presets, args.name, slot);
    resolved.name = match.internal_name;
    return resolved;
}

Outcome<Done> select_preset(const SelectPresetArgs& args)
{
    assert(wxThread::IsMain());
    assert(args.kind != PresetKind::Filament);
    PresetCollection& presets = collection(*wxGetApp().preset_bundle, args.kind);
    Tab*              tab     = wxGetApp().get_tab(preset_type(args.kind));
    if (tab == nullptr)
        return tab_unavailable();
    if (std::optional<Refusal> refusal = transfer_refusal(*tab, presets, args.kind, args.preset, args.changes))
        return *refusal;
    AnswerScope answer(*tab, args.changes);
    if (!tab->select_preset(args.preset))
        return Refusal{RefusalKind::Failed, "selection_cancelled", L("Selecting \"%1%\" was cancelled."), {args.preset}};
    // The tab can end on another preset than the one it was given, where that one is not among its choices.
    if (const std::string selected = presets.get_selected_preset_name(); selected != args.preset)
        // TRN %1% is the preset asked for, %2% the one that is selected.
        return Refusal{RefusalKind::Failed, "other_preset_selected", L("\"%2%\" was selected instead of \"%1%\"."), {args.preset, selected}};
    return Done{};
}

Outcome<std::string> assign_filament_preset(const AssignFilamentArgs& args)
{
    assert(wxThread::IsMain());
    PresetBundle&     bundle  = *wxGetApp().preset_bundle;
    Sidebar&          sidebar = wxGetApp().plater()->sidebar();
    const std::size_t slots   = filament_slots(sidebar, bundle);
    if (std::optional<Refusal> refusal = mixed_slot_refusal(args.filament_slot))
        return *refusal;
    if (args.filament_slot < 1 || std::size_t(args.filament_slot) > slots || std::size_t(args.filament_slot) > bundle.filament_presets.size())
        return no_such_slot(args.filament_slot, slots);
    // The sidebar switches the filament tab for a single filament only, and there its prompt applies.
    Tab*       tab          = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
    const bool switches_tab = args.switch_tab && !sidebar.is_multifilament();
    if (switches_tab) {
        if (tab == nullptr)
            return tab_unavailable();
        if (std::optional<Refusal> refusal = transfer_refusal(*tab, bundle.filaments, PresetKind::Filament, args.preset, args.changes))
            return *refusal;
    }
    std::optional<AnswerScope> answer;
    if (tab != nullptr)
        answer.emplace(*tab, args.changes);
    sidebar.select_filament_preset(std::size_t(args.filament_slot) - 1, args.preset, args.switch_tab);
    return bundle.filament_presets[std::size_t(args.filament_slot) - 1];
}

Outcome<ImportedPresets> import_presets(const ImportPresetsArgs& args)
{
    assert(wxThread::IsMain());
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto          names  = [&bundle] {
        std::set<std::pair<int, std::string>> out;
        for (const PresetCollection* presets : std::initializer_list<const PresetCollection*>{&bundle.prints, &bundle.filaments, &bundle.printers})
            for (const Preset& preset : presets->get_presets())
                out.emplace(int(presets->type()), preset.name);
        return out;
    };
    const auto      before = names();
    ImportedPresets result;
    // Answered for each preset that exists already as the dialog's Yes or No would be, never "to all",
    // which would stop the questions, so that every one of them can be named.
    wxGetApp().mainframe->import_configs({into_u8(from_path(args.path))}, [&](const std::string& name) {
        (args.overwrite ? result.replaced : result.kept).push_back(name);
        return args.overwrite ? 1 : 0;
    });
    for (const auto& name : names())
        if (before.count(name) == 0)
            result.imported.push_back(name.second);
    if (result.imported.empty() && result.replaced.empty() && result.kept.empty())
        return Refusal{RefusalKind::Failed, "no_presets_imported",
                       L("No presets were imported.\nHint: Make sure you have added the corresponding printer before importing the configs.")};
    return result;
}

}}} // namespace Slic3r::GUI::Ops
