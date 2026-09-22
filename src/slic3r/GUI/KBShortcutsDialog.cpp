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

// Page layout in DIPs; titles and rows are indented as in the Preferences dialog.
constexpr int PAGE_WIDTH   = 640;
constexpr int TITLE_MARGIN = DESIGN_LEFT_MARGIN - 10;
constexpr int ROW_MARGIN   = DESIGN_LEFT_MARGIN;
constexpr int ROW_GAP      = 16;

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

std::vector<wxString> to_wx(const std::vector<std::string>& parts)
{
    std::vector<wxString> out;
    for (const std::string& part : parts)
        out.push_back(from_u8(part));
    return out;
}

// The keys a Global shortcut can use, the second line of its hint and of a rejection.
wxString global_key_advice()
{
    return wxString::Format(_L("Use %s or %s, or a key that does not type a character."),
                            from_u8(KeyChord::modifier_name(wxMOD_CONTROL)), from_u8(KeyChord::modifier_name(wxMOD_ALT)));
}

// The pieces of a chord, spaced out for the dialog: "Ctrl + Shift + A".
wxString join_keys(const std::vector<wxString>& parts)
{
    wxString out;
    for (const wxString& part : parts)
        out += (out.empty() ? "" : " + ") + part;
    return out;
}

} // namespace

KBShortcutsDialog::KBShortcutsDialog(wxWindow* parent, ShortcutContext page)
    : DPIDialog(parent, wxID_ANY, _L("Keyboard Shortcuts"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    fill_pages();

    ScalableButton* probe = new ScalableButton(this, wxID_ANY, "edit");
    m_edit_size = probe->GetBestSize();
    probe->Destroy();
    m_buttons_width  = 2 * m_edit_size.x + FromDIP(6);
    m_row_text_width = FromDIP(PAGE_WIDTH) - FromDIP(ROW_MARGIN) - FromDIP(TITLE_MARGIN) - 2 * FromDIP(ROW_GAP) - m_buttons_width;
    GetTextExtent("W", &m_key_slot, nullptr, nullptr, nullptr, &Label::Head_14);

    // The page tabs follow the Preferences dialog.
    m_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS | wxTR_FULL_ROW_HIGHLIGHT);
    m_tabs->Bind(wxEVT_RIGHT_DOWN, [](auto&) {});
    m_tabs->SetFont(Label::Body_14);
    m_simplebook = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(660), FromDIP(500)));
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
    const auto shown = std::find_if(m_pages.begin(), m_pages.end(), [page](const Page& entry) { return entry.context == page; });
    m_tabs->SelectItem(shown == m_pages.end() ? 0 : int(shown - m_pages.begin()));

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
    auto fixed = [](ShortcutSection section, std::vector<wxString> keys, const char* description) { return Row{ FixedKey{ std::move(keys), description }, section }; };
    auto mouse = [](ShortcutSection section, const wxString& button, const char* preference) { return Row{ MouseAction{ button, preference }, section }; };
    auto key   = [](const std::string& key) { return _L_CONTEXT(key, "Keyboard Shortcut"); };
    auto page  = [this](const wxString& title, const wxString& caption, ShortcutContext context, std::vector<Row> fixed_rows) {
        Page entry{ title, caption, context, {} };
        for (Shortcut shortcut : shortcuts_in(context))
            entry.rows.push_back({ shortcut, shortcut_section(shortcut) });
        entry.rows.insert(entry.rows.end(), fixed_rows.begin(), fixed_rows.end());
        std::stable_sort(entry.rows.begin(), entry.rows.end(), [](const Row& a, const Row& b) { return a.section < b.section; });
        m_pages.push_back(std::move(entry));
    };

    const wxString ctrl        = from_u8(KeyChord::modifier_name(wxMOD_CONTROL));
    const wxString alt         = from_u8(KeyChord::modifier_name(wxMOD_ALT));
    const wxString shift       = from_u8(KeyChord::modifier_name(wxMOD_SHIFT));
    const wxString shift_ctrl  = shift + "/" + ctrl;   // either one
    const wxString any_key     = key(L_CONTEXT("Key", "Keyboard Shortcut"));   // the key the row's shortcut is bound to
    const wxString esc         = key(L_CONTEXT("Esc", "Keyboard Shortcut"));
    const wxString left_button = _L("Left mouse");
    const wxString wheel       = _L("Mouse wheel");
    using Section              = ShortcutSection;

    if (wxGetApp().is_editor()) {
        page(_L("Global"), _L("Available anywhere in the window, even while typing in a text field."), ShortcutContext::Global, {
            fixed(Section::Application, { alt, "1-9, 0" }, L("Run a speed dial favorite while the dial is open")),
            fixed(Section::Application, { ctrl, key(L_CONTEXT("Tab", "Keyboard Shortcut")) }, L("Switch to the next main tab")),
        });

        page(_L("Prepare"), _L("Available while the 3D view on the Prepare tab has focus."), ShortcutContext::Plater, {
            fixed(Section::Selection, { alt, left_button }, L("Select a part")),
            fixed(Section::Selection, { ctrl, left_button }, L("Select multiple objects")),
            fixed(Section::Selection, { shift, left_button }, L("Select objects by rectangle")),
            fixed(Section::Selection, { esc }, L("Deselect All")),
            fixed(Section::Objects, { "1-9" }, L("Keyboard 1-9: set filament for object/part")),
            fixed(Section::Placement, { shift, any_key }, L("Movement step set to 1mm")),
            fixed(Section::Placement, { ctrl, any_key }, L("Movement in camera space")),
            mouse(Section::Camera, left_button, "left_mouse_drag_action"),
            mouse(Section::Camera, _L("Middle mouse"), "middle_mouse_drag_action"),
            mouse(Section::Camera, _L("Right mouse"), "right_mouse_drag_action"),
            fixed(Section::Camera, { wheel }, L("Zoom View")),
        });

        page(_L("Painting"), _L("Available while a painting gizmo is open: supports, seam, fuzzy skin or color painting."), ShortcutContext::Painting, {
            fixed(Section::Gizmos, { esc }, L("Deselect All")),
            fixed(Section::Gizmos, { shift, left_button }, L("Move: press to snap by 1mm")),
            fixed(Section::PaintingTools, { ctrl, wheel }, L("Support/Color Painting: adjust pen radius")),
            fixed(Section::PaintingTools, { alt, wheel }, L("Support/Color Painting: adjust section position")),
        });

        page(_L("Objects list"), _L("Available while the object list has focus."), ShortcutContext::ObjectList, {
            fixed(Section::Selection, { esc }, L("Deselect All")),
            fixed(Section::Objects, { "1-9" }, L("Set extruder number for the objects and parts")),
            fixed(Section::Objects, { key(L_CONTEXT("Space", "Keyboard Shortcut")) }, L("Select the object/part and press space to change the name")),
            fixed(Section::Objects, { _L("Mouse click") }, L("Select the object/part and mouse click to change the name")),
        });
    }

    page(_L("Preview"), _L("Available while the 3D view on the Preview tab has focus."), ShortcutContext::Preview, {
        fixed(Section::Sliders, { shift_ctrl, any_key }, L("Move slider 5x faster")),
        fixed(Section::Sliders, { shift_ctrl, wheel }, L("Scroll slider 5x faster")),
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
    const int page_width = FromDIP(PAGE_WIDTH);
    scrollable_panel->SetInitialSize(wxSize(page_width, FromDIP(450)));

    const int title_margin = FromDIP(TITLE_MARGIN);
    const int row_margin   = FromDIP(ROW_MARGIN);
    const int gap          = FromDIP(ROW_GAP);

    wxBoxSizer* scrollable_panel_sizer = new wxBoxSizer(wxVERTICAL);

    const wxColour note_colour = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    const wxColour note_text   = StateColor::darkModeColorFor(wxColour("#6B6B6A"));
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

    auto key_parts = [](const Row& row) {
        return std::visit(overloaded{
            [](Shortcut shortcut) { return to_wx(wxGetApp().shortcuts().binding(shortcut).display_parts()); },
            [](const FixedKey& fixed) { return fixed.keys; },
            [](const MouseAction& mouse) { return std::vector<wxString>{ mouse.button }; },
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
        auto chord_label = [&](long style) {
            auto label = new wxStaticText(scrollable_panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, style);
            label->SetFont(Label::Head_14);
            label->SetForegroundColour(DESIGN_GRAY900_COLOR);
            return label;
        };
        wxStaticText* modifiers = chord_label(0);
        wxStaticText* key       = chord_label(wxALIGN_CENTRE_HORIZONTAL);   // a single key is centred in its column
        desc->Wrap(m_row_text_width - set_chord_labels(modifiers, key, key_parts(row)));

        wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
        if (const MouseAction* mouse = std::get_if<MouseAction>(&row.content)) {
            auto settings = icon_button("settings", _L("Preferences"));
            settings->Bind(wxEVT_BUTTON, [this, preference = mouse->preference](wxCommandEvent&) { open_mouse_preferences(preference); });
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
            m_editable_rows.push_back({ shortcut, desc, modifiers, key, reset });
        } else {
            auto lock = new wxStaticBitmap(scrollable_panel, wxID_ANY, ScalableBitmap(scrollable_panel, "printer_status_lock", 16).bmp());
            lock->SetToolTip(_L("Not customizable"));
            buttons->Add((m_edit_size.x - lock->GetBestSize().x) / 2, m_edit_size.y);   // centred under the edit icons, at their height
            buttons->Add(lock, 0, wxALIGN_CENTRE_VERTICAL);
        }
        if (const int used = buttons->GetMinSize().x; used < m_buttons_width)   // a box sizer recomputes its own min size, so pad it
            buttons->AddSpacer(m_buttons_width - used);

        wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);
        row_sizer->AddSpacer(row_margin);
        row_sizer->Add(desc, 1, wxALIGN_CENTRE_VERTICAL);
        row_sizer->AddSpacer(gap);
        row_sizer->Add(modifiers, 0, wxALIGN_CENTRE_VERTICAL);
        row_sizer->Add(key, 0, wxALIGN_CENTRE_VERTICAL);
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
    const wxString question = wxString::Format(_L("%s is assigned to %s. Reassign it to %s?"),
                                               join_keys(to_wx(dlg.chord().display_parts())), shortcut_names(dlg.conflicts()), _(shortcut_info(shortcut).name));
    if (!take_chord_from(shortcut, dlg.conflicts(), question))
        return;
    wxGetApp().shortcuts().bind(shortcut, dlg.chord());
    apply_bindings();
}

void KBShortcutsDialog::reset_shortcut(Shortcut shortcut)
{
    const std::vector<Shortcut> conflicts = wxGetApp().shortcuts().conflicts(shortcut, shortcut_info(shortcut).default_chord);
    const wxString question = wxString::Format(_L("The default %s is assigned to %s. Reassign it to %s?"),
                                               join_keys(to_wx(shortcut_info(shortcut).default_chord.display_parts())), shortcut_names(conflicts), _(shortcut_info(shortcut).name));
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
        const int chord_width = set_chord_labels(row.modifiers, row.key, to_wx(shortcuts.binding(row.shortcut).display_parts()));
        row.description->SetLabel(_(shortcut_info(row.shortcut).name));
        row.description->Wrap(m_row_text_width - chord_width);
        row.reset->Show(shortcuts.is_customized(row.shortcut));
        pages.insert(row.key->GetParent());
    }
    for (wxWindow* page : pages)
        page->Layout();
    wxGetApp().on_shortcuts_changed();
}

int KBShortcutsDialog::set_chord_labels(wxStaticText* modifiers, wxStaticText* key, std::vector<wxString> parts)
{
    const wxString last = parts.empty() ? wxString() : parts.back();
    if (!parts.empty())
        parts.pop_back();
    modifiers->SetLabel(parts.empty() ? wxString() : join_keys(parts) + " + ");
    modifiers->Show(!parts.empty());
    key->SetLabel(last);
    const int key_width = last.length() == 1 ? m_key_slot : key->GetBestSize().x;
    key->SetMinSize(wxSize(key_width, -1));
    return (parts.empty() ? 0 : modifiers->GetBestSize().x) + key_width;
}

void KBShortcutsDialog::open_mouse_preferences(const char* preference)
{
    // Opened from Preferences > Control, the settings are right behind this dialog.
    if (auto preferences = dynamic_cast<PreferencesDialog*>(GetParent()); preferences != nullptr) {
        // Runs once this dialog has closed and the focus is back in Preferences.
        preferences->CallAfter([preferences, preference] { preferences->select_tab(PreferencesTab::Control, preference); });
        EndModal(wxID_OK);
        return;
    }
    wxGetApp().open_preferences(PreferencesTab::Control, preference);
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

    // A Global shortcut also runs while a text field has the focus, so its hint names the keys it can use.
    const bool     global = (shortcut_info(shortcut).contexts & context_bit(ShortcutContext::Global)) != 0;
    const wxString advice = global_key_advice();
    const wxString typing = _L("Global shortcuts also apply while typing.");
    const wxString rule   = _L("A key that types a character cannot be a global shortcut.");
    m_hint      = global ? typing + "\n" + advice : _L("Esc cancels, Enter confirms.");
    m_rejection = rule + "\n" + advice;

    // Wide enough for each sentence on a line of its own where the translation allows, within limits.
    int width = FromDIP(450);
    for (const wxString& sentence : { typing, rule, advice }) {
        int extent = 0;
        GetTextExtent(sentence, &extent, nullptr, nullptr, nullptr, &wxGetApp().normal_font());
        width = std::max(width, extent);
    }
    width = std::min(width, FromDIP(550));

    auto prompt = new Label(this, wxGetApp().normal_font(), wxString::Format(_L("Press the new shortcut for\n\"%s\""), _(shortcut_info(shortcut).name)), LB_AUTO_WRAP);
    prompt->SetMinSize(wxSize(width, -1));
    sizer->Add(prompt, 0, wxALL, FromDIP(20));

    // Keyboard focus stays on this box so the buttons never receive the key presses.
    const wxColour box_colour = StateColor::darkModeColorFor(*wxWHITE);
    StaticBox* capture = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(60)), wxWANTS_CHARS);
    capture->SetCornerRadius(FromDIP(4));
    capture->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#009688")));   // the focused-input colour, since the box always has the focus
    capture->SetBackgroundColorNormal(box_colour);
    capture->SetBackgroundColour(box_colour);
    wxBoxSizer* capture_sizer = new wxBoxSizer(wxVERTICAL);
    m_chord_label = new wxStaticText(capture, wxID_ANY, join_keys(to_wx(wxGetApp().shortcuts().binding(shortcut).display_parts())));
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

    m_status = new Label(this, wxGetApp().normal_font(), m_hint, LB_AUTO_WRAP);
    m_status->SetMinSize(wxSize(width, 3 * m_status->GetCharHeight()));   // room for three lines, so the dialog keeps its size while keys are tried
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
    m_chord_label->SetLabel(join_keys(to_wx(chord.display_parts())));
    m_chord_label->GetParent()->Layout();

    auto reject = [this](const wxString& reason) {
        m_status->SetForegroundColour(ERROR_COLOUR);
        m_status->SetLabel(reason);
        m_conflicts.clear();
        m_ok->Enable(false);
    };
    const bool global = (shortcut_info(m_shortcut).contexts & context_bit(ShortcutContext::Global)) != 0;
    if (global && !chord.is_menu_accelerator()) {
        reject(m_rejection);
    } else if (const std::optional<Shortcut> owner = wxGetApp().shortcuts().step_owner(m_shortcut, chord); owner.has_value()) {
        reject(wxString::Format(_L("Already used as a step of %s."), _(shortcut_info(*owner).name)));
    } else {
        m_conflicts = wxGetApp().shortcuts().conflicts(m_shortcut, chord);
        m_status->SetForegroundColour(m_status_colour);
        if (m_conflicts.empty())
            m_status->SetLabel(m_hint);
        else
            m_status->SetLabel(wxString::Format(_L("Already assigned to %s. Press OK to reassign it."), shortcut_names(m_conflicts)));
        m_ok->Enable(true);
    }
    m_status->Refresh();   // a colour change alone does not repaint
    Layout();
    Fit();
}

} // namespace GUI
} // namespace Slic3r
