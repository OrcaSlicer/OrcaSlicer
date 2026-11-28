#include "PresetOps.hpp"

#include "Outcome.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MixedFilamentDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/SyncAmsInfoDialog.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <numeric>
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
    // Incompatible only when no compatible preset carries the name: one that does is listed under another name (an
    // alias such as "0.16mm Optimal" names presets for many printers), which the similar names point to.
    if (const std::vector<Preset*> incompatible = presets_named(presets, name, [](const Preset& p) { return !p.is_compatible; });
        !incompatible.empty() && presets_named(presets, name, [](const Preset& p) { return p.is_compatible; }).empty())
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

Outcome<Done> sync_presets()
{
    assert(wxThread::IsMain());
    if (!wxGetApp().is_user_login())
        return Refusal{RefusalKind::Precondition, "not_signed_in", L("Sign in to sync presets.")};
    wxGetApp().restart_sync_user_preset();
    return Done{};
}

// ---- Filament slots ----

namespace {

// Why the filament list cannot change, or none: a printer of another technology has none, and a G-code file shown in
// place of a project has its own.
std::optional<Refusal> filament_list_refusal(Plater& plater)
{
    if (plater.printer_technology() != ptFFF || plater.only_gcode_mode() || plater.using_exported_file())
        return Refusal{RefusalKind::Precondition, "filaments_unavailable",
                       L("Filament slots belong to a filament (FFF) project, not to a G-code file.")};
    return std::nullopt;
}

Refusal too_many_filaments() { return {RefusalKind::Precondition, "too_many_filaments", L("No more filament slots can be added.")}; }

Refusal last_filament_slot() { return {RefusalKind::Precondition, "last_filament_slot", L("The only filament slot cannot be deleted.")}; }

Refusal components_and_ratios()
{
    return {RefusalKind::Precondition, "components_and_ratios",
            L("Give the filaments and their ratios together, or neither for the dialog.")};
}

// The filament types of the filaments a mix can mix, the physical ones, a selector each, as the sidebar reads them
// for its mixed filament list (slot 1 first; a preset with no type counts as PLA).
std::vector<std::string> physical_filament_types(Sidebar& sidebar, PresetBundle& presets)
{
    std::vector<std::string> types;
    for (std::size_t slot = 0; slot < sidebar.combos_filament().size(); ++slot) {
        Preset* preset = slot < presets.filament_presets.size() ? presets.filaments.find_preset(presets.filament_presets[slot]) :
                                                                  nullptr;
        std::string display;
        std::string type = preset != nullptr ? preset->config.get_filament_type(display) : std::string();
        types.push_back(type.empty() ? "PLA" : type);
    }
    return types;
}

// What a mixed filament is, as the project stores it: its components, ratios, gradient and colour, to tell whether an
// edit changed it.
std::vector<std::string> mixed_definition(const PresetBundle& bundle, std::size_t index)
{
    std::vector<std::string> out;
    for (const char* key : {"filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient",
                            "filament_mixed_gradient_range", "filament_mixed_gradient_curve", "filament_mixed_gradient_per_part",
                            "filament_colour"}) {
        const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(bundle.project_config.option(key));
        out.push_back(vec != nullptr && index < vec->size() ? vec->vserialize()[index] : std::string());
    }
    return out;
}

} // namespace

Outcome<int> add_filament_slot()
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (std::optional<Refusal> refusal = filament_list_refusal(plater))
        return *refusal;
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const std::size_t   before = bundle.filament_presets.size();
    if (before >= MAXIMUM_EXTRUDER_NUMBER)
        return too_many_filaments();
    // A new physical slot goes after the physical ones; mixed filaments move up by one.
    const int slot = int(bundle.num_physical_filaments()) + 1;
    plater.sidebar().add_filament();
    plater.sidebar().update_filaments_counter();
    if (bundle.filament_presets.size() == before)
        return Refusal{RefusalKind::Failed, "not_added", L("No filament slot was added.")};
    return slot;
}

std::optional<Refusal> filament_delete_refusal(int slots, int slot, std::optional<int> merge_into)
{
    if (slots < 2)
        return last_filament_slot();
    if (slot < 1 || slot > slots)
        // TRN %1% is the number of the last filament slot.
        return Refusal{RefusalKind::Precondition, "no_such_slot", L("Filament slot must be 1 to %1%."), {std::to_string(slots)}};
    if (merge_into && (*merge_into < 1 || *merge_into > slots))
        // TRN %1% is the number of the last filament slot.
        return Refusal{RefusalKind::Precondition, "no_such_slot", L("The slot to merge into must be 1 to %1%."), {std::to_string(slots)}};
    if (merge_into == slot)
        return Refusal{RefusalKind::Precondition, "merge_into_itself", L("A filament slot cannot be merged into itself.")};
    return std::nullopt;
}

Outcome<DeletedFilament> delete_filament_slot(const DeleteFilamentArgs& args)
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (std::optional<Refusal> refusal = filament_list_refusal(plater))
        return *refusal;
    const PresetBundle& bundle   = *wxGetApp().preset_bundle;
    const int           slots    = int(bundle.filament_presets.size());
    const int           physical = int(bundle.num_physical_filaments());
    const int           slot     = args.filament_slot.value_or(physical);
    if (std::optional<Refusal> refusal = filament_delete_refusal(slots, slot, args.merge_into))
        return *refusal;
    // A mixed filament goes as the mixed filament list deletes it, which merges nothing; one physical slot stays,
    // whatever mixed slots there are.
    const bool mixed = bundle.is_mixed_filament(std::size_t(slot) - 1);
    if (mixed && args.merge_into)
        return Refusal{RefusalKind::Precondition, "mixed_filament_slot",
                       // TRN %1% is a filament slot number.
                       L("Filament slot %1% is a mixed filament, which can be deleted but not merged."), {std::to_string(slot)}};
    if (!mixed && physical < 2)
        return last_filament_slot();
    // With one physical filament the sidebar hides the mixed filament list and deletes nothing
    // (Sidebar::delete_filament()), so a mix left then stays until a slot is added.
    if (mixed && physical < 2)
        return Refusal{RefusalKind::Precondition, "mixed_list_hidden",
                       L("With one physical filament the mixed filaments are hidden and cannot be deleted; add a filament slot first.")};
    Sidebar& sidebar = plater.sidebar();
    // A physical slot before another on a non-Bambu Lab printer with more than one nozzle and without
    // single_extruder_multi_material asks before the later slots' tool numbers shift.
    const bool shifts_tools = !mixed && sidebar.delete_shifts_tools(std::size_t(slot) - 1);
    if (shifts_tools && args.confirm_shift == std::optional<bool>(false))
        // TRN %1% is a filament slot number.
        return Refusal{RefusalKind::Precondition, "shifts_tools",
                       L("Deleting filament slot %1% shifts the tool numbers of the slots after it."), {std::to_string(slot)}};
    // Merge with asks before it merges a filament into a mixed filament that mixes it, which leaves the mix broken.
    const bool breaks_mix = args.merge_into &&
                            sidebar.merge_removes_component(std::size_t(slot) - 1, std::size_t(*args.merge_into) - 1);
    if (breaks_mix && args.break_mix == std::optional<bool>(false))
        return Refusal{RefusalKind::Precondition, "mixed_uses_slot",
                       // TRN %1% is a filament slot number, %2% the slot of the mixed filament that mixes it.
                       L("Merging filament slot %1% into the mixed filament of slot %2% removes a filament that mixed filament mixes."),
                       {std::to_string(slot), std::to_string(*args.merge_into)}};
    bool merge_question_cancelled = false;
    if (breaks_mix && args.break_mix.value_or(false))
        sidebar.delete_filament(std::size_t(slot) - 1, *args.merge_into - 1,
                                args.confirm_shift); // what Merge with does after its question's OK
    else if (args.merge_into)
        merge_question_cancelled = !sidebar.change_filament(std::size_t(slot) - 1, std::size_t(*args.merge_into) - 1,
                                                            args.confirm_shift);
    else
        sidebar.delete_filament(std::size_t(slot) - 1, -1, args.confirm_shift);
    sidebar.update_filaments_counter();
    if (int(bundle.filament_presets.size()) == slots) {
        if (merge_question_cancelled)
            // TRN %1% is a filament slot number.
            return Refusal{RefusalKind::Failed, "merge_cancelled", L("Merging filament slot %1% was cancelled."), {std::to_string(slot)}};
        if (shifts_tools && !args.confirm_shift.has_value())
            // TRN %1% is a filament slot number.
            return Refusal{RefusalKind::Failed, "delete_cancelled", L("Deleting filament slot %1% was cancelled."), {std::to_string(slot)}};
        return Refusal{RefusalKind::Failed, "not_deleted", L("The filament slot was not deleted.")};
    }
    DeletedFilament deleted;
    deleted.filament_slot = slot;
    // The slots after the deleted one move down by one.
    if (args.merge_into)
        deleted.merged_into = *args.merge_into > slot ? *args.merge_into - 1 : *args.merge_into;
    for (const std::size_t broken : sidebar.broken_mixed_filaments())
        deleted.broken_mixed.push_back(int(broken) + 1);
    return deleted;
}

std::optional<Refusal> mixed_filament_refusal(const std::vector<std::string>& types, const std::vector<int>& components,
                                              const std::vector<double>& ratios)
{
    const int slots = int(types.size());
    for (auto it = components.begin(); it != components.end(); ++it)
        if (std::find(components.begin(), it, *it) != it)
            return Refusal{RefusalKind::Precondition, "duplicate_component",
                           // TRN %1% is a filament slot number.
                           L("Filament %1% is named twice: a mixed filament mixes each filament once."),
                           {std::to_string(*it)}};
    if (components.size() < 2)
        return Refusal{RefusalKind::Precondition, "too_few_components", L("A mixed filament mixes two filaments or more.")};
    for (int component : components)
        if (component < 1 || component > slots)
            return Refusal{RefusalKind::Precondition, "no_such_slot",
                           // TRN %1% is the filament slot asked for, %2% the number of the last filament that can be mixed.
                           L("Filament %1% does not exist: the filaments that mix are 1 to %2%."),
                           {std::to_string(component), std::to_string(slots)}};
    // The dialog's OK takes filaments of one type; check_mixed_filament_type_consistency() marks another mix broken.
    std::vector<std::string> mixed_types;
    for (int component : components)
        mixed_types.push_back(std::to_string(component) + ": " + types[std::size_t(component - 1)]);
    for (int component : components)
        if (types[std::size_t(component - 1)] != types[std::size_t(components.front() - 1)])
            return Refusal{RefusalKind::Precondition, "mixed_types", L("Cannot mix different filament types"), {}, mixed_types};
    const bool shares = ratios.size() == components.size() &&
                        std::all_of(ratios.begin(), ratios.end(), [](double ratio) { return ratio > 0.; }) &&
                        std::abs(std::accumulate(ratios.begin(), ratios.end(), 0.) - 100.) < 1e-6;
    if (!shares)
        return Refusal{RefusalKind::Precondition, "bad_ratios", L("Give one ratio per filament, as percentages that add up to 100.")};
    return std::nullopt;
}

Outcome<MixedFilament> add_mixed_filament(const AddMixedFilamentArgs& args)
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (std::optional<Refusal> refusal = filament_list_refusal(plater))
        return *refusal;
    if (args.components.empty() != args.ratios.empty())
        return components_and_ratios();
    Sidebar&          sidebar = plater.sidebar();
    PresetBundle&     presets = *wxGetApp().preset_bundle;
    const std::size_t before  = presets.filament_presets.size();
    if (args.components.empty()) {
        // The sidebar offers Add Mixed Filament from two physical filaments on, and up to the last slot.
        if (sidebar.combos_filament().size() < 2)
            return Refusal{RefusalKind::Precondition, "too_few_components", L("A mixed filament mixes two filaments or more.")};
        if (before >= std::size_t(EnforcerBlockerType::ExtruderMax))
            return too_many_filaments();
        sidebar.add_mixed_filament();
        MixedFilament mixed;
        mixed.added         = presets.filament_presets.size() > before;
        mixed.filament_slot = mixed.added ? int(before) + 1 : 0;
        return mixed;
    }
    if (std::optional<Refusal> refusal = mixed_filament_refusal(physical_filament_types(sidebar, presets), args.components, args.ratios))
        return *refusal;
    const std::vector<unsigned int> components(args.components.begin(), args.components.end());
    std::vector<int>                ratios;
    for (double ratio : args.ratios)
        ratios.push_back(int(std::lround(ratio)));
    const int index = sidebar.ensure_mixed_filament(components, ratios);
    if (index < 0 && before >= std::size_t(EnforcerBlockerType::ExtruderMax))
        return too_many_filaments();
    if (index < 0)
        return Refusal{RefusalKind::Failed, "not_mixed", L("No mixed filament was added.")};
    MixedFilament mixed;
    mixed.filament_slot = index + 1;
    mixed.added         = presets.filament_presets.size() > before;
    return mixed;
}

Outcome<EditedMixedFilament> edit_mixed_filament(const EditMixedFilamentArgs& args)
{
    assert(wxThread::IsMain());
    Plater& plater = *wxGetApp().plater();
    if (std::optional<Refusal> refusal = filament_list_refusal(plater))
        return *refusal;
    PresetBundle&     presets = *wxGetApp().preset_bundle;
    const std::size_t slots   = presets.filament_presets.size();
    if (args.filament_slot < 1 || std::size_t(args.filament_slot) > slots)
        return no_such_slot(args.filament_slot, slots);
    const std::size_t index = std::size_t(args.filament_slot) - 1;
    if (!presets.is_mixed_filament(index))
        // TRN %1% is a filament slot number.
        return Refusal{RefusalKind::Precondition, "not_mixed_filament", L("Filament slot %1% is not a mixed filament."),
                       {std::to_string(args.filament_slot)}};
    if (args.components.empty() != args.ratios.empty())
        return components_and_ratios();
    Sidebar&                       sidebar = plater.sidebar();
    const std::vector<std::string> before  = mixed_definition(presets, index);
    if (args.components.empty()) {
        // The mixed filament list's rows, one per mix in slot order; a click on a row's swatch edits it.
        const std::vector<std::size_t> rows = plater.mixed_filament_config_indices();
        sidebar.edit_mixed_filament(std::size_t(std::find(rows.begin(), rows.end(), index) - rows.begin()));
    } else {
        if (std::optional<Refusal> refusal = mixed_filament_refusal(physical_filament_types(sidebar, presets), args.components,
                                                                    args.ratios))
            return *refusal;
        MixedFilamentResult result = sidebar.mixed_filament_result(index);
        result.components.assign(args.components.begin(), args.components.end());
        result.ratios.clear();
        for (double ratio : args.ratios)
            result.ratios.push_back(int(std::lround(ratio)));
        // The dialog shows a gradient for a mix of two filaments only, and turns it off for more.
        if (result.components.size() != 2) {
            result.gradient_enabled  = false;
            result.per_part_gradient = false;
            result.gradient_curve.clear();
        }
        sidebar.apply_mixed_filament_edit(index, result);
    }
    EditedMixedFilament edited;
    edited.filament_slot = args.filament_slot;
    edited.changed       = mixed_definition(presets, index) != before;
    return edited;
}

// ---- Syncing the filament list from the AMS ----

AmsSyncState ams_sync_state()
{
    assert(wxThread::IsMain());
    AmsSyncState   state;
    DeviceManager* devices = wxGetApp().getDeviceManager();
    MachineObject* printer = devices != nullptr ? devices->get_selected_machine() : nullptr;
    Plater*        plater  = wxGetApp().plater();
    if (printer == nullptr || plater == nullptr)
        return state;
    state.printer          = true;
    state.printer_name     = printer->get_dev_name();
    state.selected_printer = into_u8(plater->get_selected_printer_name_in_combox());
    const auto& slots      = wxGetApp().preset_bundle->filament_ams_list;
    state.slots_listed     = !slots.empty();
    state.printer_answered = printer->is_connected() && printer->m_push_count > 0;
    for (const auto& slot : slots)
        if (!slot.second.opt_string("filament_type", 0u).empty() || slot.second.opt_bool("filament_exist", 0u))
            state.filament_loaded = true;
    state.filament_installed = printer->is_filament_installed();
    state.same_printer       = plater->is_same_printer_for_connected_and_selected(/*popup_warning=*/false);
    PartPlateList& plates    = plater->get_partplate_list();
    for (int plate = 0; plate < plates.get_plate_count(); ++plate)
        if (!plates.get_plate(plate)->get_objects_on_this_plate().empty())
            state.project_has_objects = true;
    return state;
}

std::optional<Refusal> ams_sync_refusal(const AmsSyncState& state, std::optional<AmsSyncMode> mode)
{
    if (!state.printer)
        return Refusal{RefusalKind::Precondition, "no_printer", L("Connect a printer to synchronize the AMS filament list.")};
    if (!state.slots_listed) {
        // Only an unreachable printer is not connected: one that has sent its status listed no slot.
        if (state.printer_answered)
            return Refusal{RefusalKind::Precondition, "no_filament_system",
                           // TRN %s is the connected printer's name.
                           L("%s is connected, but no filament system (AMS, CFS or other multi-material unit) was found on it, so "
                             "there are no filaments to synchronize."),
                           {state.printer_name}};
        return Refusal{RefusalKind::Precondition, "not_connected",
                       // TRN %s is the printer's name.
                       L("OrcaSlicer can't connect to %s. Please check if the printer is powered on and connected to the network."),
                       {state.selected_printer}};
    }
    if (!state.filament_loaded) {
        if (!state.filament_installed)
            return Refusal{RefusalKind::Precondition, "no_filament",
                           L("There are no filaments on the printer. Please load the filaments on the printer first.")};
        return Refusal{RefusalKind::Precondition, "unknown_filaments",
                       L("The filaments on the printer are all unknown types. Please go to the printer screen or software device page "
                         "to set the filament type.")};
    }
    if (!state.same_printer)
        return Refusal{RefusalKind::Precondition, "other_printer",
                       // TRN Both %s are the printer selected in the printer selector.
                       L("The currently connected printer on the device page is not %s. Please switch to %s before syncing."),
                       {state.selected_printer, state.selected_printer}};
    if (mode == AmsSyncMode::Map && !state.project_has_objects)
        return Refusal{RefusalKind::Precondition, "nothing_to_map",
                       L("Map takes a project with objects on a plate; with none, the filament list is overwritten.")};
    return std::nullopt;
}

Outcome<AmsSynced> apply_ams_sync(const AmsSyncChoice& choice)
{
    assert(wxThread::IsMain());
    Plater&                         plater  = *wxGetApp().plater();
    const Sidebar::AmsSyncApplied   applied = plater.sidebar().apply_ams_sync(choice.direct_sync, choice.sync_maps,
                                                                              choice.every_slot_changed, choice.unknowns_found);
    if (applied.filaments == 0) {
        if (choice.none_compatible)
            choice.none_compatible(applied.detail);
        return Refusal{RefusalKind::Failed, "no_compatible_filaments", L("There are no compatible filaments, and sync is not performed."),
                       {}, applied.unknowns};
    }
    AmsSynced           synced;
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    synced.presets             = bundle.filament_presets;
    if (const auto* colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour"))
        synced.colours = colours->values;
    synced.unknowns = applied.unknowns;
    return synced;
}

Outcome<AmsSynced> sync_filaments_from_ams(AmsSyncMode mode)
{
    assert(wxThread::IsMain());
    DeviceManager* devices = wxGetApp().getDeviceManager();
    MachineObject* printer = devices != nullptr ? devices->get_selected_machine() : nullptr;
    // The printer's slots, read again, as the sync reads them first.
    if (printer != nullptr)
        wxGetApp().plater()->sidebar().load_ams_list(printer);
    if (std::optional<Refusal> refusal = ams_sync_refusal(ams_sync_state(), mode))
        return *refusal;
    // Synchronize now, which the dialog answers wxID_YES: every slot counts as changed.
    AmsSyncChoice choice;
    if (mode == AmsSyncMode::Map) {
        std::vector<FilamentInfo> mapped;
        map_filaments_to_ams(printer, ams_sync_filaments(), mapped);
        choice.direct_sync = false;
        choice.sync_maps   = ams_sync_maps(mapped);
    }
    return apply_ams_sync(choice);
}

}}} // namespace Slic3r::GUI::Ops
