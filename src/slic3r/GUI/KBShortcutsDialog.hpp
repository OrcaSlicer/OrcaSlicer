#ifndef slic3r_GUI_KBShortcutsDialog_hpp_
#define slic3r_GUI_KBShortcutsDialog_hpp_

#include <wx/wx.h>
#include <map>
#include <variant>
#include <vector>

#include "GUI_Utils.hpp"
#include "Shortcuts.hpp"
#include "wxExtensions.hpp"
#include <wx/simplebook.h>

class Button;
class Label;
class TabCtrl;

namespace Slic3r {
namespace GUI {

// Lists every shortcut per context and lets the user rebind the assignable ones.
class KBShortcutsDialog : public DPIDialog
{
    // A key the user cannot rebind.
    struct FixedKey
    {
        std::vector<wxString> keys;          // modifier names and the key, shown joined with "+"
        const char*           description;   // untranslated
    };
    // A mouse button whose camera action is chosen in Preferences.
    struct MouseAction
    {
        wxString    button;
        const char* preference;    // AppConfig key of the action
    };
    struct Row
    {
        std::variant<Shortcut, FixedKey, MouseAction> content;
        ShortcutSection                               section;
    };
    struct Page
    {
        wxString         title;
        wxString         caption;   // when the page's keys apply
        ShortcutContext  context;
        std::vector<Row> rows;
    };
    struct EditableRow
    {
        Shortcut        shortcut;
        wxStaticText*   description;
        wxStaticText*   modifiers;
        wxStaticText*   key;
        ScalableButton* reset;
    };
    struct PreferenceRow
    {
        const char*   preference;
        wxStaticText* description;
    };

    std::vector<Page>          m_pages;
    std::vector<EditableRow>   m_editable_rows;
    std::vector<PreferenceRow> m_preference_rows;
    // Row geometry, measured once and shared by every page.
    wxSize                     m_edit_size;            // an edit or reset icon
    int                        m_buttons_width  = 0;   // every row's buttons column, so the right-aligned keys share an edge
    int                        m_row_text_width = 0;   // what a description and its chord share; the description wraps at the rest
    int                        m_key_slot       = 0;   // width of the widest single key, the column single keys line up in

    TabCtrl*      m_tabs;
    wxSimplebook* m_simplebook;

public:
    KBShortcutsDialog(wxWindow* parent, ShortcutContext page);   // opens on the page of that context

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void fill_pages();
    wxPanel* create_page(wxWindow* parent, const Page& page);
    void edit_shortcut(Shortcut shortcut);
    void reset_shortcut(Shortcut shortcut);
    // Asks question before unbinding conflicts; false when the user declined.
    bool take_chord_from(Shortcut shortcut, const std::vector<Shortcut>& conflicts, const wxString& question);
    void apply_bindings();   // refreshes the rows and pushes the change to the rest of the app
    // Puts a chord on a row's two labels, a single key in the shared column, and returns the width the chord takes.
    int  set_chord_labels(wxStaticText* modifiers, wxStaticText* key, std::vector<wxString> parts);
    void open_mouse_preferences(const char* preference);
};

// Records one key chord for a shortcut, warning about the shortcuts it would take the chord from.
class ShortcutCaptureDialog : public DPIDialog
{
public:
    ShortcutCaptureDialog(wxWindow* parent, Shortcut shortcut);

    // Valid after ShowModal() returned wxID_OK; an invalid chord means "unbind".
    const KeyChord&              chord() const { return m_chord; }
    const std::vector<Shortcut>& conflicts() const { return m_conflicts; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void on_key(wxKeyEvent& evt);
    void on_char(wxKeyEvent& evt);
    void record(const KeyChord& chord);

    Shortcut              m_shortcut;
    KeyChord              m_chord;
    std::vector<Shortcut> m_conflicts;
    wxStaticText*         m_chord_label;
    wxString              m_hint;        // what m_status shows while there is nothing to warn about
    wxString              m_rejection;   // what it shows for a key a Global shortcut cannot use
    Label*                m_status;
    wxColour              m_status_colour;
    Button*               m_ok;
};

} // namespace GUI
} // namespace Slic3r

#endif
