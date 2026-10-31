#include "GeometryImportDialog.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>
#include <string>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <miniz.h>
#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/ImportNames.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r::GUI {
namespace {

class ImportDialog final : public DPIDialog {
public:
    using DPIDialog::DPIDialog;

protected:
    void on_dpi_changed(const wxRect&) override {
        // Respect the controls' new minimum size when moved between DPI settings.
        if (GetSizer() != nullptr)
            GetSizer()->SetSizeHints(this);
        Layout();
    }
};

std::string option_key(const std::string& format, const char* suffix)
{
    return "import_names_" + format + "_" + suffix;
}

bool config_bool(const AppConfig& config, const std::string& key, bool fallback)
{
    // Existing user configurations predate these keys.
    return config.has(key) ? config.get_bool(key) : fallback;
}

// Same font and label color as the check boxes of the STEP import dialog.
void apply_label_style(wxWindow* control)
{
    control->SetFont(::Label::Body_14);
    control->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
}

// White dialog with the explanation in a grey rounded box, as in the STEP import dialog.
void add_tip(DPIDialog& dialog, wxBoxSizer& sizer, const wxString& text, int wrap_width)
{
    dialog.SetBackgroundColour(*wxWHITE);
    const wxColour tip_colour = StateColor::darkModeColorFor(wxColour("#F1F1F1"));
    auto* frame = new RoundedRectangle(&dialog, tip_colour, wxDefaultPosition, wxSize(-1, -1), 6, 0);
    auto* tip_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* info = new wxStaticText(frame, wxID_ANY, text);
    apply_label_style(info);
    info->SetBackgroundColour(tip_colour);
    info->Wrap(dialog.FromDIP(wrap_width));
    tip_sizer->Add(info, 0, wxEXPAND | wxALL, dialog.FromDIP(10));
    frame->SetSizer(tip_sizer);
    frame->Layout();
    sizer.Add(frame, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
}

ImportCheckBox add_checkbox(DPIDialog& dialog, wxBoxSizer& sizer, const wxString& label, bool value)
{
    return add_import_checkbox(&dialog, sizer, label, value, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
}

// The radio group takes the dialog background at creation, so add it after add_tip.
RadioGroup* add_radio_group(DPIDialog& dialog, const std::vector<wxString>& labels, int selection, long direction)
{
    auto* group = new RadioGroup(&dialog, labels, direction);
    group->SetSelection(selection);
    return group;
}

using NameGroup = std::vector<std::string*>;

// Each group has its own collision scope: object siblings or parts of one object.
std::vector<NameGroup> name_groups(const std::vector<Model*>& models)
{
    std::vector<NameGroup> groups;
    for (Model* model : models) {
        NameGroup objects;
        for (ModelObject* object : model->objects) {
            objects.push_back(&object->name);
            NameGroup parts;
            for (ModelVolume* volume : object->volumes)
                parts.push_back(&volume->name);
            groups.push_back(std::move(parts));
        }
        groups.push_back(std::move(objects));
    }
    return groups;
}

std::vector<std::vector<std::string>> cleaned_groups(const std::vector<NameGroup>& groups, const std::string& replacement)
{
    std::vector<std::vector<std::string>> results;
    results.reserve(groups.size());
    for (const NameGroup& group : groups) {
        std::vector<std::string> names;
        names.reserve(group.size());
        for (const std::string* name : group)
            names.push_back(*name);
        results.push_back(sanitize_import_names(names, replacement));
    }
    return results;
}

} // namespace

std::string geometry_import_format(const std::string& path)
{
    if (boost::algorithm::iends_with(path, ".step") || boost::algorithm::iends_with(path, ".stp"))
        return "step";
    if (boost::algorithm::iends_with(path, ".amf"))
        return "amf";
    if (boost::algorithm::iends_with(path, ".3mf"))
        return "3mf";
    return {};
}

GeometryImportOptions geometry_import_options(const AppConfig& config, const std::string& format)
{
    GeometryImportOptions options;
    options.keep_names = config_bool(config, option_key(format, "keep"), true);
    options.sort_names = config_bool(config, option_key(format, "sort"), false);
    // A 3MF keeps the structure written in it, so it has no presentation choice.
    options.load_as_assembly = format == "3mf" || config_bool(config, option_key(format, "assembly"), true);
    // Files from other slicers keep a whole plate in one AMF group, so assembling it is opt-in.
    options.separate_objects = format == "amf" && config_bool(config, option_key(format, "separate"), true);
    // The same key drives Preferences, Replace and Reload, so all STEP paths split alike.
    options.split_compound = format == "step" && config_bool(config, "is_split_compound", true);
    options.show_dialog = config_bool(config, format == "step" ? "enable_step_mesh_setting" : option_key(format, "show_dialog"), true);
    // Read the same quality defaults as the existing STEP import dialog.
    options.linear_deflection = string_to_double_decimal_point(config.get("linear_deflection"));
    options.angle_deflection = string_to_double_decimal_point(config.get("angle_deflection"));
    if (!std::isfinite(options.linear_deflection) || options.linear_deflection < 0.001 || options.linear_deflection > 0.1)
        options.linear_deflection = 0.003;
    if (!std::isfinite(options.angle_deflection) || options.angle_deflection < 0.01 || options.angle_deflection > 1.0)
        options.angle_deflection = 0.5;
    return options;
}

bool is_geometry_only_3mf(const std::string& path)
{
    if (!boost::algorithm::iends_with(path, ".3mf"))
        return false;
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, path))
        return false; // Let the normal reader report unreadable or malformed archives.
    ScopeGuard close_archive([&archive]() { close_zip_reader(&archive); });

    // Minimal published Orca projects also retain model_settings.config.
    const char* project_markers[] = {
        "Metadata/model_settings.config", "Metadata/project_settings.config", "Metadata/print_profile.config",
        "Metadata/slice_info.config", "Metadata/Slic3r_PE.config", "Metadata/Slic3r_PE_model.config",
        "Metadata/Prusa_Slicer_layer_config_ranges.xml", "Metadata/Slic3r_PE_layer_heights_profile.txt",
        "Metadata/Slic3r_PE_sla_support_points.txt", "Metadata/Slic3r_PE_sla_drain_holes.txt"
    };
    bool has_model = false;
    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&archive); ++index) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, index, &stat))
            return false;
        std::string name = stat.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        while (boost::algorithm::starts_with(name, "./"))
            name.erase(0, 2);
        for (const char* marker : project_markers)
            if (boost::algorithm::iequals(name, marker))
                return false;
        has_model = has_model || (boost::algorithm::istarts_with(name, "3D/") && boost::algorithm::iends_with(name, ".model"));
    }
    return has_model;
}

GeometryImportNamePolicy geometry_import_name_policy(const AppConfig& config)
{
    GeometryImportNamePolicy policy;
    policy.show_dialog = config_bool(config, "import_names_cleanup_show_dialog", true);
    policy.remove = config_bool(config, "import_names_cleanup_remove", false);
    if (config.has("import_names_cleanup_replacement"))
        policy.replacement = config.get("import_names_cleanup_replacement");
    return policy;
}

bool ImportCheckBox::GetValue() const { return box->GetValue(); }

void ImportCheckBox::SetValue(bool value) const { box->SetValue(value); }

void ImportCheckBox::Enable(bool enable) const
{
    box->Enable(enable);
    label->Enable(enable);
    // Same disabled text color as RadioGroup.
    label->SetForegroundColour(enable ? StateColor::darkModeColorFor(wxColour("#363636")) : wxColour("#6B6A6A"));
    label->Refresh();
}

void ImportCheckBox::SetToolTip(const wxString& tip) const
{
    box->SetToolTip(tip);
    label->SetToolTip(tip);
}

ImportCheckBox add_import_checkbox(wxWindow* parent, wxSizer& sizer, const wxString& text, bool value, int flags, int border)
{
    ImportCheckBox checkbox{new ::CheckBox(parent), new wxStaticText(parent, wxID_ANY, text)};
    apply_label_style(checkbox.label);
    checkbox.box->SetValue(value);
    ::CheckBox* box = checkbox.box;
    auto toggle = [box]() {
        if (!box->IsEnabled())
            return;
        box->SetValue(!box->GetValue());
        wxCommandEvent event(wxEVT_TOGGLEBUTTON, box->GetId());
        event.SetEventObject(box);
        box->GetEventHandler()->ProcessEvent(event);
    };
    // A double click arrives as one down and one double-click event, so each toggles once.
    checkbox.label->Bind(wxEVT_LEFT_DOWN, [toggle](wxMouseEvent&) { toggle(); });
    checkbox.label->Bind(wxEVT_LEFT_DCLICK, [toggle](wxMouseEvent&) { toggle(); });
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(checkbox.box, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(checkbox.label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(5));
    sizer.Add(row, 0, flags, border);
    return checkbox;
}

// Sorting only makes sense for original names; while Keep is off it is disabled but keeps its remembered value.
void link_sort_to_keep(const ImportCheckBox& keep, const ImportCheckBox& sort)
{
    sort.Enable(keep.GetValue());
    keep.box->Bind(wxEVT_TOGGLEBUTTON, [keep, sort](wxCommandEvent& event) {
        sort.Enable(keep.GetValue());
        event.Skip();
    });
}

bool show_geometry_import_options(wxWindow* parent, const std::string& format, GeometryImportOptions& options)
{
    if (options.chosen)
        return true;
    options.chosen = true;
    if (!options.show_dialog)
        return true;

    // One choice per format applies to all of its files in this import operation.
    ImportDialog dialog(parent, wxID_ANY, format == "3mf" ? _L("3MF geometry import options") : _L("AMF geometry import options"), wxDefaultPosition, wxDefaultSize,
                     wxDEFAULT_DIALOG_STYLE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    // A 3MF keeps its own structure, so only the naming choices remain for it.
    add_tip(dialog, *sizer, format == "3mf" ? _L("Choose how to name and order the parts of this file.") :
                                              _L("Choose how to load the bodies in this file."), 430);

    // Item 0 loads separate objects, item 1 an assembly, item 2 a single object.
    RadioGroup* load_mode = nullptr;
    if (format != "3mf") {
        load_mode = add_radio_group(dialog, {_L("Separate objects (as before)"), _L("Load as an assembly"), _L("Load as a single object (can be split later)")},
                                    options.separate_objects ? 0 : options.load_as_assembly ? 1 : 2, wxVERTICAL);
        load_mode->SetRadioTooltip(0, _L("Each object in the file is loaded on its own, as OrcaSlicer always loaded AMF files."));
        load_mode->SetRadioTooltip(1, _L("The bodies of each group in the file become parts of one object, in their relative positions."));
        load_mode->SetRadioTooltip(2, _L("The original bodies and names are kept inside the object. Use Split to parts to show them as separate parts."));
        sizer->Add(load_mode, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
    }
    const auto keep = add_checkbox(dialog, *sizer, _L("Keep original names"), options.keep_names);
    keep.SetToolTip(_L("Use the body names stored in the file, when there are any. When disabled, the usual OrcaSlicer names are used; existing project names are kept."));
    const auto sort = add_checkbox(dialog, *sizer, _L("Sort parts by name"), options.sort_names);
    sort.SetToolTip(_L("Sort the parts inside each imported assembly in the object list. This does not change the order used for slicing. Available when Keep original names is on."));
    link_sort_to_keep(keep, sort);
    const auto remember = add_checkbox(dialog, *sizer, _L("Remember my choice"), false);
    remember.SetToolTip(_L("Use these choices as defaults next time. The dialog still opens."));
    const auto dont_ask = add_checkbox(dialog, *sizer, _L("Don't ask again"), false);
    dont_ask.SetToolTip(_L("Save these choices and apply them without asking next time. You can show this dialog again in Preferences."));

    auto* buttons = new DialogButtons(&dialog, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    dialog.SetSizerAndFit(sizer);
    // DPIDialog centres itself before the sizer sets the final size.
    dialog.CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    options.keep_names = keep.GetValue();
    options.sort_names = sort.GetValue();
    if (load_mode != nullptr) {
        options.separate_objects = load_mode->GetSelection() == 0;
        // Separate objects leave the remembered assembly choice as it was.
        if (!options.separate_objects)
            options.load_as_assembly = load_mode->GetSelection() == 1;
    }
    options.remember = remember.GetValue();
    options.dont_ask = dont_ask.GetValue();
    return true;
}

bool prepare_geometry_import_names(wxWindow* parent, const std::vector<Model*>& models, GeometryImportNamePolicy& policy)
{
    const std::vector<NameGroup> groups = name_groups(models);
    bool needs_cleanup = false;
    try {
        for (const NameGroup& group : groups)
            for (const std::string* name : group)
                needs_cleanup = has_unsupported_name_characters(*name) || needs_cleanup;
    } catch (const std::invalid_argument&) {
        MessageDialog(parent, _L("A name in the file is not valid Unicode. The geometry was not imported."),
                      _L("Invalid name"), wxOK | wxICON_ERROR).ShowModal();
        return false;
    }
    if (!needs_cleanup)
        return true;
    policy.used = true;

    // A damaged saved replacement must not silently create more invalid names.
    if (!policy.remove && !valid_import_name_replacement(policy.replacement)) {
        policy.replacement = "_";
        policy.show_dialog = true;
    }

    if (policy.show_dialog) {
        ImportDialog dialog(parent, wxID_ANY, _L("Unsupported characters in names"), wxDefaultPosition, wxDefaultSize,
                         wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        add_tip(dialog, *sizer, _L("Some original names contain characters that OrcaSlicer cannot use. Choose how to change them before importing."), 520);
        // Item 0 removes the characters, item 1 replaces them with the text right after it.
        auto* rule = add_radio_group(dialog, {_L("Remove unsupported characters"), _L("Replace with:")}, policy.remove ? 0 : 1, wxHORIZONTAL);
        rule->SetRadioTooltip(1, _L("Each unsupported character is replaced with this text. It may be longer than one character."));
        auto* replacement_row = new wxBoxSizer(wxHORIZONTAL);
        replacement_row->Add(rule, 0, wxALIGN_CENTER_VERTICAL);
        auto* replacement = new ::TextInput(&dialog, from_u8(policy.replacement));
        replacement->GetTextCtrl()->SetFont(::Label::Body_14);
        replacement_row->Add(replacement, 1, wxALIGN_CENTER_VERTICAL);
        sizer->Add(replacement_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
        auto* preview = new wxTextCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, dialog.FromDIP(wxSize(540, 180)),
                                       wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        apply_label_style(preview);
        sizer->Add(preview, 1, wxEXPAND | wxALL, dialog.FromDIP(12));
        const auto remember = add_checkbox(dialog, *sizer, _L("Remember my choice"), false);
        remember.SetToolTip(_L("Use this rule as the default next time. The dialog still opens."));
        const auto dont_ask = add_checkbox(dialog, *sizer, _L("Don't ask again"), false);
        dont_ask.SetToolTip(_L("Save this rule and apply it without asking next time. You can show this dialog again in Preferences."));
        auto* buttons = new DialogButtons(&dialog, {"OK", "Cancel"});

        auto update_preview = [&]() {
            const bool remove = rule->GetSelection() == 0;
            replacement->Enable(!remove);
            const std::string text = into_u8(replacement->GetTextCtrl()->GetValue());
            const bool valid = remove || valid_import_name_replacement(text);
            buttons->GetOK()->Enable(valid);
            if (!valid) {
                preview->ChangeValue(_L("Enter a non-empty replacement containing only supported characters, or choose removal."));
                return;
            }
            const auto results = cleaned_groups(groups, remove ? std::string() : text);
            wxString lines;
            for (size_t group = 0; group < groups.size(); ++group)
                for (size_t index = 0; index < groups[group].size(); ++index)
                    if (*groups[group][index] != results[group][index]) {
                        // TRN The placeholders are the original name and its imported replacement.
                        lines += format_wxstr(_L("%1% → %2%"), from_u8(*groups[group][index]), from_u8(results[group][index])) + "\n";
                    }
            preview->ChangeValue(lines);
        };
        rule->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [&](wxCommandEvent&) { update_preview(); });
        replacement->Bind(wxEVT_TEXT, [&](wxCommandEvent&) { update_preview(); });
        buttons->GetOK()->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
        sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
        dialog.SetSizerAndFit(sizer);
        // DPIDialog centres itself before the sizer sets the final size.
        dialog.CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(&dialog);
        update_preview();
        if (dialog.ShowModal() != wxID_OK)
            return false;
        policy.remove = rule->GetSelection() == 0;
        policy.replacement = into_u8(replacement->GetTextCtrl()->GetValue());
        policy.remember = remember.GetValue();
        policy.dont_ask = dont_ask.GetValue();
    }

    const auto results = cleaned_groups(groups, policy.remove ? std::string() : policy.replacement);
    for (size_t group = 0; group < groups.size(); ++group)
        for (size_t index = 0; index < groups[group].size(); ++index)
            *groups[group][index] = results[group][index];
    return true;
}

void save_geometry_import_options(AppConfig& config, const std::string& format, const GeometryImportOptions& options)
{
    if (!options.chosen || (!options.remember && !options.dont_ask))
        return;
    config.set_bool(option_key(format, "keep"), options.keep_names);
    // A disabled sorting choice (Keep off) leaves the remembered one untouched.
    if (options.keep_names)
        config.set_bool(option_key(format, "sort"), options.sort_names);
    if (format != "3mf")
        config.set_bool(option_key(format, "assembly"), options.load_as_assembly);
    if (format == "amf")
        config.set_bool(option_key(format, "separate"), options.separate_objects);
    if (format == "step")
        config.set_bool("is_split_compound", options.split_compound);
    if (options.dont_ask)
        config.set_bool(format == "step" ? "enable_step_mesh_setting" : option_key(format, "show_dialog"), false);
    if (format == "step") {
        config.set("linear_deflection", float_to_string_decimal_point(options.linear_deflection, 3));
        config.set("angle_deflection", float_to_string_decimal_point(options.angle_deflection, 2));
    }
}

void save_geometry_import_name_policy(AppConfig& config, const GeometryImportNamePolicy& policy)
{
    if (!policy.used || (!policy.remember && !policy.dont_ask))
        return;
    config.set_bool("import_names_cleanup_remove", policy.remove);
    config.set("import_names_cleanup_replacement", policy.replacement);
    if (policy.dont_ask)
        config.set_bool("import_names_cleanup_show_dialog", false);
}

} // namespace Slic3r::GUI
