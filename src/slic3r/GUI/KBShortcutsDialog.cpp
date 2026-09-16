#include "libslic3r/libslic3r.h"
#include "KBShortcutsDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "Notebook.hpp"
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
        fixed(Section::Sliders, shift + any_key + " / " + ctrl + any_key, L("Move slider 5x faster")),
        fixed(Section::Sliders, shift + wheel + " / " + ctrl + wheel, L("Scroll slider 5x faster")),
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
    scrollable_panel->SetInitialSize(wxSize(page_width, FromDIP(450)));

    // Titles and rows are indented as in the Preferences dialog.
    const int title_margin = FromDIP(DESIGN_LEFT_MARGIN - 10);
    const int row_margin   = FromDIP(DESIGN_LEFT_MARGIN);
    const int gap          = FromDIP(16);

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
    note_text_ctrl->Wrap(page_width - 2 * title_margin - FromDIP(10 + 16 + 8 + 10));
    wxBoxSizer* note_sizer = new wxBoxSizer(wxHORIZONTAL);
    note_sizer->Add(note_icon, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, FromDIP(10));
    note_sizer->Add(note_text_ctrl, 1, wxALIGN_CENTRE_VERTICAL | wxALL, FromDIP(8));
    note->SetSizer(note_sizer);
    scrollable_panel_sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, title_margin);

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
    auto icon_button = [&](const char* icon, const wxString& tooltip) {
        auto button = new ScalableButton(scrollable_panel, wxID_ANY, icon);
        button->SetBackgroundColour(page_colour);
        button->SetToolTip(tooltip);
        return button;
    };

    // Every row ends in a buttons column of one width, so the right-aligned keys share an
    // edge without sharing a column; each description wraps at whatever its own key leaves.
    ScalableButton* probe = icon_button("edit", "");
    const int buttons_width = 2 * probe->GetBestSize().x + FromDIP(6);
    probe->Destroy();
    m_row_text_width = page_width - row_margin - title_margin - 2 * gap - buttons_width;

    std::optional<ShortcutSection> section;
    for (const Row& row : page.rows) {
        if (section != row.section) {
            auto heading = new StaticLine(scrollable_panel, false, _(section_name(row.section)));
            heading->SetFont(Label::Head_14);
            heading->SetForegroundColour(DESIGN_GRAY900_COLOR);
            wxBoxSizer* heading_sizer = new wxBoxSizer(wxHORIZONTAL);
            heading_sizer->AddSpacer(title_margin);
            heading_sizer->Add(heading, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
            heading_sizer->AddSpacer(title_margin);
            scrollable_panel_sizer->Add(heading_sizer, 0, wxEXPAND | wxTOP, FromDIP(section.has_value() ? 10 : 6));
            section = row.section;
        }
        auto desc = new wxStaticText(scrollable_panel, wxID_ANY, description(row));
        desc->SetFont(Label::Body_14);
        desc->SetForegroundColour(DESIGN_GRAY900_COLOR);
        auto key = new wxStaticText(scrollable_panel, wxID_ANY, key_text(row));
        key->SetFont(Label::Head_14);
        key->SetForegroundColour(DESIGN_GRAY900_COLOR);
        desc->Wrap(m_row_text_width - key->GetBestSize().x);

        wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->SetMinSize(buttons_width, -1);
        if (const MouseAction* mouse = std::get_if<MouseAction>(&row.content)) {
            auto settings = icon_button("settings", _L("Preferences"));
            settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_mouse_preferences(); });
            buttons->Add(settings, 0, wxALIGN_CENTRE_VERTICAL);
            m_preference_rows.push_back({ mouse->preference, desc });
        } else if (const Shortcut* editable = std::get_if<Shortcut>(&row.content)) {
            const Shortcut shortcut = *editable;
            auto change = icon_button("edit", _L("Edit"));
            change->Bind(wxEVT_BUTTON, [this, shortcut](wxCommandEvent&) { edit_shortcut(shortcut); });
            auto reset = icon_button("undo", _L("Reset"));
            reset->Bind(wxEVT_BUTTON, [this, shortcut](wxCommandEvent&) { reset_shortcut(shortcut); });
            reset->Show(wxGetApp().shortcuts().is_customized(shortcut));
            buttons->Add(change, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, FromDIP(6));
            buttons->Add(reset, 0, wxALIGN_CENTRE_VERTICAL | wxRESERVE_SPACE_EVEN_IF_HIDDEN);
            m_editable_rows.push_back({ shortcut, desc, key, reset });
        }

        wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);
        row_sizer->AddSpacer(row_margin);
        row_sizer->Add(desc, 1, wxALIGN_CENTRE_VERTICAL);
        row_sizer->Add(key, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, gap);
        row_sizer->Add(buttons, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, gap);
        row_sizer->AddSpacer(title_margin);
        scrollable_panel_sizer->Add(row_sizer, 0, wxEXPAND | wxTOP, FromDIP(4));
    }
    scrollable_panel_sizer->AddSpacer(title_margin);
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
        row.description->SetLabel(_(shortcut_info(row.shortcut).name));
        row.description->Wrap(m_row_text_width - row.key->GetBestSize().x);
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

    auto dlg_btns = new DialogButtons(this, {"Unbind", "OK", "Cancel"}, "", 1 /*left_aligned*/);
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
