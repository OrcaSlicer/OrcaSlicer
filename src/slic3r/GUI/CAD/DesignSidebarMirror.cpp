// The Design tab's left panel drawn with ImGui, straight from the card tables (DesignCardModel.hpp): each tool card, the feature
// tree, the bodies and the variables is a CardModel (a title, header buttons and rows of label + field), the lists are
// DesignRowLists, and the panel reads and writes the fields through the same small API the wx controls had. A change made in the
// panel is queued as the event the field would have sent, so the handlers (preview, validation, undo) run on the next turn of the
// event loop and not inside the ImGui frame.
//
// To add a control to a card: one line in the card's builder in DesignPanel.cpp. Nothing here has to know about it.

#include "libslic3r/LocalesUtils.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignCardModel.hpp"
#include "slic3r/GUI/CAD/DesignImGuiUi.hpp"
#include "slic3r/GUI/CAD/DesignRowList.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"

#include <boost/log/trivial.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <wx/accel.h>
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/listbase.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/window.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cmath>
#include <set>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

struct SidebarCtx {
    float em{1.f};
    int   depth{0};
    // a rename in progress in one of the row lists (the wx editor would open invisibly in the hidden pane)
    DesignRowList* rename_list{nullptr};
    int            rename_row{-1};
    std::string*   rename_text{nullptr};
    bool*          rename_focus{nullptr};
    std::function<void(bool commit)> rename_done;
    // A queued action may leave the keyboard on a hidden wx control (a list that took the focus when it selected a row); the
    // canvas must have it back, or the next key never reaches the ImGui field.
    std::function<void()> refocus;
};

namespace {

std::string u8(const wxString& s) { return s.utf8_string(); }

ImVec4 col_of(const wxColour& c) { return ImVec4(c.Red() / 255.f, c.Green() / 255.f, c.Blue() / 255.f, 1.f); }

// A colour worth honouring (a status green or red), as opposed to the panel's plain text colour.
bool is_accent(const wxColour& c)
{
    const int mx = std::max({int(c.Red()), int(c.Green()), int(c.Blue())}), mn = std::min({int(c.Red()), int(c.Green()), int(c.Blue())});
    return c.IsOk() && mx - mn > 60;
}

void tip_of(wxWindow* w)
{
    if (!ImGui::IsItemHovered()) return;
    const wxString t = w->GetToolTipText();
    if (!t.IsEmpty()) ImGui::SetTooltip("%s", u8(t).c_str());
}

// Queue (never process) an event: the handler runs on the next turn of the loop, outside the ImGui frame.
void post(wxWindow* w, wxEvent& e)
{
    e.SetEventObject(w);
    wxQueueEvent(w->GetEventHandler(), e.Clone());
}

void post_command(wxWindow* w, wxEventType type, int value = 0, const wxString& s = wxString())
{
    wxCommandEvent e(type, w->GetId());
    e.SetInt(value);
    e.SetString(s);
    post(w, e);
}

// A number as the spin shows it: its own digits, trailing zeros kept.
std::string fmt_number(double v, int digits)
{
    return float_to_string_decimal_point(v, std::clamp(digits, 0, 6));   // a '.', whatever the UI language: the field only accepts one
}

// What the field being typed in holds, and which control owns it.
const void* g_edit_owner = nullptr;
std::string g_edit_text;

// An ImGui text field over a value the user types: shows the control's value until it is clicked into, keeps the typed text
// while it is active, and hands it back on Enter or when focus leaves after an edit.
bool text_field(const void* owner, const char* id, const std::string& shown, std::string& out, float width, ImGuiInputTextFlags flags)
{
    char buf[256];
    const std::string& src = (g_edit_owner == owner) ? g_edit_text : shown;
    std::snprintf(buf, sizeof(buf), "%s", src.c_str());
    if (width > 0.f) ImGui::SetNextItemWidth(width);
    const bool enter = ImGui::InputText(id, buf, sizeof(buf), flags | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemActivated()) g_edit_owner = owner;
    if (ImGui::IsItemActive()) wxGetApp().imgui()->set_requires_extra_frame();   // keep frames coming while a field is being typed in
    if (g_edit_owner == owner) g_edit_text = buf;
    bool commit = enter || ImGui::IsItemDeactivatedAfterEdit();
    if (commit || ImGui::IsItemDeactivated()) { if (g_edit_owner == owner) g_edit_owner = nullptr; }
    if (commit) out = buf;
    return commit;
}

} // namespace

// ---- one control ---------------------------------------------------------------------------------------------------

namespace {

// ---- the card tables (DesignCardModel.hpp): the fields are drawn and set directly, with no wx control behind them ----------

void field_tip(const CardField* f)
{
    if (ImGui::IsItemHovered() && !f->tip().IsEmpty()) ImGui::SetTooltip("%s", u8(f->tip()).c_str());
}

struct DisabledScope {   // a field that cannot be edited is dimmed and takes no input
    bool on;
    explicit DisabledScope(bool enabled) : on(!enabled)
    {
        if (on) { ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true); ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f); }
    }
    ~DisabledScope() { if (on) { ImGui::PopStyleVar(); ImGui::PopItemFlag(); } }
};

void draw_num_field(NumField* sp, float em, float width)
{
    ImGui::PushID(sp);
    DisabledScope dis(sp->IsEnabled());
    const double lo = sp->GetMin(), hi = sp->GetMax(), inc = sp->GetIncrement() > 0.0 ? sp->GetIncrement() : 1.0;
    const double v = sp->GetValue();
    const float  bw = ImGui::GetFrameHeight();
    const float  total = width > 0.f ? width : ImGui::GetContentRegionAvail().x;
    std::string typed;
    if (text_field(sp, "##v", fmt_number(v, sp->GetDigits()), typed, std::max(total - 2.f * bw - 2.f * ImGui::GetStyle().ItemSpacing.x, 40.f * em), ImGuiInputTextFlags_CharsDecimal)) {
        size_t used = 0;
        const double nv = string_to_double_decimal_point(typed, &used);   // not strtod: it stops at a '.' under a decimal-comma locale
        if (used > 0) sp->user_set(nv);
    }
    ImGui::SameLine();
    if (ImGui::Button("-", ImVec2(bw, 0.f))) sp->user_set(std::clamp(v - inc, lo, hi));
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::Button("+", ImVec2(bw, 0.f))) sp->user_set(std::clamp(v + inc, lo, hi));
    ImGui::PopID();
    field_tip(sp);
}

void draw_choice_field(ChoiceField* cb, float width)
{
    ImGui::PushID(cb);
    DisabledScope dis(cb->IsEnabled());
    const int n = int(cb->GetCount()), sel = cb->GetSelection();
    const std::string preview = (sel >= 0 && sel < n) ? u8(cb->GetString(unsigned(sel))) : std::string();
    ImGui::SetNextItemWidth(width > 0.f ? width : -1.f);
    if (ImGui::BeginCombo("##c", preview.c_str())) {
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(u8(cb->GetString(unsigned(i))).c_str(), i == sel))
                cb->user_select(i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    field_tip(cb);
}

void draw_toggle_field(ToggleField* cb)
{
    ImGui::PushID(cb);
    DisabledScope dis(cb->IsEnabled());
    bool v = cb->GetValue();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);   // an unchecked box has to be seen on the dark panel
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.56f, 0.60f, 1.f));
    if (ImGui::Checkbox("##k", &v)) cb->user_toggle();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopID();
    field_tip(cb);
}

void draw_list_field(ListField* l, float em)
{
    ImGui::PushID(l);
    constexpr int kMaxRows = 8;   // the list scrolls past that
    const int n = int(l->GetCount());
    if (n == 0) {
        ImGui::TextDisabled("%s", u8(l->empty_text()).c_str());
        ImGui::PopID();
        return;
    }
    const float h = float(std::min(n, kMaxRows)) * ImGui::GetFrameHeightWithSpacing() + 4.f * em;
    if (ImGui::BeginChild("##rows", ImVec2(0.f, h), false)) {
        const ImTextureID tex = (ImTextureID) design_icon_texture("design_delete");
        const float isz = 14.f * em;
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
            const bool del = tex ? ImGui::ImageButton(tex, ImVec2(isz, isz), ImVec2(0, 0), ImVec2(1, 1), 1) : ImGui::SmallButton("x");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _u8L("Delete constraint").c_str());
            if (del) l->user_delete(i);
            ImGui::SameLine();
            if (ImGui::Selectable(u8(l->GetString(unsigned(i))).c_str(), i == l->GetSelection())) l->user_click(i);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void draw_edit_field(TextField* t, float width)
{
    ImGui::PushID(t);
    DisabledScope dis(t->IsEnabled());
    if (t->take_focus()) ImGui::SetKeyboardFocusHere();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", u8(t->GetValue()).c_str());
    ImGui::SetNextItemWidth(width > 0.f ? width : -1.f);
    const bool enter = ImGui::InputText("##e", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemActive()) wxGetApp().imgui()->set_requires_extra_frame();   // keep frames coming while a field is being typed in
    const wxString nv = wxString::FromUTF8(buf);
    if (nv != t->GetValue()) t->ChangeValue(nv);
    if (enter) t->user_enter();
    ImGui::PopID();
}

// A choice the user may also type in: the text, and an arrow that lists what is known.
void draw_editable_choice(ChoiceField* cb, float width)
{
    ImGui::PushID(cb);
    DisabledScope dis(cb->IsEnabled());
    const float bw = ImGui::GetFrameHeight();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", u8(cb->GetValue()).c_str());
    ImGui::SetNextItemWidth(std::max((width > 0.f ? width : ImGui::GetContentRegionAvail().x) - bw - ImGui::GetStyle().ItemSpacing.x, 40.f));
    if (ImGui::InputText("##t", buf, sizeof(buf)))
        cb->SetValue(wxString::FromUTF8(buf));
    if (ImGui::IsItemActive()) wxGetApp().imgui()->set_requires_extra_frame();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##d", ImGuiDir_Down)) ImGui::OpenPopup("##list");
    if (ImGui::BeginPopup("##list")) {
        for (unsigned i = 0; i < cb->GetCount(); ++i) {
            ImGui::PushID(int(i));
            if (ImGui::Selectable(u8(cb->GetString(i)).c_str(), int(i) == cb->GetSelection())) cb->user_select(int(i));
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void draw_button_field(ButtonField* b, float em)
{
    (void) em;
    ImGui::PushID(b);
    DisabledScope dis(b->IsEnabled());
    const std::string t = u8(b->GetLabel());
    if (ImGui::Button(t.empty() ? "?" : t.c_str())) b->user_click();
    ImGui::PopID();
    field_tip(b);
}

void draw_check_list(CheckListField* l, float em)
{
    ImGui::PushID(l);
    DisabledScope dis(l->IsEnabled());
    const float h = std::min(std::max(float(l->GetCount()), 3.f), 7.f) * ImGui::GetFrameHeightWithSpacing() + 4.f * em;
    if (ImGui::BeginChild("##list", ImVec2(0.f, h), true)) {
        for (unsigned i = 0; i < l->GetCount(); ++i) {
            ImGui::PushID(int(i));
            bool on = l->IsChecked(i);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);   // an unchecked box has to be seen on the dark panel
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.56f, 0.60f, 1.f));
            if (ImGui::Checkbox(u8(l->GetString(i)).c_str(), &on)) l->user_check(i);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

// A line of text, in the colour the code gave it when that is a status colour.
void draw_card_text(CardText* t)
{
    const wxColour c = t->colour();
    const bool accent = c.IsOk() && is_accent(c);
    if (accent) ImGui::PushStyleColor(ImGuiCol_Text, col_of(c));
    ImGui::TextWrapped("%s", u8(t->GetLabel()).c_str());
    if (accent) ImGui::PopStyleColor();
}

// The rows of a DesignRowList (the feature tree, the Origin / Bed rows, the bodies): the owner's own callbacks do the work.
void draw_rows(DesignRowList* list, float em, SidebarCtx& cx)
{
    ImGui::PushID(list);
    const auto& rows = list->rows();
    const int sel = list->selection();
    const bool selectable = list->selectable();
    const float row_h = std::max(20.f * em, ImGui::GetFrameHeight());
    const float isz   = 16.f * em;
    for (int i = 0; i < int(rows.size()); ++i) {
        const DesignRowList::Row& r = rows[size_t(i)];
        ImGui::PushID(i);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float cell  = isz + 6.f * em;
        const float acts_w = cell * float(r.actions.size());
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool selected = selectable && i == sel;
        ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowItemOverlap, ImVec2(std::max(avail - acts_w, 20.f * em), row_h));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            list->CallAfter([list, i, dbl, selectable, rf = cx.refocus] {
                if (selectable) list->select(i);
                if (dbl && list->on_activate) list->on_activate();
                if (rf) rf();
            });
        }
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && list->on_menu) {
            const wxPoint pos = wxGetMousePosition();
            list->CallAfter([list, i, pos, selectable, rf = cx.refocus] {
                if (selectable) list->select(i);
                list->on_menu(i, pos);
                if (rf) rf();
            });
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (cx.rename_list == list && cx.rename_row == i && cx.rename_text != nullptr) {   // the field replaces the label
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 4.f * em + (r.icon.empty() ? 0.f : isz + 6.f * em), p0.y + 1.f));
            if (cx.rename_focus != nullptr && *cx.rename_focus) { ImGui::SetKeyboardFocusHere(); *cx.rename_focus = false; }
            wxGetApp().imgui()->set_requires_extra_frame();   // the canvas draws on demand: the field is only active on the NEXT frame, and keys typed before it is are not its own
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%s", cx.rename_text->c_str());
            ImGui::SetNextItemWidth(std::max(avail - acts_w - isz - 14.f * em, 60.f * em));
            const bool entered = ImGui::InputText("##rename", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            *cx.rename_text = buf;
            if (entered) { if (cx.rename_done) cx.rename_done(true); }
            else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)) || ImGui::IsItemDeactivated()) { if (cx.rename_done) cx.rename_done(false); }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + row_h + ImGui::GetStyle().ItemSpacing.y));
            continue;
        }
        float x = p0.x + 4.f * em;
        if (!r.icon.empty()) {
            if (const ImTextureID tex = (ImTextureID) design_icon_texture(r.icon))
                dl->AddImage(tex, ImVec2(x, p0.y + (row_h - isz) * 0.5f), ImVec2(x + isz, p0.y + (row_h + isz) * 0.5f));
            x += isz + 6.f * em;
        }
        ImVec4 c = r.colour.IsOk() ? col_of(r.colour) : ImGui::GetStyle().Colors[ImGuiCol_Text];
        dl->AddText(ImVec2(x, p0.y + (row_h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(c), u8(r.label).c_str());
        // the action cells, at the right end of the row
        if (!r.actions.empty()) {
            ImGui::SameLine(0.f, 0.f);
            ImGui::SetCursorScreenPos(ImVec2(p0.x + avail - acts_w, p0.y));
        }
        for (size_t a = 0; a < r.actions.size(); ++a) {
            const DesignRowList::Action& act = r.actions[a];
            ImGui::PushID(int(a));
            if (a > 0) ImGui::SameLine(0.f, 0.f);
            const ImTextureID tex = act.icon.empty() ? nullptr : (ImTextureID) design_icon_texture(act.icon);
            if (tex) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f * em, (row_h - isz) * 0.5f));
                if (ImGui::ImageButton(tex, ImVec2(isz, isz)) && list->on_action) {
                    const int id = act.id;
                    list->CallAfter([list, i, id, rf = cx.refocus] { if (list->on_action) list->on_action(i, id); if (rf) rf(); });
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && !act.tip.IsEmpty()) ImGui::SetTooltip("%s", u8(act.tip).c_str());
            } else {
                ImGui::Dummy(ImVec2(cell, row_h));
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (rows.empty()) ImGui::Dummy(ImVec2(1.f, 4.f * em));
    ImGui::PopID();
}

// The variables table (name, expression); a double click edits through the panel's own dialog.
void draw_table_field(TableField* t, float em, std::function<void()> refocus)
{
    (void) em;
    ImGui::PushID(t);
    const int n = t->GetItemCount();
    const int sel = t->GetSelection();
    const int cols = int(t->columns().size());
    if (cols > 0 && ImGui::BeginTable("##vars", cols, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (int c = 0; c < cols; ++c)
            ImGui::TableSetupColumn(u8(t->columns()[size_t(c)]).c_str(), ImGuiTableColumnFlags_WidthStretch, c == 0 ? 0.4f : 0.6f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; ++i) {
            ImGui::TableNextRow();
            if (t->highlight.count(u8(t->GetItemText(i, 0))))
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.18f, 0.65f, 0.59f, 0.55f)));
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            if (ImGui::Selectable(u8(t->GetItemText(i, 0)).c_str(), i == sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                t->user_select(i, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                if (refocus) t->CallAfter(refocus);
            }
            for (int c = 1; c < cols; ++c) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted(u8(t->GetItemText(i, c)).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

} // namespace

// ---- walking the form ----------------------------------------------------------------------------------------------

namespace {
std::set<std::string>& warned() { static std::set<std::string> s; return s; }
}

// One card of the tables: its header, then a label and its field per row (the same 42 % / 58 % split the wx forms had).
void DesignPanel::mirror_card(CardModel& card, SidebarCtx& cx)
{
    const float em = cx.em;
    const bool folded = !card.fold_key().empty() && is_folded(card.fold_key().c_str());
    if (!card.fold_key().empty()) {
        const std::string key = card.fold_key();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushID(key.c_str());
        if (ImGui::ArrowButtonEx("##fold", folded ? ImGuiDir_Right : ImGuiDir_Down, ImVec2(ImGui::GetFrameHeight() * 0.8f, ImGui::GetFrameHeight() * 0.8f)))
            CallAfter([this, key] { toggle_fold(key); });
        ImGui::PopID();
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (const ImTextureID tex = (ImTextureID) design_icon_texture(card.icon())) {
        ImGui::Image(tex, ImVec2(18.f * em, 18.f * em));
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    ImGui::TextUnformatted(u8(card.title()->GetLabel()).c_str());
    ImGui::PopStyleColor();
    if (!card.header_buttons().empty()) {   // icon buttons at the right end of the header
        const float isz = 16.f * em, cell_w = isz + 8.f * em;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - cell_w * float(card.header_buttons().size()));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        for (size_t i = 0; i < card.header_buttons().size(); ++i) {
            const CardModel::HeaderButton& hb = card.header_buttons()[i];
            ImGui::PushID(int(i));
            if (i > 0) ImGui::SameLine(0.f, 0.f);
            const ImTextureID tex = (ImTextureID) design_icon_texture(hb.icon);
            const bool clicked = tex ? ImGui::ImageButton(tex, ImVec2(isz, isz)) : ImGui::Button("?");
            if (ImGui::IsItemHovered() && !hb.tip.IsEmpty()) ImGui::SetTooltip("%s", u8(hb.tip).c_str());
            if (clicked) CallAfter(hb.fn);
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    if (folded) return;
    const float lw = ImGui::GetContentRegionAvail().x * 0.42f;
    auto cell = [&](CardField* f) {
        const float w = std::max(ImGui::GetContentRegionAvail().x, 60.f * em);
        // A number's box leaves room for its - and + buttons; every other input is held to that same width so they line up.
        const float box_w = std::max(w - (2.f * ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().ItemSpacing.x), 40.f * em);
        switch (f->kind()) {
        case CardField::Kind::Num:      draw_num_field(static_cast<NumField*>(f), em, w);       break;
        case CardField::Kind::Choice:   if (static_cast<ChoiceField*>(f)->IsEditable()) draw_editable_choice(static_cast<ChoiceField*>(f), box_w); else draw_choice_field(static_cast<ChoiceField*>(f), box_w); break;
        case CardField::Kind::Edit:     draw_edit_field(static_cast<TextField*>(f), box_w);      break;
        case CardField::Kind::List:     draw_list_field(static_cast<ListField*>(f), em);         break;
        case CardField::Kind::Rows:     draw_rows(static_cast<RowsField*>(f)->list(), em, cx);   break;
        case CardField::Kind::Table: {
            auto* t = static_cast<TableField*>(f);
            t->highlight.clear();
            if (t == m_var_list && m_viewport != nullptr) {   // hovering a sketch segment lights up the variables behind its dimensions
                std::set<std::string> known;
                for (int i = 0; i < t->GetItemCount(); ++i) known.insert(u8(t->GetItemText(i, 0)));
                t->highlight = m_viewport->mcp_sketch_tool().hover_variable_names(known);
            }
            draw_table_field(t, em, cx.refocus);
            break;
        }
        case CardField::Kind::Toggle:   draw_toggle_field(static_cast<ToggleField*>(f));        break;
        case CardField::Kind::Button:   draw_button_field(static_cast<ButtonField*>(f), em);    break;
        case CardField::Kind::CheckList: draw_check_list(static_cast<CheckListField*>(f), em);  break;
        case CardField::Kind::Text:     ImGui::AlignTextToFramePadding(); draw_card_text(static_cast<CardText*>(f)); break;
        }
    };
    for (const CardRow& row : card.rows()) {
        CardField* f = row.field;
        if (!f->IsShown() || (row.group != nullptr && !row.group->IsShown())) continue;
        ImGui::PushID(f);
        if (row.kind == CardRow::Kind::Note) {
            draw_card_text(static_cast<CardText*>(f));
        } else if (row.kind == CardRow::Kind::Wide) {
            cell(f);
        } else if (row.kind == CardRow::Kind::Buttons) {
            draw_button_field(static_cast<ButtonField*>(f), em);
            ImGui::SameLine();
            draw_button_field(static_cast<ButtonField*>(row.field2), em);
        } else if (row.kind == CardRow::Kind::Pick) {
            draw_button_field(static_cast<ButtonField*>(f), em);
            ImGui::SameLine(lw + ImGui::GetStyle().WindowPadding.x);
            ImGui::AlignTextToFramePadding();
            draw_card_text(static_cast<CardText*>(row.field2));
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + lw - 6.f * em);
            ImGui::TextWrapped("%s", u8(row.field2 != nullptr ? static_cast<CardText*>(row.field2)->GetLabel() : row.label).c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine(lw + ImGui::GetStyle().WindowPadding.x);
            cell(f);
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void DesignPanel::draw_sidebar_imgui()
{
    SidebarCtx cx;
    cx.em = wxGetApp().imgui()->get_style_scaling();
    cx.refocus      = [this] { if (m_viewport != nullptr) m_viewport->SetFocus(); };
    cx.rename_list  = m_sb_rename_list;
    cx.rename_row   = m_sb_rename_row;
    cx.rename_text  = &m_sb_rename_text;
    cx.rename_focus = &m_sb_rename_focus;
    cx.rename_done  = [this](bool commit) {
        DesignRowList* l = m_sb_rename_list;
        const int row = m_sb_rename_row;
        std::string name = m_sb_rename_text;
        m_sb_rename_list = nullptr; m_sb_rename_row = -1;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (commit && l != nullptr && !name.empty())   // an empty commit cancels, as the wx editor does
            CallAfter([l, row, name] { if (l->on_rename) l->on_rename(row, wxString::FromUTF8(name)); });
    };
    // collapse the panel (the canvas shows an arrow at its edge to bring it back)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    if (ImGui::ArrowButtonEx("##collapse", ImGuiDir_Left, ImVec2(ImGui::GetFrameHeight() * 0.8f, ImGui::GetFrameHeight() * 0.8f)))
        CallAfter([this] { collapse_sidebar(true); });
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _u8L("Collapse sidebar").c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    ImGui::TextUnformatted(_u8L("Design").c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    // the tool cards (one is open at a time), then the tree, the bodies and the variables
    for (const auto& c : m_card_models)
        if (c->IsShown()) { mirror_card(*c, cx); ImGui::Spacing(); }
    for (CardModel* c : { m_tree_box, m_parts_box, m_var_box })
        if (c != nullptr && c->IsShown()) { mirror_card(*c, cx); ImGui::Spacing(); }
    if (m_dof_status != nullptr && m_dof_status->IsShown() && !m_dof_status->GetLabel().IsEmpty())
        draw_card_text(m_dof_status);
}

}} // namespace Slic3r::GUI
