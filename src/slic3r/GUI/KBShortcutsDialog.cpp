#include "libslic3r/libslic3r.h"
#include "KBShortcutsDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "Notebook.hpp"
#include <wx/gbsizer.h>
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <algorithm>
#include <set>
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Preferences.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/TabCtrl.hpp"
#include <wx/notebook.h>

namespace Slic3r {
namespace GUI {

namespace {

wxString shortcut_names(const std::vector<Shortcut>& shortcuts)
{
    wxString names;
    for (Shortcut shortcut : shortcuts) {
        if (!names.empty())
            names += ", ";
        names += _(shortcut_info(shortcut).name);
    }
    return names;
}

const wxColour ERROR_COLOUR("#D01B1B");

// The camera action a mouse button drags, as set in Preferences > Control.
const char* mouse_action(const char* preference)
{
    const std::string action = wxGetApp().app_config->get(preference);
    return action == "1" ? L("Pan View") : action == "2" ? L("Rotate View") : L("None");
}

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// Heading icon of each ShortcutSection, in enum order.
constexpr std::array<const char*, size_t(ShortcutSection::Count)> SECTION_ICONS{
    "param_information",       // Project
    "printer",                 // Slicing and printing
    "param_precision",         // Selection
    "param_wall",              // Editing
    "param_printable_space",   // Objects
    "plate_arrange",           // Placement
    "toolbar_scale",           // Gizmos
    "param_layer_height",      // Sliders
    "objlist_color_painting",  // Painting tools
    "param_position",          // Camera
    "im_visible",              // Display
    "param_settings",          // Application
};

} // namespace

KBShortcutsDialog::KBShortcutsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Keyboard Shortcuts"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    fill_pages();

    // The page tabs follow the Preferences dialog.
    m_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS | wxTR_FULL_ROW_HIGHLIGHT);
    m_tabs->Bind(wxEVT_RIGHT_DOWN, [](auto&) {});
    m_tabs->SetFont(Label::Body_14);
    m_simplebook = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(620), FromDIP(500)));
    for (const Page& page : m_pages) {
        m_tabs->AppendItem(page.title);
        m_simplebook->AddPage(create_page(m_simplebook, page), page.title);
    }
    const StateColor tab_colour(std::make_pair(wxColour("#6B6B6C"), (int) StateColor::NotChecked), std::make_pair(wxColour("#363636"), (int) StateColor::Normal));
    for (size_t i = 0; i < m_tabs->GetCount(); ++i)
        m_tabs->SetItemTextColour(i, tab_colour);
    m_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent& e) {
        for (size_t i = 0; i < m_tabs->GetCount(); ++i)
            m_tabs->SetItemBold(i, int(i) == e.GetSelection());
        m_simplebook->SetSelection(e.GetSelection());
    });
    m_tabs->SelectItem(0);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_tabs, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(5));
    sizer->Add(m_simplebook, 1, wxEXPAND);
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void KBShortcutsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    m_tabs->Rescale();
    Layout();
    Fit();
    Refresh();
}

void KBShortcutsDialog::fill_pages()
{
    // A fixed row is listed in the section of the shortcuts it belongs with.
    auto fixed = [](ShortcutSection section, const wxString& key, const char* description) { return Row{ FixedKey{ key, description }, section }; };
    auto mouse = [](ShortcutSection section, const wxString& button, const char* preference) { return Row{ MouseAction{ button, preference }, section }; };
    auto key   = [](const std::string& key) { return _L_CONTEXT(key, "Keyboard Shortcut"); };
    auto page  = [this](const wxString& title, const wxString& caption, ShortcutContext context, std::vector<Row> fixed_rows) {
        Page entry{ title, caption, {} };
        for (Shortcut shortcut : shortcuts_in(context))
            entry.rows.push_back({ shortcut, shortcut_section(shortcut) });
        entry.rows.insert(entry.rows.end(), fixed_rows.begin(), fixed_rows.end());
        std::stable_sort(entry.rows.begin(), entry.rows.end(), [](const Row& a, const Row& b) { return a.section < b.section; });
        m_pages.push_back(std::move(entry));
    };

    const wxString ctrl        = from_u8(KeyChord::modifier_prefix(wxMOD_CONTROL));
    const wxString alt         = from_u8(KeyChord::modifier_prefix(wxMOD_ALT));
    const wxString shift       = from_u8(KeyChord::modifier_prefix(wxMOD_SHIFT));
    const wxString any_key     = key(L_CONTEXT("Key", "Keyboard Shortcut"));   // the key the row's shortcut is bound to
    const wxString esc         = key(L_CONTEXT("Esc", "Keyboard Shortcut"));
    const wxString left_button = _L("Left mouse button");
    const wxString wheel       = _L("Mouse wheel");
    using Section              = ShortcutSection;

    if (wxGetApp().is_editor()) {
        page(_L("Global"), _L("Available anywhere in the window, even while typing in a text field."), ShortcutContext::Global, {
            fixed(Section::Application, ctrl + key(L_CONTEXT("Tab", "Keyboard Shortcut")), L("Switch table page")),
        });

        page(_L("Prepare"), _L("Available while the 3D view on the Prepare tab has focus."), ShortcutContext::Plater, {
            fixed(Section::Selection, alt + left_button, L("Select a part")),
            fixed(Section::Selection, ctrl + left_button, L("Select multiple objects")),
            fixed(Section::Selection, shift + left_button, L("Select objects by rectangle")),
            fixed(Section::Selection, esc, L("Deselect All")),
            fixed(Section::Objects, "1-9", L("Keyboard 1-9: set filament for object/part")),
            fixed(Section::Placement, shift + any_key, L("Movement step set to 1mm")),
            fixed(Section::Placement, ctrl + any_key, L("Movement in camera space")),
            mouse(Section::Camera, left_button, "left_mouse_drag_action"),
            mouse(Section::Camera, _L("Middle mouse button"), "middle_mouse_drag_action"),
            mouse(Section::Camera, _L("Right mouse button"), "right_mouse_drag_action"),
            fixed(Section::Camera, wheel, L("Zoom View")),
        });

        page(_L("Painting"), _L("Available while a painting gizmo is open: supports, seam, fuzzy skin or color painting."), ShortcutContext::Painting, {
            fixed(Section::Gizmos, esc, L("Deselect All")),
            fixed(Section::Gizmos, shift + left_button, L("Move: press to snap by 1mm")),
            fixed(Section::PaintingTools, ctrl + wheel, L("Support/Color Painting: adjust pen radius")),
            fixed(Section::PaintingTools, alt + wheel, L("Support/Color Painting: adjust section position")),
        });

        page(_L("Objects list"), _L("Available while the object list has focus."), ShortcutContext::ObjectList, {
            fixed(Section::Selection, esc, L("Deselect All")),
            fixed(Section::Objects, "1-9", L("Set extruder number for the objects and parts")),
            fixed(Section::Objects, key(L_CONTEXT("Space", "Keyboard Shortcut")), L("Select the object/part and press space to change the name")),
            fixed(Section::Objects, _L("Mouse click"), L("Select the object/part and mouse click to change the name")),
        });
    }

    page(_L("Preview"), _L("Available while the 3D view on the Preview tab has focus."), ShortcutContext::Preview, {
        fixed(Section::Sliders, shift + any_key, L("Move slider 5x faster")),
        fixed(Section::Sliders, ctrl + any_key, L("Move slider 5x faster")),
        fixed(Section::Sliders, shift + wheel, L("Move slider 5x faster")),
        fixed(Section::Sliders, ctrl + wheel, L("Move slider 5x faster")),
    });
}

wxPanel* KBShortcutsDialog::create_page(wxWindow* parent, const Page& page)
{
    wxPanel* main_page = new wxPanel(parent);
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    wxScrolledWindow *scrollable_panel = new wxScrolledWindow(main_page);
    wxGetApp().UpdateDarkUI(scrollable_panel);
    const wxColour page_colour = StateColor::darkModeColorFor(*wxWHITE);
    scrollable_panel->SetBackgroundColour(page_colour);
    scrollable_panel->SetScrollRate(0, 20);
    const int page_width = FromDIP(600);
    const int margin     = FromDIP(20);
    scrollable_panel->SetInitialSize(wxSize(page_width, FromDIP(450)));

    wxBoxSizer* scrollable_panel_sizer = new wxBoxSizer(wxVERTICAL);

    const wxColour note_colour = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    const wxColour note_text   = StateColor::darkModeColorFor(wxColour("#6B6B6C"));
    StaticBox* note = new StaticBox(scrollable_panel);
    note->SetCornerRadius(FromDIP(4));
    note->SetBorderWidth(0);
    note->SetBackgroundColor(note_colour);
    note->SetBackgroundColour(note_colour);
    auto note_icon = new wxStaticBitmap(note, wxID_ANY, ScalableBitmap(note, "help", 16).bmp());
    auto note_text_ctrl = new wxStaticText(note, wxID_ANY, page.caption);
    note_text_ctrl->SetFont(Label::Body_13);
    note_text_ctrl->SetForegroundColour(note_text);
    note_text_ctrl->SetBackgroundColour(note_colour);
    note_text_ctrl->Wrap(page_width - 2 * margin - FromDIP(10 + 16 + 8 + 10));
    wxBoxSizer* note_sizer = new wxBoxSizer(wxHORIZONTAL);
    note_sizer->Add(note_icon, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, FromDIP(10));
    note_sizer->Add(note_text_ctrl, 1, wxALIGN_CENTRE_VERTICAL | wxALL, FromDIP(8));
    note->SetSizer(note_sizer);
    scrollable_panel_sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    auto key_text = [](const Row& row) {
        return std::visit(overloaded{
            [](Shortcut shortcut) { return from_u8(wxGetApp().shortcuts().display(shortcut)); },
            [](const FixedKey& fixed) { return fixed.key; },
            [](const MouseAction& mouse) { return mouse.button; },
        }, row.content);
    };
    auto description = [](const Row& row) {
        return std::visit(overloaded{
            [](Shortcut shortcut) { return _(shortcut_info(shortcut).name); },
            [](const FixedKey& fixed) { return _(fixed.description); },
            [](const MouseAction& mouse) { return _(mouse_action(mouse.preference)); },
        }, row.content);
    };

    // Descriptions wrap to the width the widest binding on the page allows.
    int key_width = 0;
    for (const Row& row : page.rows) {
        int width = 0;
        scrollable_panel->GetTextExtent(key_text(row), &width, nullptr, nullptr, nullptr, &Label::Head_14);
        key_width = std::max(key_width, width);
    }
    const int indent     = FromDIP(18) + 5;   // the heading icon and its gap in StaticLine, so rows align with the heading text
    const int desc_width = page_width - key_width - 2 * margin - indent - FromDIP(16 + 16 + 44 + 20);

    // Headings span the three columns (description, key, buttons) of one grid, so the key
    // column lines up across sections.
    const wxColour                 text_colour(50, 58, 61);
    wxGridBagSizer*                grid_sizer = new wxGridBagSizer(FromDIP(4), FromDIP(16));
    std::optional<ShortcutSection> section;
    int                            grid_row = 0;
    for (const Row& row : page.rows) {
        if (section != row.section) {
            auto heading = new StaticLine(scrollable_panel, false, _(section_name(row.section)), SECTION_ICONS[size_t(row.section)]);
            heading->SetFont(Label::Head_14);
            heading->SetForegroundColour(text_colour);
            wxBoxSizer* heading_sizer = new wxBoxSizer(wxVERTICAL);
            heading_sizer->Add(heading, 0, wxEXPAND | wxTOP, section.has_value() ? FromDIP(16) : 0);
            grid_sizer->Add(heading_sizer, wxGBPosition(grid_row++, 0), wxGBSpan(1, 3), wxEXPAND | wxBOTTOM, FromDIP(4));
            section = row.section;
        }
        auto desc = new wxStaticText(scrollable_panel, wxID_ANY, description(row));
        desc->SetFont(Label::Body_14);
        desc->SetForegroundColour(text_colour);
        desc->Wrap(desc_width);
        grid_sizer->Add(desc, wxGBPosition(grid_row, 0), wxDefaultSpan, wxALIGN_CENTRE_VERTICAL | wxLEFT, indent);

        auto key = new wxStaticText(scrollable_panel, wxID_ANY, key_text(row));
        key->SetForegroundColour(text_colour);
        key->SetFont(Label::Head_14);
        grid_sizer->Add(key, wxGBPosition(grid_row, 1), wxDefaultSpan, wxALIGN_CENTRE_VERTICAL | wxALIGN_RIGHT);

        const wxGBPosition buttons_cell(grid_row++, 2);
        if (const MouseAction* mouse = std::get_if<MouseAction>(&row.content)) {
            auto settings = new ScalableButton(scrollable_panel, wxID_ANY, "settings");
            settings->SetBackgroundColour(page_colour);
            settings->SetToolTip(_L("Preferences"));
            settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_mouse_preferences(); });
            grid_sizer->Add(settings, buttons_cell, wxDefaultSpan, wxALIGN_CENTRE_VERTICAL);
            m_preference_rows.push_back({ mouse->preference, desc });
            continue;
        }
        const Shortcut* editable = std::get_if<Shortcut>(&row.content);
        if (editable == nullptr)
            continue;
        const Shortcut shortcut = *editable;
        wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto change = new ScalableButton(scrollable_panel, wxID_ANY, "edit");
        change->SetBackgroundColour(page_colour);
        change->SetToolTip(_L("Edit"));
        change->Bind(wxEVT_BUTTON, [this, shortcut](wxCommandEvent&) { edit_shortcut(shortcut); });
        auto reset = new ScalableButton(scrollable_panel, wxID_ANY, "undo");
        reset->SetBackgroundColour(page_colour);
        reset->SetToolTip(_L("Reset"));
        reset->Bind(wxEVT_BUTTON, [this, shortcut](wxCommandEvent&) { reset_shortcut(shortcut); });
        reset->Show(wxGetApp().shortcuts().is_customized(shortcut));
        buttons->Add(change, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(6));
        buttons->Add(reset, 0, wxALIGN_CENTRE_VERTICAL | wxRESERVE_SPACE_EVEN_IF_HIDDEN);
        grid_sizer->Add(buttons, buttons_cell, wxDefaultSpan, wxALIGN_CENTRE_VERTICAL);
        m_editable_rows.push_back({ shortcut, key, reset });
    }
    grid_sizer->AddGrowableCol(0, 1);
    scrollable_panel_sizer->Add(grid_sizer, 0, wxEXPAND | wxALL, margin);
    scrollable_panel->SetSizer(scrollable_panel_sizer);

    main_sizer->Add(scrollable_panel, 1, wxEXPAND);
    main_page->SetSizer(main_sizer);

    return main_page;
}

void KBShortcutsDialog::edit_shortcut(Shortcut shortcut)
{
    ShortcutCaptureDialog dlg(this, shortcut);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const wxString question = wxString::Format(_L("%s is already used by: %s. Assign it here and remove it from those shortcuts?"), from_u8(dlg.chord().display()), shortcut_names(dlg.conflicts()));
    if (!take_chord_from(shortcut, dlg.conflicts(), question))
        return;
    wxGetApp().shortcuts().bind(shortcut, dlg.chord());
    apply_bindings();
}

void KBShortcutsDialog::reset_shortcut(Shortcut shortcut)
{
    const std::vector<Shortcut> conflicts = wxGetApp().shortcuts().conflicts(shortcut, shortcut_info(shortcut).default_chord);
    const wxString question = wxString::Format(_L("The default is already used by: %s. Resetting removes it from those shortcuts."), shortcut_names(conflicts));
    if (!take_chord_from(shortcut, conflicts, question))
        return;
    wxGetApp().shortcuts().reset(shortcut);
    apply_bindings();
}

bool KBShortcutsDialog::take_chord_from(Shortcut shortcut, const std::vector<Shortcut>& conflicts, const wxString& question)
{
    if (conflicts.empty())
        return true;
    MessageDialog confirm(this, question, _(shortcut_info(shortcut).name), wxICON_QUESTION | wxOK | wxCANCEL);
    if (confirm.ShowModal() != wxID_OK)
        return false;
    for (Shortcut other : conflicts)
        wxGetApp().shortcuts().bind(other, KeyChord{});
    return true;
}

void KBShortcutsDialog::apply_bindings()
{
    const ShortcutRegistry& shortcuts = wxGetApp().shortcuts();
    std::set<wxWindow*>     pages;
    for (const EditableRow& row : m_editable_rows) {
        row.key->SetLabel(from_u8(shortcuts.display(row.shortcut)));
        row.reset->Show(shortcuts.is_customized(row.shortcut));
        pages.insert(row.key->GetParent());
    }
    for (wxWindow* page : pages)
        page->Layout();
    wxGetApp().on_shortcuts_changed();
}

void KBShortcutsDialog::open_mouse_preferences()
{
    // Opened from Preferences > Control, the settings are right behind this dialog.
    if (dynamic_cast<PreferencesDialog*>(GetParent()) != nullptr) {
        EndModal(wxID_OK);
        return;
    }
    wxGetApp().open_preferences(size_t(PreferencesDialog::Tab::Control));
    // A language change rebuilds the main frame, taking this dialog with it.
    if (GetParent() != wxGetApp().mainframe) {
        EndModal(wxID_CANCEL);
        return;
    }
    for (const PreferenceRow& row : m_preference_rows)
        row.description->SetLabel(_(mouse_action(row.preference)));
}

ShortcutCaptureDialog::ShortcutCaptureDialog(wxWindow* parent, Shortcut shortcut)
    : DPIDialog(parent, wxID_ANY, _(shortcut_info(shortcut).name), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_shortcut(shortcut)
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    auto prompt = new Label(this, wxGetApp().normal_font(), wxString::Format(_L("Press the new shortcut for \"%s\""), _(shortcut_info(shortcut).name)), LB_AUTO_WRAP);
    prompt->SetMinSize(wxSize(FromDIP(400), -1));
    sizer->Add(prompt, 0, wxALL, FromDIP(20));

    // Keyboard focus stays on this box so the buttons never receive the key presses.
    const wxColour box_colour = StateColor::darkModeColorFor(*wxWHITE);
    StaticBox* capture = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), FromDIP(60)), wxWANTS_CHARS);
    capture->SetCornerRadius(FromDIP(4));
    capture->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
    capture->SetBackgroundColorNormal(box_colour);
    capture->SetBackgroundColour(box_colour);
    wxBoxSizer* capture_sizer = new wxBoxSizer(wxVERTICAL);
    m_chord_label = new wxStaticText(capture, wxID_ANY, from_u8(wxGetApp().shortcuts().display(shortcut)));
    m_chord_label->SetFont(::Label::Head_14);
    m_chord_label->SetBackgroundColour(box_colour);
    capture_sizer->AddStretchSpacer();
    capture_sizer->Add(m_chord_label, 0, wxALIGN_CENTER);
    capture_sizer->AddStretchSpacer();
    capture->SetSizer(capture_sizer);
    capture->Bind(wxEVT_KEY_DOWN, &ShortcutCaptureDialog::on_key, this);
    capture->Bind(wxEVT_CHAR, &ShortcutCaptureDialog::on_char, this);
    capture->Bind(wxEVT_LEFT_DOWN, [capture](wxMouseEvent&) { capture->SetFocus(); });
    sizer->Add(capture, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(20));

    m_status = new Label(this, wxGetApp().normal_font(), _L("Esc cancels, Enter confirms."), LB_AUTO_WRAP);
    m_status->SetMinSize(wxSize(FromDIP(400), -1));
    m_status_colour = m_status->GetForegroundColour();
    sizer->Add(m_status, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    auto dlg_btns = new DialogButtons(this, {"Unbind", "Cancel", "OK"}, "", 1 /*left_aligned*/);
    dlg_btns->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_chord = KeyChord{};
        m_conflicts.clear();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_ok = dlg_btns->GetOK();
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    m_ok->Enable(false);
    sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(10));

    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    capture->CallAfter([capture]() { capture->SetFocus(); });
}

void ShortcutCaptureDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Layout();
    Fit();
}

void ShortcutCaptureDialog::on_key(wxKeyEvent& evt)
{
    if (!evt.HasAnyModifiers()) {
        if (evt.GetKeyCode() == WXK_ESCAPE) {
            EndModal(wxID_CANCEL);
            return;
        }
        if (evt.GetKeyCode() == WXK_RETURN || evt.GetKeyCode() == WXK_NUMPAD_ENTER) {
            if (m_ok->IsEnabled())
                EndModal(wxID_OK);
            return;
        }
    }
    const KeyChord chord = KeyChord::from_event(evt);
    if (!chord.valid())
        return;
    if (chord.needs_char_event()) {
        evt.Skip();
        return;
    }
    record(chord);
}

void ShortcutCaptureDialog::on_char(wxKeyEvent& evt)
{
    const KeyChord chord = KeyChord::from_event(evt);
    if (chord.is_punctuation())
        record(chord);
}

void ShortcutCaptureDialog::record(const KeyChord& chord)
{
    m_chord = chord;
    m_chord_label->SetLabel(from_u8(chord.display()));

    const bool global = (shortcut_info(m_shortcut).contexts & context_bit(ShortcutContext::Global)) != 0;
    if (global && !chord.is_menu_accelerator()) {
        m_status->SetLabel(wxString::Format(_L("This shortcut works while typing in text fields too, so it must include %s or %s (Shift alone is not enough) or use a key that does not type a character, such as a function key."),
                                            from_u8(KeyChord::modifier_name(wxMOD_CONTROL)), from_u8(KeyChord::modifier_name(wxMOD_ALT))));
        m_status->SetForegroundColour(ERROR_COLOUR);
        m_conflicts.clear();
        m_ok->Enable(false);
    } else {
        m_conflicts = wxGetApp().shortcuts().conflicts(m_shortcut, chord);
        m_status->SetForegroundColour(m_status_colour);
        if (m_conflicts.empty())
            m_status->SetLabel(_L("Esc cancels, Enter confirms."));
        else
            m_status->SetLabel(wxString::Format(_L("Already used by: %s. Confirming removes it from those shortcuts."), shortcut_names(m_conflicts)));
        m_ok->Enable(true);
    }
    Layout();
    Fit();
}

} // namespace GUI
} // namespace Slic3r
