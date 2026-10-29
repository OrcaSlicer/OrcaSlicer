#ifndef slic3r_DesignCardModel_hpp_
#define slic3r_DesignCardModel_hpp_

// A tool card of the Design panel as DATA: a title, an icon and an ordered list of rows, each a label and a field. The ImGui
// sidebar draws a card straight from this table (DesignSidebarMirror.cpp); there is no wx control behind it.
//
// The fields are wxEvtHandlers with the small slice of the wxSpinCtrlDouble / ComboBox / CheckBox API the panel's code uses
// (GetValue, SetValue, Append, SetSelection, Enable, ...), so the handlers written against those controls bind and read
// them unchanged. A change made by the USER (user_set / user_select) is queued as the event the wx control would have sent
// and then handed on to the next handler, the panel's form, which is where its refresh-the-preview handlers are bound.
// A change made by the code (SetValue ...) sends nothing, as with the wx controls.
//
// To add a control to a card: one line in the card's builder (card->spin(label, ...), card->combo(label), ...). Nothing else
// has to know about it.

#include <wx/bitmap.h>
#include <wx/checklst.h>
#include <wx/colour.h>
#include "slic3r/GUI/CAD/DesignRowList.hpp"

#include <wx/event.h>
#include <wx/string.h>
#include <wx/combobox.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

class CardField : public wxEvtHandler
{
public:
    enum class Kind { Num, Choice, Toggle, Text, Button, CheckList, Edit, List, Rows, Table, Slot };
    virtual Kind kind() const = 0;

    bool IsEnabled() const { return m_enabled; }
    void Enable(bool on = true) { m_enabled = on; }
    bool IsShown() const { return m_shown; }
    void Show(bool on = true) { m_shown = on; }
    const wxString& tip() const { return m_tip; }
    void SetToolTip(const wxString& t) { m_tip = t; }

protected:
    // What the wx control would have sent, queued: the handlers run on the next turn of the loop, outside the ImGui frame.
    void post(wxEvent& e) { e.SetEventObject(this); wxQueueEvent(this, e.Clone()); }

private:
    bool     m_enabled{true};
    bool     m_shown{true};
    wxString m_tip;
};

class NumField : public CardField
{
public:
    NumField(double v, double lo, double hi) : m_v(v), m_lo(lo), m_hi(hi) {}
    Kind kind() const override { return Kind::Num; }

    double GetValue() const { return m_v; }
    void   SetValue(double v) { m_v = std::clamp(v, m_lo, m_hi); }
    double GetMin() const { return m_lo; }
    double GetMax() const { return m_hi; }
    double GetIncrement() const { return m_inc; }
    int    GetDigits() const { return m_digits; }
    void   SetRange(double lo, double hi) { m_lo = lo; m_hi = hi; m_v = std::clamp(m_v, lo, hi); }
    void   SetDigits(int d) { m_digits = d; }
    void   SetIncrement(double i) { m_inc = i; }

    // the user typed or stepped to `v`
    void user_set(double v)
    {
        v = std::clamp(v, m_lo, m_hi);
        if (v == m_v) return;
        m_v = v;
        wxSpinDoubleEvent e(wxEVT_SPINCTRLDOUBLE, 0);
        e.SetValue(v);
        post(e);
    }

private:
    double m_v, m_lo, m_hi, m_inc{1.0};
    int    m_digits{2};
};

class ChoiceField : public CardField
{
public:
    Kind kind() const override { return Kind::Choice; }

    int  Append(const wxString& s) { m_items.push_back(s); m_data.push_back(nullptr); return int(m_items.size()) - 1; }
    int  Append(const wxString& s, const wxBitmap&) { return Append(s); }
    void Clear() { m_items.clear(); m_data.clear(); m_sel = -1; }
    void  SetClientData(unsigned i, void* d) { if (i < m_data.size()) m_data[i] = d; }
    void* GetClientData(unsigned i) const { return i < m_data.size() ? m_data[i] : nullptr; }
    unsigned GetCount() const { return unsigned(m_items.size()); }
    wxString GetString(unsigned i) const { return i < m_items.size() ? m_items[i] : wxString(); }
    int  GetSelection() const { return m_sel; }
    void SetSelection(int i) { m_sel = i; if (i >= 0 && i < int(m_items.size())) m_text = m_items[size_t(i)]; }
    // An editable choice takes typed text as well (a field name the list does not hold).
    void SetEditable(bool on) { m_editable = on; }
    bool IsEditable() const { return m_editable; }
    wxString GetValue() const { return m_editable ? m_text : GetString(unsigned(std::max(m_sel, 0))); }
    void     SetValue(const wxString& s) { m_text = s; }

    void user_select(int i)
    {
        if (i == m_sel || i < 0 || i >= int(m_items.size())) return;
        m_sel = i;
        m_text = m_items[size_t(i)];
        wxCommandEvent e(wxEVT_COMBOBOX, 0);
        e.SetInt(i);
        e.SetString(m_items[size_t(i)]);
        post(e);
    }

private:
    std::vector<wxString> m_items;
    std::vector<void*>    m_data;
    int                   m_sel{-1};
    wxString              m_text;
    bool                  m_editable{false};
};

class ToggleField : public CardField
{
public:
    Kind kind() const override { return Kind::Toggle; }
    bool GetValue() const { return m_v; }
    void SetValue(bool v) { m_v = v; }

    void user_toggle()
    {
        m_v = !m_v;
        wxCommandEvent e(wxEVT_TOGGLEBUTTON, 0);
        e.SetInt(m_v ? 1 : 0);
        post(e);
    }

private:
    bool m_v{false};
};

// A line of text the code changes (a card title, a note, the picked edge's name).
class CardText : public CardField
{
public:
    explicit CardText(const wxString& s = wxString()) : m_text(s) {}
    Kind kind() const override { return Kind::Text; }
    void     SetLabel(const wxString& s) { m_text = s; }
    wxString GetLabel() const { return m_text; }
    void     Refresh() {}   // drawn every frame
    void     Update() {}
    void     Hide() { Show(false); }
    bool     UseForegroundColour() const { return m_fg.IsOk(); }
    wxColour GetForegroundColour() const { return m_fg; }
    // a status colour (green, red); the default is the panel's text colour
    void     SetForegroundColour(const wxColour& c) { m_fg = c; }
    const wxColour& colour() const { return m_fg; }
    // A reference the tool needs (a sketch, a face): drawn as a box, lit while it still holds a placeholder such as "(pick a face)" or
    // "Sketch: —". `required` = an empty one is drawn red as what is still missing; an optional one stays neutral.
    CardText* SetReference(bool required = true) { m_ref = true; m_ref_required = required; return this; }
    bool      is_reference() const { return m_ref; }
    bool      ref_required() const { return m_ref_required; }
    bool      placeholder() const { return m_text.IsEmpty() || m_text.StartsWith("(") || m_text.EndsWith(wxString::FromUTF8("—")); }

private:
    wxString m_text;
    wxColour m_fg;
    bool     m_ref{false}, m_ref_required{true};
};

// A line the user types in; Enter sends wxEVT_TEXT_ENTER, as the wx text control did.
class TextField : public CardField
{
public:
    Kind kind() const override { return Kind::Edit; }
    wxString GetValue() const { return m_v; }
    void     SetValue(const wxString& s) { m_v = s; }
    void     ChangeValue(const wxString& s) { m_v = s; }
    void     Clear() { m_v.clear(); }
    void     SetFocus() { m_want_focus = true; }
    void     SetSelection(long, long) {}   // a field takes the focus with its text selected
    bool     take_focus() { const bool f = m_want_focus; m_want_focus = false; return f; }

    void user_enter()
    {
        wxCommandEvent e(wxEVT_TEXT_ENTER, 0);
        e.SetString(m_v);
        post(e);
    }

private:
    wxString m_v;
    bool     m_want_focus{false};
};

class ButtonField : public CardField
{
public:
    explicit ButtonField(const wxString& s) : m_text(s) {}
    Kind kind() const override { return Kind::Button; }
    void     SetLabel(const wxString& s) { m_text = s; }
    wxString GetLabel() const { return m_text; }
    template<class A, class B> void SetStyle(A&&, B&&) {}
    wxEvtHandler* GetEventHandler() { return this; }   // the wx button's look; the ImGui one has one

    void user_click()
    {
        wxCommandEvent e(wxEVT_BUTTON, 0);
        post(e);
    }

private:
    wxString m_text;
};

// A reference a tool needs from the model (a profile, an axis, a face ...): a box that shows what is chosen, or says what to
// click. While `active` the viewport is waiting for that pick and the box is lit; clicking the box arms it (wxEVT_BUTTON).
class SlotField : public CardField
{
public:
    Kind kind() const override { return Kind::Slot; }
    // the chosen thing's name; empty = nothing chosen yet
    void     SetLabel(const wxString& s) { m_text = s; }
    wxString GetLabel() const { return m_text; }
    bool     filled() const { return !m_text.IsEmpty(); }
    // what to click, shown while the slot is empty
    void     SetHint(const wxString& s) { m_hint = s; }
    const wxString& hint() const { return m_hint; }
    // armed: the next click in the viewport lands here
    void     SetActive(bool on) { m_active = on; }
    bool     active() const { return m_active; }
    // a slot the tool cannot do without (an empty one is drawn as the thing still missing)
    void     SetRequired(bool on) { m_required = on; }
    bool     required() const { return m_required; }

    void user_click()
    {
        wxCommandEvent e(wxEVT_BUTTON, 0);
        post(e);
    }

private:
    wxString m_text, m_hint;
    bool     m_active{false}, m_required{true};
};

// A list of items each with a check box (the features to repeat, the profiles of a loft).
class CheckListField : public CardField
{
public:
    Kind kind() const override { return Kind::CheckList; }
    int      Append(const wxString& s) { m_items.push_back(s); m_on.push_back(false); return int(m_items.size()) - 1; }
    void     Clear() { m_items.clear(); m_on.clear(); }
    unsigned GetCount() const { return unsigned(m_items.size()); }
    wxString GetString(unsigned i) const { return i < m_items.size() ? m_items[i] : wxString(); }
    bool     IsChecked(unsigned i) const { return i < m_on.size() && m_on[i]; }
    void     Check(unsigned i, bool on = true) { if (i < m_on.size()) m_on[i] = on; }

    void user_check(unsigned i)
    {
        if (i >= m_on.size()) return;
        m_on[i] = !m_on[i];
        wxCommandEvent e(wxEVT_CHECKLISTBOX, 0);
        e.SetInt(int(i));
        post(e);
    }

private:
    std::vector<wxString> m_items;
    std::vector<bool>     m_on;
};

// A list the code refills (the constraints of a sketch): a label per row, a delete cross and a click on each.
class ListField : public CardField
{
public:
    Kind kind() const override { return Kind::List; }
    void     Clear() { m_rows.clear(); m_sel = -1; }
    void     Add(const wxString& s) { m_rows.push_back(s); }
    unsigned GetCount() const { return unsigned(m_rows.size()); }
    wxString GetString(unsigned i) const { return i < m_rows.size() ? m_rows[i] : wxString(); }
    int      GetSelection() const { return m_sel; }
    void     SetSelection(int i) { m_sel = i; }
    void     SetEmptyText(const wxString& s) { m_empty = s; }
    const wxString& empty_text() const { return m_empty; }

    std::function<void(int)> on_click;    // a row's label
    std::function<void(int)> on_delete;   // a row's cross
    // from the ImGui frame: the handler runs after it, since both rebuild the rows
    void user_click(int i)  { CallAfter([this, i] { if (on_click) on_click(i); }); }
    void user_delete(int i) { CallAfter([this, i] { if (on_delete) on_delete(i); }); }

private:
    std::vector<wxString> m_rows;
    wxString              m_empty;
    int                   m_sel{-1};
};

// The rows of the feature tree or the bodies: the list is a DesignRowList the panel talks to.
class RowsField : public CardField
{
public:
    RowsField(int max_visible, bool selectable) : m_list(new DesignRowList(max_visible, selectable)) {}
    Kind kind() const override { return Kind::Rows; }
    DesignRowList* list() { return m_list.get(); }
private:
    std::unique_ptr<DesignRowList> m_list;
};

// A table of text (the document variables), with the slice of the wxListCtrl API the panel's code uses.
class TableField : public CardField
{
public:
    Kind kind() const override { return Kind::Table; }
    void AppendColumn(const wxString& name, int = 0, int = 0) { m_cols.push_back(name); }
    void DeleteAllItems() { m_rows.clear(); m_sel = -1; }
    long InsertItem(long i, const wxString& s)
    {
        i = std::clamp(i, 0L, long(m_rows.size()));
        m_rows.insert(m_rows.begin() + i, std::vector<wxString>(std::max<size_t>(m_cols.size(), 1)));
        m_rows[size_t(i)][0] = s;
        return i;
    }
    void SetItem(long i, int col, const wxString& s) { if (i >= 0 && i < long(m_rows.size()) && col < int(m_rows[size_t(i)].size())) m_rows[size_t(i)][size_t(col)] = s; }
    wxString GetItemText(long i, int col = 0) const { return (i >= 0 && i < long(m_rows.size()) && col < int(m_rows[size_t(i)].size())) ? m_rows[size_t(i)][size_t(col)] : wxString(); }
    int  GetItemCount() const { return int(m_rows.size()); }
    // the selected row, or -1 (the wx call took more arguments: next item after `from` in some state)
    long GetNextItem(long, int = 0, int = 0) const { return m_sel; }
    const std::vector<wxString>& columns() const { return m_cols; }
    int  GetSelection() const { return m_sel; }

    std::function<void()> on_activate;   // a double click on a row
    std::set<std::string> highlight;     // names (column 0) of rows to light up, set by the owner every frame
    void user_select(int i, bool activate)
    {
        CallAfter([this, i, activate] {
            m_sel = i;
            if (activate && on_activate) on_activate();
        });
    }

private:
    std::vector<wxString>              m_cols;
    std::vector<std::vector<wxString>> m_rows;
    int                                m_sel{-1};
};

// Rows that show and hide together (a mode of a card that needs only some of its inputs).
struct CardGroup {
    bool shown{true};
    void Show(bool on = true) { shown = on; }
    bool IsShown() const { return shown; }
};

struct CardRow {
    // Field: a label and its control. Note: a text across the card. Wide: a control across the card (a list).
    // Pick: a button in the label column and a text in the control column. Buttons: two buttons side by side.
    enum class Kind { Field, Note, Wide, Pick, Buttons };
    Kind        kind{Kind::Field};
    wxString    label;
    CardField*  field{nullptr};        // owned by the card
    CardField*  field2{nullptr};       // Pick: the text beside the button; Field: a label the code changes (else `label`)
    CardGroup*  group{nullptr};        // hidden when its group is
};

class CardModel
{
public:
    CardModel(std::string icon, const wxString& title) : m_icon(std::move(icon)), m_title(new CardText(title)) {}

    // The panel's form takes the events the fields queue (its handlers refresh the preview).
    void set_sink(wxEvtHandler* sink) { m_sink = sink; for (auto& f : m_fields) f->SetNextHandler(sink); }

    // The card's header carries a fold arrow; the panel keeps the state under this key.
    void         foldable(const std::string& key) { m_fold_key = key; }
    const std::string& fold_key() const { return m_fold_key; }
    // A card the panel shows and hides (a tool card is open or it is not).
    void Show(bool on = true) { m_shown = on; }
    bool IsShown() const { return m_shown; }
    // An icon button at the right end of the header.
    struct HeaderButton { std::string icon; wxString tip; std::function<void()> fn; };
    void header_button(const std::string& icon, const wxString& tip, std::function<void()> fn) { m_header_buttons.push_back({icon, tip, std::move(fn)}); }
    const std::vector<HeaderButton>& header_buttons() const { return m_header_buttons; }
    CardText*    title() { return m_title.get(); }
    const std::string& icon() const { return m_icon; }
    const std::vector<CardRow>& rows() const { return m_rows; }

    // A control of the card's row builders is a row: label on the left, field on the right.
    NumField*    spin(const wxString& label, double v, double lo = 0.1, double hi = 1000.0)
    { return add_field(label, new NumField(v, lo, hi)); }
    ChoiceField* combo(const wxString& label) { return add_field(label, new ChoiceField()); }
    ToggleField* check(const wxString& label) { return add_field(label, new ToggleField()); }
    CardText*    text(const wxString& label, const wxString& s = wxString()) { return add_field(label, new CardText(s)); }
    // a line across the card
    CardText*    note(const wxString& s)
    {
        CardText* t = add_field(wxString(), new CardText(s));
        m_rows.back().kind = CardRow::Kind::Note;
        return t;
    }
    // The wx-style two steps: make a field, set it up, then give it a row. (A field with no row is never drawn.)
    ChoiceField* make_combo() { return own(new ChoiceField()); }
    NumField*    make_spin(double v, double lo = 0.1, double hi = 1000.0) { return own(new NumField(v, lo, hi)); }
    ToggleField* make_check() { return own(new ToggleField()); }
    // a picked reference: label on the left, the slot on the right
    SlotField*   slot(const wxString& label, const wxString& hint) { SlotField* f = add_field(label, new SlotField()); f->SetHint(hint); return f; }
    CardText*    make_text(const wxString& s = wxString()) { return own(new CardText(s)); }
    CheckListField* make_checklist() { return own(new CheckListField()); }
    // a list of rows across the card (the tree, the bodies)
    DesignRowList* make_rows(int max_visible, bool selectable = true)
    {
        RowsField* f = own(new RowsField(max_visible, selectable));
        m_rows.push_back({CardRow::Kind::Wide, wxString(), f, nullptr, m_group});
        return f->list();
    }
    TableField*  make_table()
    {
        TableField* t = own(new TableField());
        m_rows.push_back({CardRow::Kind::Wide, wxString(), t, nullptr, m_group});
        return t;
    }
    ListField*   make_list() { return own(new ListField()); }
    TextField*   make_edit() { return own(new TextField()); }
    void         buttons(ButtonField* a, ButtonField* b) { m_rows.push_back({CardRow::Kind::Buttons, wxString(), a, b, m_group}); }
    ButtonField* make_button(const wxString& s) { return own(new ButtonField(s)); }
    void         row(const wxString& label, CardField* f) { m_rows.push_back({CardRow::Kind::Field, label, f, nullptr, m_group}); }
    void         row_t(CardText* label, CardField* f) { m_rows.push_back({CardRow::Kind::Field, wxString(), f, label, m_group}); }
    void         wide(CardField* f) { m_rows.push_back({CardRow::Kind::Wide, wxString(), f, nullptr, m_group}); }
    void         note_text(CardText* t) { m_rows.push_back({CardRow::Kind::Note, wxString(), t, nullptr, m_group}); }
    // a button in the control column (the label column stays empty)
    ButtonField* button(const wxString& label, const wxString& text) { return add_field(label, new ButtonField(text)); }
    // a list across the card, under a caption
    CheckListField* checklist(const wxString& caption)
    {
        note(caption);
        CheckListField* l = add_field(wxString(), new CheckListField());
        m_rows.back().kind = CardRow::Kind::Wide;
        return l;
    }
    // a button on the left and the text it fills in on the right
    void pick(const wxString& button_text, ButtonField*& b, CardText*& t, const wxString& initial)
    {
        b = new ButtonField(button_text);
        t = new CardText(initial);
        m_fields.emplace_back(b);
        m_fields.emplace_back(t);
        b->SetNextHandler(m_sink);
        t->SetNextHandler(m_sink);
        CardRow r{CardRow::Kind::Pick, wxString(), b, t, m_group};
        m_rows.push_back(r);
    }
    // The rows added until end_group() show and hide together.
    CardGroup*   begin_group() { m_groups.emplace_back(new CardGroup()); m_group = m_groups.back().get(); return m_group; }
    void         end_group() { m_group = nullptr; }

private:
    template<class T> T* own(T* f)
    {
        m_fields.emplace_back(f);
        f->SetNextHandler(m_sink);
        return f;
    }
    template<class T> T* add_field(const wxString& label, T* f)
    {
        m_fields.emplace_back(f);
        f->SetNextHandler(m_sink);
        m_rows.push_back({CardRow::Kind::Field, label, f, nullptr, m_group});
        return f;
    }

    std::string                             m_icon;
    bool                                    m_shown{true};
    std::vector<HeaderButton>               m_header_buttons;
    std::string                             m_fold_key;
    std::unique_ptr<CardText>               m_title;
    std::vector<std::unique_ptr<CardField>> m_fields;
    std::vector<CardRow>                    m_rows;
    std::vector<std::unique_ptr<CardGroup>> m_groups;
    CardGroup*                              m_group{nullptr};
    wxEvtHandler*                           m_sink{nullptr};
};

}} // namespace Slic3r::GUI

#endif
