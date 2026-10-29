#include "libslic3r/Config.hpp"
#include "UiInspector.hpp"

#include "CanvasTarget.hpp"
#include "McpService.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/BBLTopbar.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/ExtraRenderers.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSVG.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/KeyChord.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SceneRaycaster.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/SideButton.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/DropDown.hpp"
#include "slic3r/GUI/Widgets/SpinInput.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "UiRefs.hpp"
#ifdef SLIC3R_CAD
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignRowList.hpp"
#endif

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <wx/anybutton.h>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/chartype.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/ctrlsub.h>
#include <wx/dataview.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/dvrenderers.h>
#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/grid.h>
#include <wx/kbdstate.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/mousestate.h>
#include <wx/notebook.h>
#include <wx/popupwin.h>
#include <wx/radiobut.h>
#include <wx/scrolwin.h>
#include <wx/slider.h>
#include <wx/spinbutt.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/arrstr.h>
#include <wx/tracker.h>
#include <wx/utils.h>
#include <wx/variant.h>
#include <wx/weakref.h>
#include <wx/webview.h>
#include <wx/window.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string short_class(const wxWindow* w)
{
    std::string c = class_name(w);
    if (const auto lt = c.find('<'); lt != std::string::npos)
        c = c.substr(0, lt);
    if (const auto colon = c.rfind("::"); colon != std::string::npos)
        c = c.substr(colon + 2);
    return c;
}

// The role a window plays for an agent; empty for plain containers.
std::string kind_of(wxWindow* w)
{
    if (dynamic_cast<wxPopupWindow*>(w) != nullptr)
        return "popup";
    if (dynamic_cast<wxDialog*>(w) != nullptr)
        return "dialog";
    if (dynamic_cast<wxTopLevelWindow*>(w) != nullptr)
        return "window";
    if (dynamic_cast<wxGLCanvas*>(w) != nullptr)
        return "canvas";
    if (dynamic_cast<wxWebView*>(w) != nullptr)
        return "webview";
    if (dynamic_cast<::Button*>(w) != nullptr || dynamic_cast<wxButton*>(w) != nullptr || dynamic_cast<::SideButton*>(w) != nullptr)
        return "button";
    if (dynamic_cast<wxCheckBox*>(w) != nullptr)
        return "checkbox";
    if (dynamic_cast<wxBitmapToggleButton*>(w) != nullptr || dynamic_cast<wxToggleButton*>(w) != nullptr)
        return "toggle";
    if (dynamic_cast<wxRadioButton*>(w) != nullptr)
        return "radio";
    if (dynamic_cast<::ComboBox*>(w) != nullptr || dynamic_cast<wxComboBox*>(w) != nullptr)
        return "combo";
    if (dynamic_cast<wxChoice*>(w) != nullptr)
        return "choice";
    if (dynamic_cast<::SpinInput*>(w) != nullptr || dynamic_cast<wxSpinCtrl*>(w) != nullptr || dynamic_cast<wxSpinCtrlDouble*>(w) != nullptr)
        return "spin";
    if (dynamic_cast<::TextInput*>(w) != nullptr || dynamic_cast<wxTextCtrl*>(w) != nullptr)
        return "text";
    if (dynamic_cast<wxSlider*>(w) != nullptr)
        return "slider";
    if (dynamic_cast<wxListBox*>(w) != nullptr || dynamic_cast<wxDataViewCtrl*>(w) != nullptr || dynamic_cast<wxGrid*>(w) != nullptr)
        return "list";
#ifdef SLIC3R_CAD
    if (dynamic_cast<DesignRowList*>(w) != nullptr)
        return "list";
#endif
    if (dynamic_cast<wxBookCtrlBase*>(w) != nullptr)
        return "tabs";
    if (dynamic_cast<wxStaticText*>(w) != nullptr)
        return "label";
    // Custom-drawn controls (Orca's StaticBox family and the like) show a label but have no
    // class of their own here.
    if (w->GetChildren().empty() && !w->GetLabel().empty())
        return "control";
    return {};
}

wxTextEntry* text_entry(wxWindow* w)
{
    if (dynamic_cast<::ComboBox*>(w) != nullptr)
        return nullptr; // a TextInput too, but its value is the selection
    if (auto* ti = dynamic_cast<::TextInput*>(w))
        return ti->GetTextCtrl();
    return dynamic_cast<wxTextEntry*>(w);
}

std::string value_of(wxWindow* w)
{
    if (auto* web = dynamic_cast<wxWebView*>(w))
        return into_u8(web->GetCurrentURL());
    if (auto* spin = dynamic_cast<::SpinInput*>(w))
        return std::to_string(spin->GetValue());
    if (auto* spin = dynamic_cast<wxSpinCtrl*>(w))
        return std::to_string(spin->GetValue());
    if (auto* spin = dynamic_cast<wxSpinCtrlDouble*>(w))
        return std::to_string(spin->GetValue());
    if (auto* slider = dynamic_cast<wxSlider*>(w))
        return std::to_string(slider->GetValue());
    if (auto* items = dynamic_cast<wxItemContainer*>(w); items != nullptr && dynamic_cast<wxListBox*>(w) == nullptr) {
        const int sel = items->GetSelection();
        if (sel != wxNOT_FOUND)
            return into_u8(items->GetString(unsigned(sel)));
    }
    if (wxTextEntry* te = text_entry(w))
        return into_u8(te->GetValue());
    return {};
}

int checked_of(wxWindow* w)
{
    if (auto* c = dynamic_cast<wxCheckBox*>(w))
        return c->GetValue() ? 1 : 0;
    if (auto* t = dynamic_cast<wxBitmapToggleButton*>(w))
        return t->GetValue() ? 1 : 0;
    if (auto* t = dynamic_cast<wxToggleButton*>(w))
        return t->GetValue() ? 1 : 0;
    if (auto* r = dynamic_cast<wxRadioButton*>(w))
        return r->GetValue() ? 1 : 0;
    return -1;
}

std::string option_key_of(wxWindow* w);

// The text left of a control on its row: how Orca's forms label their fields.
std::string row_label(wxWindow* w)
{
    wxWindow* parent = w->GetParent();
    if (parent == nullptr)
        return {};
    const wxRect r  = w->GetRect();
    const int    cy = r.y + r.height / 2;
    wxWindow*    best    = nullptr;
    int          best_dx = INT_MAX;
    for (wxWindow* sib : parent->GetChildren()) {
        if (sib == w || !sib->IsShown() || sib->GetLabel().Trim().empty())
            continue;
        if (dynamic_cast<wxStaticText*>(sib) == nullptr && kind_of(sib) != "control")
            continue;
        const wxRect s = sib->GetRect();
        if (s.GetRight() > r.x || cy < s.y || cy > s.GetBottom())
            continue;
        if (r.x - s.GetRight() < best_dx) {
            best_dx = r.x - s.GetRight();
            best    = sib;
        }
    }
    if (best == nullptr)
        return {};
    wxString label = best->GetLabel();
    label.Replace("\n", " ");
    return into_u8(label.Trim().Trim(false));
}

// The label of a setting from its definition, in the UI language.
std::string option_label(const std::string& key)
{
    const ConfigOptionDef* def = print_config_def.get(key.substr(0, key.find('[')));
    return def == nullptr || def->label.empty() ? std::string() : into_u8(_(def->label));
}

std::string label_of(wxWindow* w)
{
    // A popup has no title, and a drop-down list's tooltip is the hovered item's.
    if (dynamic_cast<wxPopupWindow*>(w) != nullptr)
        return {};
    if (auto* tlw = dynamic_cast<wxTopLevelWindow*>(w))
        return into_u8(tlw->GetTitle());
    // Input fields are named by the setting they edit or the label on their row; their own label is
    // a unit ("mm") or nothing.
    const std::string kind = kind_of(w);
    if (kind == "text" || kind == "spin" || kind == "combo" || kind == "choice" || kind == "slider" || kind == "toggle" ||
        ((kind == "checkbox" || kind == "radio") && w->GetLabel().Trim().empty())) {
        if (const std::string key = option_key_of(w); !key.empty())
            if (std::string l = option_label(key); !l.empty())
                return l;
        if (std::string l = row_label(w); !l.empty())
            return l;
    }
    wxString label = w->GetLabel();
    if (label.Trim().Trim(false).empty() && dynamic_cast<wxStaticText*>(w) == nullptr) {
        // Icon buttons say what they do in their tooltip.
        label = w->GetToolTipText().BeforeFirst('\n');
        if (label.StartsWith(_L("parameter name") + ": "))
            label.clear();
    }
    label.Replace("\n", " ");
    std::string out = into_u8(wxStripMenuCodes(label));
    if (out.size() > 120)
        out = out.substr(0, 117) + "...";
    return out;
}

// Orca's setting fields have a "parameter name: <option key>" line in their tooltip.
std::string option_key_of(wxWindow* w)
{
    auto from_tooltip = [](wxWindow* x) -> std::string {
        const wxString prefix = _L("parameter name") + ": ";
        for (const wxString& line : wxSplit(x->GetToolTipText(), '\n', '\0')) {
            if (!line.StartsWith(prefix))
                continue;
            const std::string key = into_u8(line.Mid(prefix.length()).Trim());
            const bool ok = !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '[' || c == ']';
            });
            return ok ? key : std::string();
        }
        return {};
    };
    if (std::string key = from_tooltip(w); !key.empty())
        return key;
    // TextInput and SpinInput put the tooltip on their inner text control.
    if (dynamic_cast<::TextInput*>(w) != nullptr || dynamic_cast<::SpinInput*>(w) != nullptr)
        for (wxWindow* child : w->GetChildren())
            if (std::string key = from_tooltip(child); !key.empty())
                return key;
    return {};
}

// wx gives every window a default name for its class; those say nothing.
std::string meaningful_name(wxWindow* w)
{
    static const std::set<std::string> defaults{"panel", "frame", "dialog", "button", "text", "staticText", "checkbox", "radioButton",
                                                "comboBox", "choice", "listBox", "GLCanvas", "notebook", "wxSpinCtrl", "wxSpinCtrlDouble",
                                                "slider", "scrolledWindow", "bitmapButton", "toggle", "staticBitmap", "staticBox",
                                                "hyperlink", "dataviewCtrl", "listCtrl", "webView", "staticLine", "splitter", "toolbar",
                                                "statusBar", "control", "wxWebView", "auiToolBar", "simplebook", "book", "check", ""};
    const std::string name = into_u8(w->GetName());
    return defaults.count(name) != 0 || name == short_class(w) ? std::string() : name;
}

wxPoint client_pos_in_tlw(wxWindow* w)
{
    wxWindow* tlw = wxGetTopLevelParent(w);
    if (tlw == nullptr || tlw == w)
        return {0, 0};
    return w->ClientToScreen(wxPoint(0, 0)) - tlw->ClientToScreen(wxPoint(0, 0));
}

// Shown top-level windows: the main frame first, then the others (modal dialogs last, in the
// order they opened).
std::vector<wxTopLevelWindow*> top_level_windows(McpService& service)
{
    std::vector<wxTopLevelWindow*> out;
    if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->IsShown())
        out.push_back(wxGetApp().mainframe);
    const auto& modals = service.modals().stack();
    for (wxWindow* w : wxTopLevelWindows) {
        auto* tlw = dynamic_cast<wxTopLevelWindow*>(w);
        if (tlw == nullptr || !tlw->IsShown() || tlw == wxGetApp().mainframe)
            continue;
        if (std::find(modals.begin(), modals.end(), tlw) != modals.end())
            continue;
        out.push_back(tlw);
    }
    for (wxDialog* d : modals)
        if (d->IsShown())
            out.push_back(d);
    return out;
}

// The menus of the main window: its menu bar on macOS, the title bar's File and dropdown menus
// elsewhere.
std::vector<std::pair<std::string, wxMenu*>> main_menus()
{
    std::vector<std::pair<std::string, wxMenu*>> out;
    MainFrame* frame = wxGetApp().mainframe;
    if (frame == nullptr)
        return out;
    if (wxMenuBar* bar = frame->GetMenuBar(); bar != nullptr) {
        for (size_t i = 0; i < bar->GetMenuCount(); ++i)
            out.emplace_back(into_u8(wxStripMenuCodes(bar->GetMenuLabel(i))), bar->GetMenu(i));
        return out;
    }
    if (BBLTopbar* topbar = frame->topbar()) {
        if (wxMenu* file = topbar->GetFileMenu())
            out.emplace_back(into_u8(_L("File")), file);
        if (wxMenu* top = topbar->GetTopMenu())
            for (wxMenuItem* item : top->GetMenuItems())
                if (item->IsSubMenu())
                    out.emplace_back(into_u8(item->GetItemLabelText()), item->GetSubMenu());
    }
    return out;
}

// Finds a menu item by id under the main menus, with its label path and owning menu.
wxMenuItem* find_menu_item(int id, std::string* path, wxMenu** owner)
{
    std::function<wxMenuItem*(wxMenu*, const std::string&)> search = [&](wxMenu* menu, const std::string& prefix) -> wxMenuItem* {
        for (wxMenuItem* item : menu->GetMenuItems()) {
            if (item->IsSeparator())
                continue;
            const std::string here = prefix + ">" + into_u8(item->GetItemLabelText());
            if (item->GetId() == id && !item->IsSubMenu()) {
                if (path)
                    *path = here;
                if (owner)
                    *owner = menu;
                return item;
            }
            if (item->IsSubMenu())
                if (wxMenuItem* found = search(item->GetSubMenu(), here))
                    return found;
        }
        return nullptr;
    };
    for (const auto& [label, menu] : main_menus())
        if (wxMenuItem* found = search(menu, label))
            return found;
    return nullptr;
}

wxMenuItem* find_menu_path(const std::string& wanted, int& id)
{
    std::function<wxMenuItem*(wxMenu*, const std::string&)> search = [&](wxMenu* menu, const std::string& prefix) -> wxMenuItem* {
        for (wxMenuItem* item : menu->GetMenuItems()) {
            if (item->IsSeparator())
                continue;
            const std::string here = prefix + ">" + into_u8(item->GetItemLabelText());
            if (!item->IsSubMenu() && lower(here) == lower(wanted)) {
                id = item->GetId();
                return item;
            }
            if (item->IsSubMenu())
                if (wxMenuItem* found = search(item->GetSubMenu(), here))
                    return found;
        }
        return nullptr;
    };
    for (const auto& [label, menu] : main_menus())
        if (wxMenuItem* found = search(menu, label))
            return found;
    return nullptr;
}

// What a menu appends to the label of the item in use, which menu paths may leave out.
std::string current_marker() { return " (" + _u8L("current") + ")"; }

// The items of a popup menu, a submenu before its items, with the paths automation names them by.
std::vector<MenuEntry> menu_entries(wxMenu& menu)
{
    std::vector<MenuEntry>                        entries;
    std::function<void(wxMenu&, int, bool)> walk = [&](wxMenu& m, int parent, bool enabled) {
        for (wxMenuItem* item : m.GetMenuItems()) {
            if (item->IsSeparator())
                continue;
            MenuEntry e;
            e.label     = into_u8(item->GetItemLabelText());
            e.parent    = parent;
            e.id        = item->GetId();
            e.enabled   = enabled && item->IsEnabled();
            e.checkable = item->IsCheckable();
            e.checked   = e.checkable && item->IsChecked();
            e.submenu   = item->IsSubMenu();
            entries.push_back(e);
            if (e.submenu)
                walk(*item->GetSubMenu(), int(entries.size()) - 1, e.enabled);
        }
    };
    walk(menu, -1, true);
    name_menu_entries(entries, current_marker());
    return entries;
}

// Native controls act on what the system tells them, not on wx mouse events sent to them, so a
// click at a point of one does nothing.
bool is_native_control(wxWindow* w)
{
    return dynamic_cast<wxAnyButton*>(w) != nullptr || dynamic_cast<wxCheckBox*>(w) != nullptr || dynamic_cast<wxChoice*>(w) != nullptr ||
           dynamic_cast<wxComboBox*>(w) != nullptr || dynamic_cast<wxTextCtrl*>(w) != nullptr;
}

} // namespace

// Stable English gizmo names for refs and locators: the names Orca uses for the gizmos it names,
// and the enum's own for the rest.
std::string gizmo_name(int type)
{
    if (std::string name = get_name_from_gizmo_etype(GLGizmosManager::EType(type)); !name.empty())
        return name;
    switch (type) {
    case GLGizmosManager::Svg: return "SVG";
    case GLGizmosManager::Measure: return "Measure";
    case GLGizmosManager::Assembly: return "Assembly";
    case GLGizmosManager::Simplify: return "Simplify";
    case GLGizmosManager::BrimEars: return "Brim Ears";
    default: return "Gizmo " + std::to_string(type);
    }
}

namespace {

GLGizmosManager::EType gizmo_from_name(const std::string& name)
{
    for (int t = 0; t < int(GLGizmosManager::Undefined); ++t)
        if (lower(gizmo_name(GLGizmosManager::EType(t))) == lower(name))
            return GLGizmosManager::EType(t);
    return GLGizmosManager::Undefined;
}

// The mouse tools' modifiers, named as in ui_key's chords so both mean the same keys: ctrl (or cmd) is
// the shortcut modifier, Command on macOS, and rawctrl is the macOS Control key.
int parse_modifiers(const std::vector<std::string>& mods)
{
    int modifiers = wxMOD_NONE;
    for (const std::string& m : mods) {
        const int modifier = KeyChord::parse_modifier(m);
        if (modifier == wxMOD_NONE)
            throw ToolFailure("Unknown modifier " + m + " (ctrl, cmd, rawctrl, shift, alt)");
        modifiers |= modifier;
    }
    return modifiers;
}

void set_modifiers(wxKeyboardState& state, int modifiers)
{
    state.SetControlDown((modifiers & wxMOD_CONTROL) != 0);
    state.SetShiftDown((modifiers & wxMOD_SHIFT) != 0);
    state.SetAltDown((modifiers & wxMOD_ALT) != 0);
#ifdef __APPLE__
    // Elsewhere raw control is Ctrl itself, so setting it would undo SetControlDown.
    state.SetRawControlDown((modifiers & wxMOD_RAW_CONTROL) != 0);
#endif
}

std::vector<int> modifier_keys(int modifiers)
{
    std::vector<int> keys;
    if (modifiers & wxMOD_CONTROL)
        keys.push_back(WXK_CONTROL); // Command on macOS
#ifdef __APPLE__
    if (modifiers & wxMOD_RAW_CONTROL)
        keys.push_back(WXK_RAW_CONTROL);
#endif
    if (modifiers & wxMOD_SHIFT)
        keys.push_back(WXK_SHIFT);
    if (modifiers & wxMOD_ALT)
        keys.push_back(WXK_ALT);
    return keys;
}

bool send(wxWindow* target, wxEvent& evt)
{
    evt.SetEventObject(target);
    return target->GetEventHandler()->ProcessEvent(evt);
}

void command(wxWindow* w, wxEventType type, int int_value = 0, const wxString& string_value = {})
{
    wxCommandEvent evt(type, w->GetId());
    evt.SetInt(int_value);
    evt.SetString(string_value);
    send(w, evt);
}

wxMouseEvent mouse_event(wxEventType type, const wxPoint& pos, bool left, bool right, bool middle, int modifiers)
{
    wxMouseEvent evt(type);
    evt.SetPosition(pos);
    evt.SetLeftDown(left);
    evt.SetRightDown(right);
    evt.SetMiddleDown(middle);
    set_modifiers(evt, modifiers);
    return evt;
}

// Scrolls every scrolled ancestor so `w` is in view, as a person would before using it.
void scroll_into_view(wxWindow* w)
{
    for (wxWindow* p = w->GetParent(); p != nullptr && !p->IsTopLevel(); p = p->GetParent()) {
        auto* scroller = dynamic_cast<wxScrollHelperBase*>(p);
        if (scroller == nullptr)
            continue;
        const wxPoint pos    = w->GetScreenPosition() - p->GetScreenPosition();
        const wxSize  client = p->GetClientSize();
        if (pos.y >= 0 && pos.y + w->GetSize().y <= client.y)
            continue;
        int ppu_x = 0, ppu_y = 0, view_x = 0, view_y = 0;
        scroller->GetScrollPixelsPerUnit(&ppu_x, &ppu_y);
        scroller->GetViewStart(&view_x, &view_y);
        if (ppu_y <= 0)
            continue;
        const int units = std::max(0, (view_y * ppu_y + pos.y - client.y / 3) / ppu_y);
#ifdef __WXGTK__
        // Moving the scrollbar, as the mouse wheel does: after wxScrollHelper::Scroll() GTK leaves
        // the panel's native labels unpainted.
        if (GtkWidget* widget = static_cast<GtkWidget*>(p->GetHandle()); widget != nullptr && GTK_IS_SCROLLED_WINDOW(widget)) {
            gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(widget)), units);
            continue;
        }
#endif
        scroller->Scroll(-1, units);
        p->Update();
    }
}

// Keys held in SyntheticInput for one gesture, for code that asks the system which keys are down
// (the object list's Ctrl/Shift multi-select); released however the gesture ends.
struct HeldKeys
{
    explicit HeldKeys(const std::vector<int>& keys)
    {
        for (int k : keys)
            SyntheticInput::press_key(k);
    }
    ~HeldKeys() { SyntheticInput::clear(); }
    HeldKeys(const HeldKeys&)            = delete;
    HeldKeys& operator=(const HeldKeys&) = delete;
};

// A mouse click on a plain window, at its centre or at the point the options give: down, up, and the
// double-click or context-menu event a real click produces.
void click_window(wxWindow* w, const ClickOptions& o)
{
    const int      mods = parse_modifiers(o.modifiers);
    HeldKeys       held(modifier_keys(mods));
    const wxSize   size = w->GetClientSize();
    const wxPoint  pos  = o.at_point ? wxPoint(o.x, o.y) : wxPoint(size.x / 2, size.y / 2);
    const bool     right = o.button == "right", middle = o.button == "middle", left = !right && !middle;
    const auto     down  = left ? wxEVT_LEFT_DOWN : right ? wxEVT_RIGHT_DOWN : wxEVT_MIDDLE_DOWN;
    const auto     up    = left ? wxEVT_LEFT_UP : right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP;
    wxMouseEvent   d     = mouse_event(down, pos, left, right, middle, mods);
    send(w, d);
    wxMouseEvent u = mouse_event(up, pos, false, false, false, mods);
    send(w, u);
    if (o.double_click && left) {
        wxMouseEvent dc = mouse_event(wxEVT_LEFT_DCLICK, pos, true, false, false, mods);
        send(w, dc);
        wxMouseEvent u2 = mouse_event(wxEVT_LEFT_UP, pos, false, false, false, mods);
        send(w, u2);
    }
#ifndef __WXOSX__
    // On macOS wx makes the context menu event itself from a right press its handlers pass on, as for a
    // real click; elsewhere it comes from the system.
    if (right) {
        wxContextMenuEvent menu(wxEVT_CONTEXT_MENU, w->GetId(), w->ClientToScreen(pos));
        send(w, menu);
    }
#endif
}

// Delivers synthesized mouse events to a 3D canvas, drawing a frame after each so ImGui sees the
// hover, the press and the release on separate frames, as with a real mouse. Its modifiers are
// pressed as a keyboard does, with a key-down event each before the gesture and a key-up after it,
// so gizmos that switch modes on the key (Measure's Shift point picking) see them.
class CanvasMouse
{
public:
    // `keys_down`: the modifiers' keys are already down, held by an earlier gesture.
    CanvasMouse(GLCanvas3D& canvas, int mods, bool keys_down = false) : m_canvas(canvas), m_mods(mods)
    {
        if (keys_down)
            return;
        int down = wxMOD_NONE;
        for (int k : modifier_keys(m_mods)) {
            SyntheticInput::press_key(k);
            down |= modifier_of(k);
            key_event(wxEVT_KEY_DOWN, k, down);
        }
    }
    ~CanvasMouse()
    {
        if (!m_keep_held) {
            int down = m_mods;
            for (int k : modifier_keys(m_mods)) {
                SyntheticInput::release_key(k);
                down &= ~modifier_of(k);
                key_event(wxEVT_KEY_UP, k, down);
            }
            SyntheticInput::clear();
        }
        m_canvas.set_as_dirty();
    }
    // A press that a later call releases: the button and the modifier keys stay down for code that
    // asks for their state, until UiInspector::release_held() ends the gesture.
    void keep_held() { m_keep_held = true; }
    void event(wxEventType type, const wxPoint& pos, bool left, bool right, bool middle, int wheel = 0)
    {
        wxGLCanvas*  glc = m_canvas.get_wxglcanvas();
        wxMouseEvent evt = mouse_event(type, pos, left, right, middle, m_mods);
        if (type == wxEVT_MOUSEWHEEL) {
            evt.m_wheelRotation = wheel;
            evt.m_wheelDelta    = 120;
            evt.m_wheelAxis     = wxMOUSE_WHEEL_VERTICAL;
        }
        wxMouseState state;
        state.SetPosition(glc->ClientToScreen(pos));
        state.SetLeftDown(left);
        state.SetRightDown(right);
        state.SetMiddleDown(middle);
        SyntheticInput::set_mouse(state);
        send(glc, evt);
        m_canvas.render();
    }

private:
    static int modifier_of(int key)
    {
        switch (key) {
        case WXK_CONTROL: return wxMOD_CONTROL;
#ifdef __APPLE__
        case WXK_RAW_CONTROL: return wxMOD_RAW_CONTROL;
#endif
        case WXK_SHIFT: return wxMOD_SHIFT;
        case WXK_ALT: return wxMOD_ALT;
        default: return wxMOD_NONE;
        }
    }
    // A modifier key's own event, carrying the modifiers down once it has happened.
    void key_event(wxEventType type, int key, int down)
    {
        wxKeyEvent evt(type);
        evt.m_keyCode = key;
        set_modifiers(evt, down);
        send(m_canvas.get_wxglcanvas(), evt);
        m_canvas.render();
    }

    GLCanvas3D& m_canvas;
    int         m_mods;
    bool        m_keep_held = false;
};

// The pointer arriving with no modifier held, as a person moves onto a spot before holding Shift to
// click it: gizmos that prepare on hover (Measure's point picking) see the spot first.
void hover(GLCanvas3D& canvas, const wxPoint& pos)
{
    CanvasMouse m(canvas, wxMOD_NONE);
    m.event(wxEVT_MOTION, pos, false, false, false);
}

void canvas_click(GLCanvas3D& canvas, const wxPoint& pos, const ClickOptions& o)
{
    const int mods = parse_modifiers(o.modifiers);
    if (mods != wxMOD_NONE)
        hover(canvas, pos);
    CanvasMouse m(canvas, mods);
    const bool  right = o.button == "right", middle = o.button == "middle", left = !right && !middle;
    m.event(wxEVT_MOTION, pos, false, false, false);
    m.event(left ? wxEVT_LEFT_DOWN : right ? wxEVT_RIGHT_DOWN : wxEVT_MIDDLE_DOWN, pos, left, right, middle);
    m.event(left ? wxEVT_LEFT_UP : right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP, pos, false, false, false);
    if (o.double_click && left) {
        m.event(wxEVT_LEFT_DCLICK, pos, true, false, false);
        m.event(wxEVT_LEFT_UP, pos, false, false, false);
    }
}

// Canvas client coordinates (logical) of a point in ImGui coordinates.
wxPoint imgui_to_canvas(GLCanvas3D& canvas, float x, float y)
{
    const float scale = canvas.get_scale();
    return {int(std::lround(x / scale)), int(std::lround(y / scale))};
}

// Kinds of ImGui items that hold a value a person types or picks.
bool is_imgui_field(const std::string& kind)
{
    return kind == "slider" || kind == "drag" || kind == "input" || kind == "multiline" || kind == "combo";
}

// What a listed ImGui item is: its widget kind, else a checkbox or a button.
std::string imgui_kind(const ImGuiWrapper::RecordedItem& item)
{
    if (!item.kind.empty())
        return item.kind;
    return (item.flags & ImGuiItemStatusFlags_Checkable) != 0 ? "checkbox" : "button";
}

// What an item shows as its value: a field's recorded value, or for a combo drawn as a button (the
// Text gizmo's font list) the button's own label.
std::string imgui_value(const ImGuiWrapper::RecordedItem& item)
{
    return item.value.empty() && item.kind == "combo" ? clean_imgui_label(item.label) : item.value;
}

// Whether an item is in the popup of a combo or a popup-list (ImGui names those windows
// "##Combo_NN" and "##Popup_XXXXXXXX", and their children after them).
bool in_popup(const ImGuiWrapper::RecordedItem& item)
{
    return item.window.rfind("##Combo", 0) == 0 || item.window.rfind("##Popup", 0) == 0;
}

// What a click at logical pixel `pixel` of `canvas` would reach instead of its target, described for
// a reason: the ImGui window over the pixel, unless it is `own` (the window of the ImGui item the
// click is for) or one of `own`'s children; for a click on the scene (no `own`), also an open popup,
// which takes any click to close itself. Empty when the click reaches its target. Judged on a frame
// drawn now, whose windows ImGui hands the click's events to.
std::string covered_by(GLCanvas3D& canvas, const wxPoint& pixel, const std::string& own = {})
{
    ImGuiWrapper* imgui = wxGetApp().imgui();
    if (imgui == nullptr || ImGui::GetCurrentContext() == nullptr)
        return {};
    canvas.set_as_dirty();
    canvas.render();
    const float  scale = canvas.get_scale();
    ImGuiWindow* hit   = imgui_window_at(ImVec2(float(pixel.x) * scale, float(pixel.y) * scale));
    ImGuiWindow* mine  = own.empty() ? nullptr : ImGui::FindWindowByName(own.c_str());
    if (hit != nullptr && mine != nullptr && hit->RootWindow == mine->RootWindow)
        return {};
    const ImGuiContext& g     = *ImGui::GetCurrentContext();
    ImGuiWindow*        cover = hit != nullptr ? hit : own.empty() && g.OpenPopupStack.Size > 0 ? g.OpenPopupStack.back().Window : nullptr;
    if (cover == nullptr)
        return {};
    return imgui_window_description(cover->RootWindow->Name, imgui->recorded_texts(&canvas));
}

// The logical pixel of `canvas` showing a point of the model: {object, instance, volume} and either
// `point` (with `frame` "volume", the part's mesh coordinates, or "world", the scene's) or `spot`.
// With `check` "surface" a click there must land on that part at that point (see check_surface()).
wxPoint target_pixel(GLCanvas3D& canvas, const json& target, const std::string& check, double tolerance)
{
    if (!target.is_object())
        throw ToolFailure("A model target is {object, instance, volume, point | spot}");
    const int oi = target.value("object", -1), ii = target.value("instance", 0), vi = target.value("volume", 0);
    const std::string what = "object " + std::to_string(oi) + " instance " + std::to_string(ii) + " part " + std::to_string(vi);
    // The instance the open gizmo works on draws its parts itself, even while their scene entries are
    // inactive (the painters deactivate them).
    GLGizmosManager& gizmos    = canvas.get_gizmos_manager();
    const Selection& selection = canvas.get_selection();
    const bool       gizmo     = gizmos.get_current_type() != GLGizmosManager::Undefined;
    auto             own       = [&](const GLVolume* v) {
        return gizmo && v->object_idx() == selection.get_object_idx() && v->instance_idx() == selection.get_instance_idx();
    };
    const GLVolume* shown = nullptr;
    for (const GLVolume* v : canvas.get_volumes().volumes)
        if (v->object_idx() == oi && v->instance_idx() == ii && v->volume_idx() == vi && !v->is_wipe_tower)
            shown = v;
    // The assemble view shows only the objects selected when it opened, and clears `visible` on the rest.
    if (shown == nullptr || shown->mesh_raycaster == nullptr || !shown->visible || (!shown->is_active && !own(shown)))
        throw ToolFailure(what + " is not shown in the " + std::string(canvas_name(&canvas)) +
                          " view (list_objects lists objects and parts; the assemble view shows the objects selected when it opened)");
    const Transform3d world = shown->world_matrix();
    const AABBMesh&   mesh  = shown->mesh_raycaster->get_aabb_mesh();

    Vec3d point;
    if (target.contains("spot")) {
        const std::string spot = target["spot"].get<std::string>();
        const auto        hit  = spot_point(mesh, world, spot);
        if (!hit)
            throw ToolFailure("No " + spot + " spot on " + what + " (spot: top, bottom, front, back, left, right)");
        point = *hit;
    } else if (target.contains("point")) {
        const json& p = target["point"];
        if (!p.is_array() || p.size() != 3)
            throw ToolFailure("point is [x, y, z]");
        point = Vec3d(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
        const std::string frame = target.value("frame", "volume");
        if (frame == "volume")
            point = world * point;
        else if (frame != "world")
            throw ToolFailure("frame is volume or world");
    } else {
        throw ToolFailure("A model target needs point or spot");
    }

    CanvasView view;
    const Camera& camera = wxGetApp().plater()->get_camera();
    view.view            = camera.get_view_matrix();
    view.projection      = camera.get_projection_matrix();
    view.viewport        = camera.get_viewport();
    view.scale           = canvas.get_scale();
    const auto projected = project_point(view, point);
    if (!projected)
        throw ToolFailure("The point of " + what + " is off screen; turn or zoom the camera with set_view");
    // Events carry whole logical pixels: the click goes through this one, not the projected point.
    const wxPoint pixel(int(std::lround(projected->x())), int(std::lround(projected->y())));

    if (check == "surface") {
        // What a person sees (surface_meshes()): other inactive parts are hidden, other modifiers and
        // negative parts are see-through, and gizmo geometry takes clicks as it would from a person.
        // The manager hands the plane out flipped, for drawing; the gizmos' own raycasts use it as
        // the object clipper holds it.
        std::optional<ClippingPlane> clip;
        if (gizmo)
            if (const auto drawn = gizmos.get_clipping_plane())
                clip = drawn->inverted_normal();
        const std::vector<SurfaceMesh> meshes = surface_meshes(canvas.get_volumes().volumes, shown, own);
        BoundingBoxf3                  target_box;
        for (const Vec3f& vertex : mesh.vertices())
            target_box.merge(world * vertex.cast<double>());
        std::vector<Polygon> beds;
        PartPlateList&       plates = wxGetApp().plater()->get_partplate_list();
        for (int i = 0; i < plates.get_plate_count(); ++i)
            if (PartPlate* plate = plates.get_plate(i))
                beds.push_back(Polygon::new_scale(plate->get_shape()));
        const double      limit = tolerance > 0. ? tolerance : std::max(0.1, 0.01 * target_box.size().norm());
        const std::string why   = check_surface(point, pixel_ray(view, Vec2d(pixel.x, pixel.y)), meshes, clip ? &*clip : nullptr, beds, limit);
        if (!why.empty())
            throw ToolFailure("A click at the point of " + what + " would miss it: " + why +
                              ". Turn the camera with set_view, or pass check:\"point\" for a point off the surface");
    } else if (check != "point") {
        throw ToolFailure("check is surface or point");
    }
    return pixel;
}

// Whether the selection is one SVG part, the only selection the SVG gizmo opens on.
bool selection_is_svg_part(GLCanvas3D& canvas)
{
    const GLVolume* gl_volume = get_selected_gl_volume(canvas.get_selection());
    const Model*    model     = canvas.get_selection().get_model();
    if (gl_volume == nullptr || model == nullptr)
        return false;
    const ModelVolume* volume = get_model_volume(*gl_volume, model->objects);
    return volume != nullptr && GLGizmoSVG::is_svg(*volume);
}

} // namespace

const char* canvas_name(GLCanvas3D* canvas)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || canvas == nullptr)
        return "";
    if (canvas == plater->get_view3D_canvas3D())
        return "3d";
    if (canvas == plater->get_preview_canvas3D())
        return "preview";
    if (canvas == plater->get_assmeble_canvas3D())
        return "assemble";
    if (canvas == design_canvas())
        return "design";
    return "";
}

GLCanvas3D* canvas_by_name(const std::string& name)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return nullptr;
    if (name == "preview")
        return plater->get_preview_canvas3D();
    if (name == "assemble")
        return plater->get_assmeble_canvas3D();
    if (name == "design")
        return design_canvas();
    if (name == "3d" || name.empty())
        return plater->get_view3D_canvas3D();
    return nullptr;
}

namespace {

bool is_live_canvas(GLCanvas3D* canvas)
{
    const auto canvases = ui_canvases();
    return canvas != nullptr && std::find(canvases.begin(), canvases.end(), canvas) != canvases.end();
}

static_assert(kPlateIconCount == int(PartPlate::GRABBER_COUNT), "plate icon names follow PartPlate's picking actions");

// The plates and plate icons the Prepare view can pick now, by bed picking id. PartPlate registers
// the icons it shows (settings where plate settings are offered, filament_map on two-nozzle Bambu
// printers, imex_mode on IDEX printers), and other plates are inactive while they are hidden, so the
// registry says what a click can reach.
std::set<int> plate_picking_ids(GLCanvas3D& canvas)
{
    std::set<int> ids;
    if (const auto* items = canvas.get_raycasters_for_picking(SceneRaycaster::EType::Bed))
        for (const std::shared_ptr<SceneRaycasterItem>& item : *items)
            if (item->is_active())
                ids.insert(SceneRaycaster::decode_id(SceneRaycaster::EType::Bed, item->get_id()));
    return ids;
}

// A plate as the object list names it: "Plate 2", with its own name after it.
std::string plate_label(PartPlate& plate)
{
    std::string label = _u8L("Plate") + " " + std::to_string(plate.get_index() + 1);
    if (!plate.get_plate_name().empty())
        label += " (" + plate.get_plate_name() + ")";
    return label;
}

std::string no_such_plate(int plate, int count)
{
    return "Plate " + std::to_string(plate) + " does not exist (plates 0.." + std::to_string(count - 1) + ")";
}

} // namespace

std::vector<GLCanvas3D*> live_canvases()
{
    std::vector<GLCanvas3D*> out;
    if (Plater* plater = wxGetApp().plater())
        for (GLCanvas3D* c : {plater->get_view3D_canvas3D(), plater->get_preview_canvas3D(), plater->get_assmeble_canvas3D()})
            if (c != nullptr)
                out.push_back(c);
    return out;
}

GLCanvas3D* design_canvas()
{
#ifdef SLIC3R_CAD
    if (DesignPanel* design = DesignPanel::if_built())
        if (DesignCanvas* view = design->mcp_viewport())
            return view->gl_canvas();
#endif
    return nullptr;
}

std::vector<GLCanvas3D*> ui_canvases()
{
    std::vector<GLCanvas3D*> out = live_canvases();
    if (GLCanvas3D* design = design_canvas())
        out.push_back(design);
    return out;
}

GLCanvas3D* shown_canvas()
{
#ifdef SLIC3R_CAD
    if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr && frame->shown_design_panel() != nullptr)
        return design_canvas();
#endif
    Plater* plater = wxGetApp().plater();
    return plater != nullptr ? plater->get_current_canvas3D() : nullptr;
}

// ---- popups and rows of lists -----------------------------------------------------------------

wxWindow* owning_window(wxWindow* w)
{
    wxWindow* top = w != nullptr ? wxGetTopLevelParent(w) : nullptr;
    while (dynamic_cast<wxPopupWindow*>(top) != nullptr && top->GetParent() != nullptr)
        top = wxGetTopLevelParent(top->GetParent());
    return top;
}

std::vector<wxWindow*> popups_of(wxWindow* top)
{
    std::vector<wxWindow*> out;
    std::function<void(wxWindow*)> visit = [&](wxWindow* w) {
        for (wxWindow* child : w->GetChildren()) {
            if (dynamic_cast<wxTopLevelWindow*>(child) != nullptr || !child->IsShown())
                continue;
            if (dynamic_cast<wxPopupWindow*>(child) != nullptr)
                out.push_back(child);
            visit(child);
        }
    };
    if (top != nullptr)
        visit(top);
    return out;
}

namespace {

std::string joined(const std::vector<std::string>& parts, const char* separator)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
        out += (i == 0 ? "" : separator) + parts[i];
    return out;
}

// The object list's columns by model column (ColumnNumber): it shows no header, so they are named here.
const char* const kObjectListColumns[] = {"Name", "Height", "Printable", "Filament", "Support", "Color", "Sinking", "Editing"};

std::vector<wxDataViewColumn*> shown_columns(wxDataViewCtrl& ctrl)
{
    std::vector<wxDataViewColumn*> out;
    for (unsigned i = 0; i < ctrl.GetColumnCount(); ++i)
        if (wxDataViewColumn* c = ctrl.GetColumn(i); c != nullptr && !c->IsHidden())
            out.push_back(c);
    return out;
}

std::string column_name(wxDataViewCtrl& ctrl, const wxDataViewColumn& col, std::size_t pos)
{
    if (dynamic_cast<ObjectList*>(&ctrl) != nullptr && col.GetModelColumn() < std::size(kObjectListColumns))
        return kObjectListColumns[col.GetModelColumn()];
    wxString title = col.GetTitle();
    const std::string name = into_u8(title.Trim().Trim(false));
    return name.empty() ? "#" + std::to_string(pos + 1) : name;
}

// The column `name` picks (see list_columns()), or the first shown one for none.
wxDataViewColumn* data_view_column(wxDataViewCtrl& ctrl, const std::string& name)
{
    const std::vector<wxDataViewColumn*> cols = shown_columns(ctrl);
    if (cols.empty())
        throw ToolFailure("The list shows no columns");
    if (name.empty())
        return cols.front();
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (lower(column_name(ctrl, *cols[i], i)) == lower(name))
            return cols[i];
    throw ToolFailure("The list has no column \"" + name + "\"", {{"columns", list_columns(&ctrl)}});
}

// A cell's text as it shows: outside macOS the compare presets and unsaved changes trees draw theirs from
// markup (<b>, <span color="...">), whose tags and entities are not shown. Other text is kept as it is.
std::string shown_text(const wxString& value)
{
    const std::string text = into_u8(value);
    if (text.empty() || text.front() != '<')
        return text;
    static constexpr std::pair<std::string_view, char> entities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<') {
            if ((i = text.find('>', i)) == std::string::npos)
                return text;
            continue;
        }
        char c = text[i];
        if (c == '&')
            for (const auto& [entity, plain] : entities)
                if (text.compare(i, entity.size(), entity) == 0) {
                    c = plain;
                    i += entity.size() - 1;
                    break;
                }
        out += c;
    }
    return out;
}

// The text a data view cell shows, or for a checkbox cell nothing, its state going to `checked` (the
// row's first). Icon cells show nothing.
std::optional<std::string> cell_text(wxDataViewModel& model, const wxDataViewItem& item, unsigned col, int& checked)
{
    wxVariant v;
    model.GetValue(v, item, col);
    const wxString type = v.GetType();
    if (type == "bool") {
        if (checked < 0)
            checked = v.GetBool() ? 1 : 0;
        return std::nullopt;
    }
    if (type == "string")
        return shown_text(v.GetString());
    if (type == "wxDataViewIconText") {
        wxDataViewIconText text;
        text << v;
        return shown_text(text.GetText());
    }
    if (type == "DataViewBitmapText") {
        DataViewBitmapText text;
        text << v;
        return shown_text(text.GetText());
    }
    if (type == "long" || type == "double")
        return into_u8(v.MakeString());
    return std::nullopt;
}

// What an object list row stands for, by the indices list_objects uses.
std::string object_row_note(ObjectDataViewModel& model, const wxDataViewItem& item)
{
    int            plate  = -1;
    const ItemType type   = model.GetItemType(item, plate);
    const std::string object = "object " + std::to_string(model.GetObjectIdByItem(item));
    switch (type) {
    case itPlate: return plate >= 0 ? "plate " + std::to_string(plate) : "outside the plates";
    case itObject: return object;
    case itVolume: {
        const std::string part = object + " part " + std::to_string(model.GetVolumeIdByItem(item));
        switch (model.GetVolumeType(item)) {
        case ModelVolumeType::NEGATIVE_VOLUME: return part + ", negative";
        case ModelVolumeType::PARAMETER_MODIFIER: return part + ", modifier";
        case ModelVolumeType::SUPPORT_BLOCKER: return part + ", support_blocker";
        case ModelVolumeType::SUPPORT_ENFORCER: return part + ", support_enforcer";
        default: return part;
        }
    }
    case itInstanceRoot: return "instances of " + object;
    case itInstance: return object + " instance " + std::to_string(model.GetInstanceIdByItem(item));
    case itSettings: return "settings of " + object_row_note(model, model.GetParent(item));
    case itLayerRoot: return "layer ranges of " + object;
    case itLayer: {
        const auto range = model.GetLayerRangeByItem(item);
        char       text[64];
        std::snprintf(text, sizeof(text), " layer range %g-%g", range.first, range.second);
        return object + text;
    }
    case itInfo: return "info of " + object;
    default: return {};
    }
}

void data_view_rows(wxDataViewCtrl& ctrl, std::vector<ListRow>& out)
{
    wxDataViewModel* model = ctrl.GetModel();
    if (model == nullptr)
        return;
    const std::vector<wxDataViewColumn*> cols    = shown_columns(ctrl);
    auto*                                objects = dynamic_cast<ObjectDataViewModel*>(model);
    // Collapsed branches too: acting on a row expands the branches above it.
    std::function<void(const wxDataViewItem&, const std::vector<std::string>&, int, bool)> add =
        [&](const wxDataViewItem& parent, const std::vector<std::string>& above, int depth, bool shown) {
            wxDataViewItemArray children;
            model->GetChildren(parent, children);
            for (const wxDataViewItem& item : children) {
                ListRow row;
                row.path = above;
                bool labelled = false;
                for (wxDataViewColumn* col : cols) {
                    const std::optional<std::string> text = cell_text(*model, item, col->GetModelColumn(), row.checked);
                    if (!text)
                        continue;
                    if (!labelled)
                        row.path.push_back(*text);
                    else if (!text->empty())
                        row.value += (row.value.empty() ? "" : " | ") + *text;
                    labelled = true;
                }
                if (!labelled)
                    row.path.emplace_back();
                if (objects != nullptr)
                    row.note = object_row_note(*objects, item);
                row.selected = ctrl.IsSelected(item);
                row.shown    = shown;
                row.depth    = depth;
                row.item     = item;
                const std::vector<std::string> path = row.path;
                out.push_back(std::move(row));
                if (model->IsContainer(item))
                    add(item, path, depth + 1, shown && ctrl.IsExpanded(item));
            }
        };
    add(wxDataViewItem(nullptr), {}, 0, true);
}

void list_box_rows(wxListBox& box, std::vector<ListRow>& out)
{
    auto* checks = dynamic_cast<wxCheckListBox*>(&box);
    for (unsigned i = 0; i < box.GetCount(); ++i) {
        ListRow row;
        row.path     = {into_u8(box.GetString(i))};
        row.checked  = checks == nullptr ? -1 : checks->IsChecked(i) ? 1 : 0;
        row.selected = box.IsSelected(int(i));
        row.index    = int(i);
        out.push_back(std::move(row));
    }
}

// A drop-down list's items; one of a group, which shows in a sublist, has the group's label above it.
// The picked item is the checked one.
void drop_down_rows(DropDown& drop, std::vector<ListRow>& out)
{
    if (drop.IsSubList())
        return; // its main list has the same items
    const std::vector<DropDown::Item>& items = drop.GetItems();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const DropDown::Item& item = items[i];
        if ((item.style & DD_ITEM_STYLE_SPLIT_ITEM) != 0)
            continue;
        ListRow row;
        if (!item.group_key.IsEmpty()) {
            row.path.push_back(into_u8(item.group_label.IsEmpty() ? item.group_key : item.group_label));
            row.note = "in " + row.path.back();
        }
        row.path.push_back(into_u8(item.text));
        row.enabled = (item.style & DD_ITEM_STYLE_DISABLED) == 0;
        row.checked = drop.GetSelection() == int(i) ? 1 : -1;
        row.index   = int(i);
        out.push_back(std::move(row));
    }
}

// A table's column titles as its header shows them, without a sort indicator ("Name ↑↓" in the object
// settings table); empty for a hidden or untitled column. A table that hides its column labels draws
// its titles as its first row, so they are read from there.
std::vector<std::string> grid_titles(wxGrid& grid)
{
    const bool               header_row = grid.GetColLabelSize() == 0;
    std::vector<std::string> out(std::size_t(std::max(grid.GetNumberCols(), 0)));
    for (int c = 0; c < int(out.size()); ++c) {
        if (!grid.IsColShown(c) || (header_row && grid.GetNumberRows() == 0))
            continue;
        wxString title = header_row ? grid.GetCellValue(0, c) : grid.GetColLabelValue(c);
        title.Trim();
        while (title.EndsWith(wxString::FromUTF8("\u2191")) || title.EndsWith(wxString::FromUTF8("\u2193")))
            title.RemoveLast().Trim();
        out[std::size_t(c)] = into_u8(title);
    }
    return out;
}

// A cell's text without the indent a table may draw parts with; a checkbox cell's state.
std::string grid_cell(wxGrid& grid, int row, int col)
{
    wxGridTableBase* table = grid.GetTable();
    if (table->GetTypeName(row, col) == wxGRID_VALUE_BOOL && table->CanGetValueAs(row, col, wxGRID_VALUE_BOOL))
        return table->GetValueAsBool(row, col) ? "yes" : "no";
    wxString text = grid.GetCellValue(row, col);
    return into_u8(text.Trim().Trim(false));
}

// The column a table row is named and clicked by: its "Name" column, else its first titled one.
int grid_name_column(const std::vector<std::string>& titles)
{
    const auto name = std::find(titles.begin(), titles.end(), "Name");
    if (name != titles.end())
        return int(name - titles.begin());
    const auto titled = std::find_if(titles.begin(), titles.end(), [](const std::string& t) { return !t.empty(); });
    return titled == titles.end() ? -1 : int(titled - titles.begin());
}

// A table's rows, read-only: named by their "Name" cell, else by their row label where the table
// shows one, else by their first titled cell, with every titled cell as the value.
void grid_rows(wxGrid& grid, std::vector<ListRow>& out)
{
    if (grid.GetTable() == nullptr)
        return;
    const std::vector<std::string> titles = grid_titles(grid);
    const bool                     named  = std::find(titles.begin(), titles.end(), "Name") != titles.end();
    const int                      name   = grid_name_column(titles);
    for (int r = grid.GetColLabelSize() == 0 ? 1 : 0; r < grid.GetNumberRows(); ++r) {
        if (!grid.IsRowShown(r))
            continue;
        ListRow                  row;
        std::vector<std::string> cells;
        for (int c = 0; c < int(titles.size()); ++c)
            if (!titles[c].empty())
                cells.push_back(grid_cell(grid, r, c));
        row.path     = {!named && grid.GetRowLabelSize() > 0 ? into_u8(grid.GetRowLabelValue(r)) : name >= 0 ? grid_cell(grid, r, name) : std::string()};
        row.value    = joined(cells, " | ");
        row.selected = grid.GetGridCursorRow() == r;
        row.index    = r;
        out.push_back(std::move(row));
    }
}

#ifdef SLIC3R_CAD
// The Design tab's feature tree and body lists: each row with the tips of its action icons, which
// `column` names.
void design_rows(DesignRowList& list, std::vector<ListRow>& out)
{
    const std::vector<DesignRowList::Row>& rows = list.rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ListRow                  row;
        std::vector<std::string> tips;
        for (const DesignRowList::Action& action : rows[i].actions)
            if (!action.icon.empty())
                tips.push_back(into_u8(action.tip));
        row.path     = {into_u8(rows[i].label)};
        row.note     = tips.empty() ? std::string() : "actions: " + joined(tips, " | ");
        row.selected = list.selection() == int(i);
        row.index    = int(i);
        out.push_back(std::move(row));
    }
}
#endif

std::optional<ListRow> find_row(wxWindow& list, const std::vector<std::string>& path, int twin)
{
    for (ListRow& row : list_rows(&list))
        if (row.path == path && row.twin == twin)
            return std::move(row);
    return std::nullopt;
}

json selected_rows(wxWindow& list)
{
    json out = json::array();
    for (const ListRow& row : list_rows(&list))
        if (row.selected)
            out.push_back(joined(row.path, ">"));
    return out;
}

void swallow_selection(wxDataViewEvent&) {}

// Sets a data view's selection as a click does, and tells its owner once, for the clicked row. The
// controls report a selection they are given differently (macOS each change it makes on the way, GTK
// and the generic control none), and a click reports it once.
void select_rows(wxDataViewCtrl& ctrl, const wxDataViewItemArray& selection, const wxDataViewItem& clicked)
{
    ctrl.Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, &swallow_selection);
    ctrl.SetSelections(selection);
    if (ctrl.IsSelected(clicked))
        ctrl.SetCurrentItem(clicked);
    ctrl.Unbind(wxEVT_DATAVIEW_SELECTION_CHANGED, &swallow_selection);
    wxDataViewEvent changed(wxEVT_DATAVIEW_SELECTION_CHANGED, &ctrl, clicked);
    send(&ctrl, changed);
}

json click_data_view_row(wxDataViewCtrl& ctrl, const ListRow& row, const std::string& column, const ClickOptions& o)
{
    const bool        right = o.button == "right";
    wxDataViewColumn* col   = data_view_column(ctrl, column);
    const int         mods  = parse_modifiers(o.modifiers);
    HeldKeys          held(modifier_keys(mods));
    ctrl.EnsureVisible(row.item, col); // expands the branches above it and scrolls to it
    scroll_into_view(&ctrl);
    const wxRect cell = ctrl.GetItemRect(row.item, col);
    if (cell.IsEmpty())
        throw ToolFailure("\"" + row.path.back() + "\" can't be scrolled into the list's view; make the window taller with set_view window_size");
    // The pointer stays on the cell for the click: the object list acts on the cell under it (Printable
    // toggles printability, Editing resets the row's settings).
    wxMouseState pointer;
    pointer.SetPosition(ctrl.ClientToScreen(wxPoint(cell.x + cell.width / 2, cell.y + cell.height / 2)));
    SyntheticInput::set_mouse(pointer);

    // A right click selects an unselected row only; Ctrl toggles a row in the selection, Shift selects
    // the shown rows from the current one.
    if (!right || !ctrl.IsSelected(row.item)) {
        wxDataViewItemArray selection;
        if (right || !ctrl.HasFlag(wxDV_MULTIPLE) || (mods & (wxMOD_CONTROL | wxMOD_SHIFT)) == 0) {
            selection.Add(row.item);
        } else if ((mods & wxMOD_CONTROL) != 0) {
            ctrl.GetSelections(selection);
            if (const int at = selection.Index(row.item); at != wxNOT_FOUND)
                selection.RemoveAt(std::size_t(at));
            else
                selection.Add(row.item);
        } else {
            const std::vector<ListRow> rows   = list_rows(&ctrl);
            const wxDataViewItem       anchor = ctrl.GetCurrentItem().IsOk() ? ctrl.GetCurrentItem() : row.item;
            int                        from = -1, to = -1;
            for (int i = 0; i < int(rows.size()); ++i) {
                if (rows[i].item == anchor)
                    from = i;
                if (rows[i].item == row.item)
                    to = i;
            }
            if (from < 0)
                from = to;
            for (int i = std::min(from, to); i <= std::max(from, to); ++i)
                if (rows[std::size_t(i)].shown)
                    selection.Add(rows[std::size_t(i)].item);
        }
        select_rows(ctrl, selection, row.item);
    }
#ifdef __WXOSX__
    // The native list acts on a click in an editable icon cell by starting to edit it, which the object
    // list turns into the column's action; elsewhere the owner acts on the cell under the pointer when
    // the selection changes.
    if (!right && !o.double_click && !column.empty() && col->GetRenderer() != nullptr &&
        dynamic_cast<wxDataViewBitmapRenderer*>(col->GetRenderer()) != nullptr && col->GetRenderer()->GetMode() == wxDATAVIEW_CELL_EDITABLE) {
        wxDataViewEvent edit(wxEVT_DATAVIEW_ITEM_EDITING_STARTED, &ctrl, col, row.item);
        send(&ctrl, edit);
    }
#endif
    if (o.double_click && !right) {
        wxDataViewEvent activated(wxEVT_DATAVIEW_ITEM_ACTIVATED, &ctrl, col, row.item);
        send(&ctrl, activated);
    }
    if (right) {
        wxDataViewEvent menu(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, &ctrl, col, row.item);
        send(&ctrl, menu);
    }
    return {{"clicked", joined(row.path, ">")}, {"selected", selected_rows(ctrl)}};
}

json click_list_box_row(wxListBox& box, const ListRow& row, const ClickOptions& o)
{
    const bool     right = o.button == "right";
    const int      n     = row.index;
    const int      mods  = parse_modifiers(o.modifiers);
    const wxString text  = box.GetString(unsigned(n));
    if (!right || !box.IsSelected(n)) {
        if (box.HasMultipleSelection() && !right && (mods & (wxMOD_CONTROL | wxMOD_SHIFT)) != 0) {
            if (box.IsSelected(n))
                box.Deselect(n);
            else
                box.SetSelection(n);
        } else {
            if (box.HasMultipleSelection())
                box.DeselectAll();
            box.SetSelection(n);
        }
        command(&box, wxEVT_LISTBOX, n, text);
    }
    if (o.double_click && !right)
        command(&box, wxEVT_LISTBOX_DCLICK, n, text);
    if (right) {
        // As from the keyboard: the menu is for the selection.
        wxContextMenuEvent menu(wxEVT_CONTEXT_MENU, box.GetId(), wxDefaultPosition);
        send(&box, menu);
    }
    return {{"clicked", joined(row.path, ">")}, {"selected", selected_rows(box)}};
}

json click_grid_row(wxGrid& grid, const ListRow& row, const std::string& column, const ClickOptions& o)
{
    if (o.button != "left" || o.double_click)
        throw ToolFailure("A table row takes a single left click, which makes its cell the current one; the table is read-only here");
    const std::vector<std::string> titles = grid_titles(grid);
    int                            col    = grid_name_column(titles);
    if (!column.empty()) {
        col = -1;
        for (int c = 0; c < int(titles.size()); ++c)
            if (!titles[c].empty() && lower(titles[c]) == lower(column))
                col = c;
        if (col < 0)
            throw ToolFailure("The table has no column \"" + column + "\"", {{"columns", list_columns(&grid)}});
    }
    grid.GoToCell(row.index, std::max(col, 0)); // sends wxEVT_GRID_SELECT_CELL, as a click does
    return {{"clicked", joined(row.path, ">")}, {"column", col >= 0 ? titles[std::size_t(col)] : std::string()}};
}

#ifdef SLIC3R_CAD
// A click at the row's centre, or at its action icon whose tip `column` gives, through the list's own
// mouse handlers.
json click_design_row(DesignRowList& list, const ListRow& row, const std::string& column, const ClickOptions& o)
{
    const int n = row.index;
    if (!list.IsRowVisible(std::size_t(n)))
        list.ScrollToRow(std::size_t(n));
    wxRect cell = list.GetItemRect(std::size_t(n));
    if (!column.empty()) {
        const std::vector<DesignRowList::Action>& actions = list.rows()[std::size_t(n)].actions;
        std::size_t                               i       = 0;
        while (i < actions.size() && (actions[i].icon.empty() || lower(into_u8(actions[i].tip)) != lower(column)))
            ++i;
        if (i == actions.size())
            throw ToolFailure("\"" + row.path.back() + "\" has no action icon \"" + column + "\"", {{"actions", row.note}});
        cell = list.action_rect(n, i);
    }
    if (cell.IsEmpty())
        throw ToolFailure("\"" + row.path.back() + "\" can't be scrolled into the list's view");
    ClickOptions at = o;
    at.at_point     = true;
    at.x            = cell.x + cell.width / 2;
    at.y            = cell.y + cell.height / 2;
    click_window(&list, at);
    return {{"clicked", joined(row.path, ">")}};
}
#endif

} // namespace

std::vector<ListRow> list_rows(wxWindow* list)
{
    std::vector<ListRow> out;
    if (auto* view = dynamic_cast<wxDataViewCtrl*>(list))
        data_view_rows(*view, out);
    else if (auto* box = dynamic_cast<wxListBox*>(list))
        list_box_rows(*box, out);
    else if (auto* drop = dynamic_cast<DropDown*>(list))
        drop_down_rows(*drop, out);
    else if (auto* grid = dynamic_cast<wxGrid*>(list))
        grid_rows(*grid, out);
#ifdef SLIC3R_CAD
    else if (auto* design = dynamic_cast<DesignRowList*>(list))
        design_rows(*design, out);
#endif
    // Rows with the same labels are told apart by their order.
    std::map<std::vector<std::string>, int> seen;
    for (ListRow& row : out)
        row.twin = seen[row.path]++;
    return out;
}

bool is_row_list(wxWindow* w)
{
#ifdef SLIC3R_CAD
    if (dynamic_cast<DesignRowList*>(w) != nullptr)
        return true;
#endif
    return dynamic_cast<wxDataViewCtrl*>(w) != nullptr || dynamic_cast<wxListBox*>(w) != nullptr || dynamic_cast<DropDown*>(w) != nullptr ||
           dynamic_cast<wxGrid*>(w) != nullptr;
}

std::vector<std::string> list_columns(wxWindow* list)
{
    std::vector<std::string> out;
    if (auto* view = dynamic_cast<wxDataViewCtrl*>(list)) {
        const std::vector<wxDataViewColumn*> cols = shown_columns(*view);
        for (std::size_t i = 0; i < cols.size(); ++i)
            out.push_back(column_name(*view, *cols[i], i));
    } else if (auto* grid = dynamic_cast<wxGrid*>(list)) {
        for (const std::string& title : grid_titles(*grid))
            if (!title.empty())
                out.push_back(title);
    }
    return out;
}

std::vector<std::pair<wxWindow*, ListRow>> find_rows(const std::vector<wxWindow*>& roots, const std::vector<std::string>& wanted)
{
    std::vector<std::pair<wxWindow*, ListRow>> out;
    const std::string                          current = current_marker();
    std::function<void(wxWindow*)>             visit   = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return;
        if (is_row_list(w))
            for (ListRow& row : list_rows(w))
                if (wanted.empty() || row_matches(row.path, wanted, current))
                    out.emplace_back(w, std::move(row));
        for (wxWindow* child : w->GetChildren())
            if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr && dynamic_cast<wxPopupWindow*>(child) == nullptr)
                visit(child);
    };
    for (wxWindow* root : roots) {
        visit(root);
        for (wxWindow* popup : popups_of(root))
            visit(popup);
    }
    return out;
}

json click_row(wxWindow& list, const ListRow& row, const std::string& column, const ClickOptions& o)
{
    if (!list.IsShownOnScreen())
        throw ToolFailure("The list is no longer shown; take a new ui_snapshot");
    if (!list.IsEnabled())
        throw ToolFailure("The list is disabled");
    if (!row.enabled)
        throw ToolFailure("\"" + row.path.back() + "\" is disabled");
    if (o.at_point)
        throw ToolFailure("x/y don't go with a row; name its cell with column");
    if (o.button != "left" && o.button != "right")
        throw ToolFailure("A row takes a left or right click");
    if (auto* view = dynamic_cast<wxDataViewCtrl*>(&list))
        return click_data_view_row(*view, row, column, o);
    if (auto* grid = dynamic_cast<wxGrid*>(&list))
        return click_grid_row(*grid, row, column, o);
#ifdef SLIC3R_CAD
    if (auto* design = dynamic_cast<DesignRowList*>(&list))
        return click_design_row(*design, row, column, o);
#endif
    if (!column.empty())
        throw ToolFailure("This list has no columns; leave out column");
    if (auto* box = dynamic_cast<wxListBox*>(&list))
        return click_list_box_row(*box, row, o);
    if (auto* drop = dynamic_cast<DropDown*>(&list)) {
        if (o.button != "left" || o.double_click || !o.modifiers.empty())
            throw ToolFailure("A drop-down list's row takes a plain left click, which picks it");
        if (!drop->ClickItem(row.index))
            throw ToolFailure("\"" + row.path.back() + "\" can't be picked");
        return {{"clicked", joined(row.path, ">")}};
    }
    throw ToolFailure("Not a list");
}

json input_row(wxWindow& list, const ListRow& row, const std::string& column, const json& value)
{
    if (!list.IsShownOnScreen())
        throw ToolFailure("The list is no longer shown; take a new ui_snapshot");
    if (!list.IsEnabled())
        throw ToolFailure("The list is disabled");
    if (dynamic_cast<wxGrid*>(&list) != nullptr)
        throw ToolFailure("Table cells are read-only here; edit object settings with set_settings {scope: \"object\"}");
    const std::string text = value.is_string() ? lower(value.get<std::string>()) : value.dump();
    if (!value.is_boolean() && text != "true" && text != "false" && text != "1" && text != "0")
        throw ToolFailure("A row takes true or false, for its checkbox");
    const bool want = value.is_boolean() ? value.get<bool>() : (text == "true" || text == "1");
    if (auto* view = dynamic_cast<wxDataViewCtrl*>(&list)) {
        // The checkbox cell: the column named, else the row's first.
        wxDataViewModel*               model = view->GetModel();
        std::vector<wxDataViewColumn*> cols  = column.empty() ? shown_columns(*view) : std::vector<wxDataViewColumn*>{data_view_column(*view, column)};
        for (wxDataViewColumn* col : cols) {
            wxVariant now;
            model->GetValue(now, row.item, col->GetModelColumn());
            if (now.GetType() != "bool")
                continue;
            view->EnsureVisible(row.item, col);
            // What the toggle renderer does on a click; the model's notifier tells the owner
            // (wxEVT_DATAVIEW_ITEM_VALUE_CHANGED).
            if (now.GetBool() != want)
                model->ChangeValue(wxVariant(want), row.item, col->GetModelColumn());
            model->GetValue(now, row.item, col->GetModelColumn());
            return {{"row", joined(row.path, ">")}, {"checked", now.GetBool()}};
        }
    } else if (auto* checks = dynamic_cast<wxCheckListBox*>(&list)) {
        const unsigned n = unsigned(row.index);
        if (checks->IsChecked(n) != want) {
            checks->Check(n, want);
            command(checks, wxEVT_CHECKLISTBOX, int(n), checks->GetString(n));
        }
        return {{"row", joined(row.path, ">")}, {"checked", checks->IsChecked(n)}};
    }
    throw ToolFailure("This row has no checkbox; use ui_click");
}

// ---- snapshot ---------------------------------------------------------------------------------

bool UiInspector::push(UiNode node, UiTarget target, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& o)
{
    if (!o.query.empty()) {
        const std::string q = lower(o.query);
        if (lower(node.label).find(q) == std::string::npos && lower(node.value).find(q) == std::string::npos &&
            lower(node.name).find(q) == std::string::npos && lower(node.cls).find(q) == std::string::npos && lower(node.kind) != q &&
            lower(node.option).find(q) == std::string::npos)
            return false;
    }
    if (int(nodes.size()) >= o.max_nodes) {
        ++omitted;
        return false;
    }
    const uint64_t generation = m_service.ui_generation();
    std::function<bool()> alive;
    char                  kind = 'w';
    switch (target.kind) {
    case UiTarget::Kind::Window: {
        wxWeakRef<wxWindow> weak = target.window;
        const int           page = target.page;
        const std::string   text = target.name;
        alive = [weak, page, text] {
            wxWindow* w = weak.get();
            if (w == nullptr || page < 0)
                return w != nullptr;
            auto* book = dynamic_cast<wxBookCtrlBase*>(w);
            return book != nullptr && page < int(book->GetPageCount()) && into_u8(book->GetPageText(size_t(page))) == text;
        };
        break;
    }
    case UiTarget::Kind::MenuItem: {
        kind = 'm';
        const int id = target.menu_id;
        const std::string path = target.menu_path;
        alive = [id, path] {
            std::string now;
            return find_menu_item(id, &now, nullptr) != nullptr && now == path;
        };
        break;
    }
    case UiTarget::Kind::ToolbarItem: {
        kind = 't';
        GLCanvas3D* canvas = target.canvas;
        const std::string name = target.name;
        alive = [canvas, name] { return is_live_canvas(canvas) && canvas->get_main_toolbar().get_item(name) != nullptr; };
        break;
    }
    case UiTarget::Kind::Gizmo: {
        kind = 'z';
        GLCanvas3D* canvas = target.canvas;
        alive = [canvas] { return is_live_canvas(canvas); };
        break;
    }
    case UiTarget::Kind::Row: {
        kind  = 'r';
        alive = [weak = target.window, path = target.row_path, twin = target.row_twin] {
            wxWindow* w = weak.get();
            return w != nullptr && find_row(*w, path, twin).has_value();
        };
        break;
    }
    case UiTarget::Kind::PlateIcon: {
        kind  = 'p';
        alive = [canvas = target.canvas, id = target.plate * kPlateIconCount + target.plate_action] {
            return is_live_canvas(canvas) && plate_picking_ids(*canvas).count(id) > 0;
        };
        break;
    }
    case UiTarget::Kind::ImGuiItem: {
        kind = 'g';
        GLCanvas3D* canvas = target.canvas;
        alive = [canvas, id = target.imgui_id, name = target.imgui_name, window = target.imgui_window, twins = target.imgui_twins] {
            return is_live_canvas(canvas) && wxGetApp().imgui() != nullptr &&
                   imgui_find_item(wxGetApp().imgui()->recorded_items(canvas), id, name, window, twins).has_value();
        };
        break;
    }
    }
    node.ref = m_refs.add(kind, generation, std::move(target), std::move(alive));
    nodes.push_back(std::move(node));
    return true;
}

void UiInspector::add_menus(wxMenu* menu, const std::string& path, wxWindow* frame, int depth, std::vector<UiNode>& nodes,
                            std::size_t& omitted, const SnapshotOptions& o)
{
    for (wxMenuItem* item : menu->GetMenuItems()) {
        if (item->IsSeparator())
            continue;
        const std::string label = into_u8(item->GetItemLabelText());
        const std::string here  = path + ">" + label;
        UiNode node;
        node.kind    = item->IsSubMenu() ? "menu" : "menu_item";
        node.label   = label;
        node.enabled = item->IsEnabled();
        node.depth   = depth;
        if (item->IsCheckable())
            node.checked = item->IsChecked() ? 1 : 0;
        if (item->IsSubMenu()) {
            // Submenus are listed for structure; their items carry the refs.
            if (o.query.empty() && int(nodes.size()) < o.max_nodes) {
                node.ref = "-";
                nodes.push_back(node);
            }
            add_menus(item->GetSubMenu(), here, frame, depth + 1, nodes, omitted, o);
            continue;
        }
        UiTarget t;
        t.kind      = UiTarget::Kind::MenuItem;
        t.window    = frame;
        t.menu_id   = item->GetId();
        t.menu_path = here;
        t.locator   = {{"menu", here}};
        push(std::move(node), std::move(t), nodes, omitted, o);
    }
}

void UiInspector::add_canvas_items(GLCanvas3D& canvas, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& o)
{
    // Plate icons and ImGui items are what the last frame drew. A canvas that changed since (a new
    // project, a tab just shown) waits for its paint, which a Wayland compositor holds back until its
    // frame callback, so it is drawn now and the items match it.
    if (canvas.get_wxglcanvas()->IsShownOnScreen()) {
        canvas.set_as_dirty();
        canvas.render();
    }
    const std::string cname   = canvas_name(&canvas);
    bool              changed = false;
    for (GLToolbarItem* item : toolbar_items(canvas.get_main_toolbar(), &changed)) {
        if (item == nullptr || item->is_separator() || !item->is_visible())
            continue;
        UiNode node;
        node.kind    = "gltoolbar";
        node.label   = item->get_tooltip().substr(0, item->get_tooltip().find('\n'));
        node.name    = item->get_name();
        node.enabled = !item->is_disabled();
        node.checked = item->is_pressed() ? 1 : -1;
        node.depth   = depth;
        UiTarget t;
        t.kind    = UiTarget::Kind::ToolbarItem;
        t.window  = canvas.get_wxglcanvas();
        t.canvas  = &canvas;
        t.name    = item->get_name();
        t.locator = {{"gltoolbar", item->get_name()}, {"canvas", cname}};
        push(std::move(node), std::move(t), nodes, omitted, o);
    }
    if (changed)
        canvas.set_as_dirty();
    GLGizmosManager& gizmos = canvas.get_gizmos_manager();
    if (gizmos.is_enabled())
        for (int type = 0; type < int(GLGizmosManager::Undefined); ++type) {
            GLGizmoBase* g = gizmos.get_gizmo(GLGizmosManager::EType(type));
            if (g == nullptr)
                continue;
            // The toolbar's gizmos, and on the Prepare canvas the two that open from the object list
            // while they can open now: Simplify, and SVG on a selected SVG part.
            const bool on_toolbar = gizmos.is_on_toolbar(GLGizmosManager::EType(type));
            const bool prepare    = cname == std::string("3d");
            if (!on_toolbar && !(prepare && type == GLGizmosManager::Simplify && g->is_activable()) &&
                !(prepare && type == GLGizmosManager::Svg && selection_is_svg_part(canvas)))
                continue;
            UiNode node;
            node.kind    = "gizmo";
            node.label   = gizmo_name(type);
            node.enabled = g->is_activable();
            node.checked = gizmos.get_current_type() == GLGizmosManager::EType(type) ? 1 : 0;
            node.note    = on_toolbar ? "toolbar" : "no toolbar icon";
            node.depth   = depth;
            UiTarget t;
            t.kind    = UiTarget::Kind::Gizmo;
            t.window  = canvas.get_wxglcanvas();
            t.canvas  = &canvas;
            t.gizmo   = type;
            t.name    = node.label;
            t.locator = {{"gizmo", node.label}, {"canvas", cname}};
            push(std::move(node), std::move(t), nodes, omitted, o);
        }
    // The Prepare view's plates, then the current plate's icons; a plate_icon locator reaches the
    // icons of any plate.
    if (cname == std::string("3d")) {
        PartPlateList&      plates  = wxGetApp().plater()->get_partplate_list();
        const std::set<int> shown   = plate_picking_ids(canvas);
        const int           current = plates.get_curr_plate_index();
        auto add = [&](int plate, int action) {
            if (shown.count(plate * kPlateIconCount + action) == 0)
                return;
            UiNode node;
            node.kind  = "plate_icon";
            node.label = action == 0 ? plate_label(*plates.get_plate(plate)) : plate_icon_name(action);
            node.name  = action == 0 ? "plate" : "";
            node.note  = "plate " + std::to_string(plate) + (action == 0 && plate == current ? ", current" : "");
            if (action == plate_icon_action("lock"))
                node.checked = plates.is_locked(plate) ? 1 : 0;
            node.depth = depth;
            UiTarget t;
            t.kind         = UiTarget::Kind::PlateIcon;
            t.window       = canvas.get_wxglcanvas();
            t.canvas       = &canvas;
            t.name         = plate_icon_name(action);
            t.plate        = plate;
            t.plate_action = action;
            t.locator      = {{"plate_icon", t.name}, {"plate", plate}};
            push(std::move(node), std::move(t), nodes, omitted, o);
        };
        for (int plate = 0; plate < plates.get_plate_count(); ++plate)
            add(plate, 0);
        for (int action = 1; action < kPlateIconCount; ++action)
            add(current, action);
    }
    if (o.include_imgui && wxGetApp().imgui() != nullptr)
        add_imgui_items(canvas, depth, nodes, omitted, o);
}

// The ImGui windows of a canvas's last frame, each with its items and text in reading order. Text
// is listed for reading only: it has no ref and no locator matches it.
void UiInspector::add_imgui_items(GLCanvas3D& canvas, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& o)
{
    const std::string cname  = canvas_name(&canvas);
    const auto        items  = wxGetApp().imgui()->recorded_items(&canvas);
    const auto        texts  = wxGetApp().imgui()->recorded_texts(&canvas);
    const auto        labels = imgui_display_labels(items, texts);

    // A window's items and texts, row by row (by their middles: a label sits lower than its field's
    // frame) and left to right.
    struct Entry
    {
        float       y, x; // middle, left
        int         item; // index into items, or -1 for texts[text]
        std::size_t text;
    };
    std::vector<std::string>                   windows;
    std::map<std::string, std::vector<Entry>> entries;
    auto add = [&](const std::string& window, Entry e) {
        if (entries.find(window) == entries.end())
            windows.push_back(window);
        entries[window].push_back(e);
    };
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!labels[i].empty() || !imgui_item_name(items[i]).empty())
            add(items[i].window, {items[i].y + items[i].h / 2.f, items[i].x, int(i), 0});
    for (std::size_t i = 0; i < texts.size(); ++i)
        add(texts[i].window, {texts[i].y + texts[i].h / 2.f, texts[i].x, -1, i});

    // Nodes without a ref (window headers, text) obey the query and the node limit too.
    auto push_plain = [&](UiNode node) {
        if (!o.query.empty()) {
            const std::string q = lower(o.query);
            if (lower(node.label).find(q) == std::string::npos && lower(node.kind) != q)
                return;
        }
        if (int(nodes.size()) >= o.max_nodes) {
            ++omitted;
            return;
        }
        node.ref = "-";
        nodes.push_back(std::move(node));
    };
    for (const std::string& window : windows) {
        UiNode header;
        header.kind  = "imgui_window";
        header.label = imgui_window_label(window);
        header.depth = depth;
        // A child window is "<parent>/<name>_<8 hex digits>"; its name says which part of the panel it is.
        if (const auto slash = window.rfind('/'); slash != std::string::npos) {
            const std::string child = window.substr(slash + 1);
            const std::string stem  = child.size() > 9 ? child.substr(0, child.size() - 9) : child;
            header.name             = imgui_label_name(stem).empty() ? clean_imgui_label(stem) : imgui_label_name(stem);
        }
        if (ImGuiWindow* w = ImGui::GetCurrentContext() != nullptr ? ImGui::FindWindowByName(window.c_str()) : nullptr) {
            const wxPoint at = imgui_to_canvas(canvas, w->Pos.x, w->Pos.y);
            const wxPoint br = imgui_to_canvas(canvas, w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
            header.x = at.x, header.y = at.y, header.w = br.x - at.x, header.h = br.y - at.y;
            if (w->ScrollMax.y > 0.f)
                header.note = "scroll " + std::to_string(int(w->Scroll.y)) + "/" + std::to_string(int(w->ScrollMax.y)) +
                              "; ui_mouse wheel over it scrolls it";
        }
        push_plain(std::move(header));

        std::vector<Entry>& list = entries[window];
        std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.y < b.y; });
        for (auto row = list.begin(); row != list.end();) {
            const auto end = std::find_if(row, list.end(), [&](const Entry& e) { return e.y - row->y > 4.f; });
            std::stable_sort(row, end, [](const Entry& a, const Entry& b) { return a.x < b.x; });
            row = end;
        }
        for (const Entry& e : list) {
            if (e.item < 0) {
                const ImGuiWrapper::RecordedText& text = texts[e.text];
                UiNode node;
                node.kind  = "text";
                node.label = text.text.size() > 120 ? text.text.substr(0, 117) + "..." : text.text;
                std::replace(node.label.begin(), node.label.end(), '\n', ' ');
                node.depth = depth + 1;
                const wxPoint at = imgui_to_canvas(canvas, text.x, text.y);
                const wxPoint br = imgui_to_canvas(canvas, text.x + text.w, text.y + text.h);
                node.x = at.x, node.y = at.y, node.w = br.x - at.x, node.h = br.y - at.y;
                push_plain(std::move(node));
                continue;
            }
            const ImGuiWrapper::RecordedItem& item = items[std::size_t(e.item)];
            const std::string                 name = imgui_item_name(item);
            const wxPoint at = imgui_to_canvas(canvas, item.x, item.y);
            const wxPoint br = imgui_to_canvas(canvas, item.x + item.w, item.y + item.h);
            UiNode        node;
            node.kind  = "imgui";
            node.cls   = imgui_kind(item);
            node.label = labels[std::size_t(e.item)];
            node.name  = name == node.label ? std::string() : name;
            node.value = imgui_value(item);
            if (item.flags & ImGuiItemStatusFlags_Checkable)
                node.checked = (item.flags & ImGuiItemStatusFlags_Checked) ? 1 : 0;
            if ((node.cls == "slider" || node.cls == "drag") && (item.flags & ImGuiItemStatusFlags_Inputable) == 0)
                node.note = "no typing";
            if (item.clipped)
                node.note += std::string(node.note.empty() ? "" : ", ") + "clipped: ui_click and ui_input scroll it into view";
            node.depth = depth + 1;
            node.x = at.x, node.y = at.y, node.w = br.x - at.x, node.h = br.y - at.y;
            UiTarget t;
            t.kind         = UiTarget::Kind::ImGuiItem;
            t.window       = canvas.get_wxglcanvas();
            t.canvas       = &canvas;
            t.imgui_id     = item.id;
            t.imgui_label  = node.label.empty() ? name : node.label;
            t.imgui_name   = name;
            t.imgui_window = item.window;
            t.imgui_twins  = imgui_twins(items, name, item.window);
            t.x = item.x, t.y = item.y, t.w = item.w, t.h = item.h;
            // The shown label when no other item in its window shares it, else the name when that is
            // unique, else both (Cut's "Keep" checkboxes, named by their row); locator_for() adds an
            // index when even both repeat.
            const std::string wlabel = imgui_window_label(item.window);
            int               same_label = 0, same_name = 0;
            for (std::size_t j = 0; j < items.size(); ++j)
                if (imgui_window_label(items[j].window) == wlabel) {
                    same_label += labels[j] == node.label ? 1 : 0;
                    same_name += imgui_item_name(items[j]) == name ? 1 : 0;
                }
            if (!node.label.empty() && (same_label == 1 || name.empty()))
                t.locator = {{"imgui", node.label}};
            else if (node.label.empty() || same_name == 1)
                t.locator = {{"imgui_name", name}};
            else
                t.locator = {{"imgui", node.label}, {"imgui_name", name}};
            if (!wlabel.empty())
                t.locator["window"] = wlabel;
            t.locator["canvas"] = cname;
            push(std::move(node), std::move(t), nodes, omitted, o);
        }
    }
}

void UiInspector::walk(wxWindow* w, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& o)
{
    if (o.visible_only && !w->IsShownOnScreen())
        return;
    const std::string kind = kind_of(w);
    const std::string name = meaningful_name(w);
    std::string       label = label_of(w);
    const bool        top   = dynamic_cast<wxTopLevelWindow*>(w) != nullptr;
    const bool        popup = kind == "popup";
    const bool interesting  = top || (!kind.empty() && (kind != "label" || !label.empty())) || !name.empty();
    const bool        list  = is_row_list(w);
    int        child_depth  = depth;
    if (interesting) {
        UiNode node;
        node.kind    = kind.empty() ? "panel" : kind;
        node.cls     = short_class(w);
        node.value   = value_of(w);
        node.label   = label == node.value ? std::string() : label;
        node.name    = name;
        node.option  = option_key_of(w);
        node.enabled = w->IsEnabled();
        node.focused = wxWindow::FindFocus() == w;
        node.checked = checked_of(w);
        node.depth   = depth;
        if (list)
            if (const std::vector<std::string> columns = list_columns(w); columns.size() > 1)
                node.note = "columns: " + joined(columns, " | ");
        const wxPoint pos  = top || popup ? w->GetScreenPosition() : client_pos_in_tlw(w);
        const wxSize  size = w->GetSize();
        node.x = pos.x, node.y = pos.y, node.w = size.x, node.h = size.y;
        UiTarget t;
        t.kind   = UiTarget::Kind::Window;
        t.window = w;
        wxWindow* tlw = owning_window(w);
        // A web view's class differs per platform (WebKit, WebView2), so its locator names the kind.
        t.locator = {{"class", kind == "webview" ? kind : node.cls}};
        if (!label.empty())
            t.locator["label"] = label;
        if (!name.empty())
            t.locator["name"] = name;
        if (!node.option.empty())
            t.locator = {{"option", node.option}};
        // The main window's title follows the project name, so its widgets are found without it.
        if (tlw != nullptr && tlw != w && tlw != wxGetApp().mainframe)
            t.locator["within"] = label_of(tlw);
        push(std::move(node), std::move(t), nodes, omitted, o);
        child_depth = depth + 1;
    }
    if (top && w == wxGetApp().mainframe && o.include_menus)
        for (const auto& [label_text, menu] : main_menus()) {
            if (o.query.empty() && int(nodes.size()) < o.max_nodes) {
                UiNode m;
                m.ref   = "-";
                m.kind  = "menu";
                m.label = label_text;
                m.depth = child_depth;
                nodes.push_back(m);
            }
            add_menus(menu, label_text, w, child_depth + 1, nodes, omitted, o);
        }
    if (kind == "canvas")
        for (GLCanvas3D* canvas : ui_canvases())
            if (canvas->get_wxglcanvas() == w)
                add_canvas_items(*canvas, child_depth, nodes, omitted, o);
    // A notebook's pages, so one can be picked by its tab.
    if (auto* book = dynamic_cast<wxBookCtrlBase*>(w); book != nullptr && kind == "tabs")
        for (size_t i = 0; i < book->GetPageCount(); ++i) {
            UiNode page;
            page.kind    = "tab";
            page.label   = into_u8(book->GetPageText(i));
            page.checked = book->GetSelection() == int(i) ? 1 : 0;
            page.depth   = child_depth;
            UiTarget t;
            t.kind    = UiTarget::Kind::Window;
            t.window  = book;
            t.page    = int(i);
            t.name    = page.label;
            t.locator = {{"tab", page.label}};
            if (wxWindow* tlw = owning_window(book); tlw != nullptr && tlw != wxGetApp().mainframe)
                t.locator["within"] = label_of(tlw);
            push(std::move(page), std::move(t), nodes, omitted, o);
        }
    // A list's rows; row locators find them again by their labels (locator_for()).
    if (list)
        for (ListRow& r : list_rows(w)) {
            UiNode row;
            row.kind    = "row";
            row.label   = r.path.back();
            row.value   = r.value;
            row.checked = r.checked;
            row.enabled = r.enabled && w->IsEnabled();
            row.note    = r.selected ? (r.note.empty() ? "selected" : r.note + ", selected") : r.note;
            row.depth   = child_depth + r.depth;
            UiTarget t;
            t.kind     = UiTarget::Kind::Row;
            t.window   = w;
            t.row_path = std::move(r.path);
            t.row_twin = r.twin;
            push(std::move(row), std::move(t), nodes, omitted, o);
        }
    // Web views and canvases are listed by their own means; Orca's composite inputs are one
    // control to the agent, not their inner text field and buttons; a table's inner windows are its
    // rows.
    if (kind == "webview" || kind == "canvas" || kind == "text" || kind == "spin" || kind == "combo" || dynamic_cast<wxGrid*>(w) != nullptr)
        return;
    // Popups are listed after the window they belong to (snapshot()).
    for (wxWindow* child : w->GetChildren())
        if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr && dynamic_cast<wxPopupWindow*>(child) == nullptr)
            walk(child, child_depth, nodes, omitted, o);
}

json UiInspector::snapshot(const SnapshotOptions& o)
{
    std::vector<UiNode> nodes;
    std::size_t         omitted = 0;
    std::vector<wxWindow*> roots;
    // A window, then the popups that opened from it, before any window above it.
    auto with_popups = [&roots](wxWindow* top) {
        roots.push_back(top);
        for (wxWindow* popup : popups_of(top))
            roots.push_back(popup);
    };
    if (o.root == "dialog") {
        if (wxDialog* d = m_service.modals().top())
            with_popups(d);
        else
            throw ToolFailure("No modal dialog is open");
    } else if (o.root == "main") {
        if (wxGetApp().mainframe == nullptr)
            throw ToolFailure("The main window is not up yet");
        with_popups(wxGetApp().mainframe);
    } else if (o.root == "popup") {
        for (wxTopLevelWindow* tlw : top_level_windows(m_service))
            for (wxWindow* popup : popups_of(tlw))
                roots.push_back(popup);
    } else if (o.root == "all" || o.root.empty()) {
        for (wxTopLevelWindow* tlw : top_level_windows(m_service))
            with_popups(tlw);
    } else {
        const UiTarget t = resolve(json(o.root));
        if (t.kind != UiTarget::Kind::Window || t.window.get() == nullptr)
            throw ToolFailure("root must be all, main, dialog, popup or the ref of a window");
        roots.push_back(t.window.get());
    }
    for (wxWindow* root : roots)
        walk(root, 0, nodes, omitted, o);
    json out{{"tree", format_snapshot(nodes, omitted)}, {"nodes", nodes.size()}};
    if (omitted > 0)
        out["omitted"] = omitted;
    json dialogs = m_service.modals().snapshot();
    if (!dialogs.empty())
        out["dialogs"] = std::move(dialogs);
    return out;
}

// ---- resolution -------------------------------------------------------------------------------

UiTarget UiInspector::resolve(const json& target)
{
    if (target.is_string()) {
        std::string     err;
        const UiTarget* t = m_refs.resolve(target.get<std::string>(), m_service.ui_generation(), err);
        if (t == nullptr)
            throw ToolFailure(err);
        UiTarget out = *t;
        // The item may have moved since the snapshot, changed its id with its label, or been renamed by
        // its place ("Layer 3" becoming "Layer 2" when a layer before it goes).
        if (out.kind == UiTarget::Kind::ImGuiItem) {
            const auto items = wxGetApp().imgui()->recorded_items(out.canvas);
            if (const auto at = imgui_find_item(items, out.imgui_id, out.imgui_name, out.imgui_window, out.imgui_twins)) {
                const ImGuiWrapper::RecordedItem& item = items[*at];
                out.imgui_id   = item.id;
                out.imgui_name = imgui_item_name(item);
                out.x = item.x, out.y = item.y, out.w = item.w, out.h = item.h;
            }
        }
        return out;
    }
    if (!target.is_object())
        throw ToolFailure("target must be a ref from ui_snapshot or a locator object");

    UiTarget t;
    t.locator = target;
    if (target.contains("menu")) {
        int         id   = -1;
        const std::string path = target["menu"].get<std::string>();
        if (find_menu_path(path, id) == nullptr)
            throw ToolFailure("No menu item " + path + " (ui_snapshot lists them)");
        t.kind      = UiTarget::Kind::MenuItem;
        t.window    = wxGetApp().mainframe;
        t.menu_id   = id;
        t.menu_path = path;
        return t;
    }
    if (target.contains("plate_icon")) {
        Plater* plater = wxGetApp().plater();
        t.kind         = UiTarget::Kind::PlateIcon;
        t.canvas       = plater != nullptr ? plater->get_view3D_canvas3D() : nullptr;
        if (t.canvas == nullptr)
            throw ToolFailure("No Prepare view");
        t.window       = t.canvas->get_wxglcanvas();
        t.name         = target["plate_icon"].is_string() ? target["plate_icon"].get<std::string>() : std::string();
        t.plate_action = plate_icon_action(t.name);
        if (t.plate_action < 0) {
            json names = json::array();
            for (int a = 0; a < kPlateIconCount; ++a)
                names.push_back(plate_icon_name(a));
            throw ToolFailure("No plate icon \"" + t.name + "\"", {{"plate_icons", names}});
        }
        if (!target.contains("plate") || !target["plate"].is_number_integer())
            throw ToolFailure("A plate_icon locator needs plate, the 0-based plate index");
        t.plate          = target["plate"].get<int>();
        const int plates = plater->get_partplate_list().get_plate_count();
        if (t.plate < 0 || t.plate >= plates)
            throw ToolFailure(no_such_plate(t.plate, plates));
        return t;
    }
    const bool imgui = target.contains("imgui") || target.contains("imgui_name");
    if (target.contains("gltoolbar") || target.contains("gizmo") || imgui) {
        t.canvas = canvas_by_name(target.value("canvas", imgui ? "" : "3d"));
        if (t.canvas == nullptr)
            throw ToolFailure("No such canvas");
        t.window = t.canvas->get_wxglcanvas();
        if (target.contains("gltoolbar")) {
            t.kind = UiTarget::Kind::ToolbarItem;
            t.name = target["gltoolbar"].get<std::string>();
            if (t.canvas->get_main_toolbar().get_item(t.name) == nullptr)
                throw ToolFailure("No toolbar item " + t.name);
            return t;
        }
        if (target.contains("gizmo")) {
            t.kind  = UiTarget::Kind::Gizmo;
            t.name  = target["gizmo"].get<std::string>();
            t.gizmo = int(gizmo_from_name(t.name));
            if (t.gizmo == int(GLGizmosManager::Undefined))
                throw ToolFailure("No gizmo " + t.name);
            return t;
        }
        // ImGui item: search the named canvas, or every canvas when none was named.
        const auto matches = match_imgui(target);
        const int  index   = target.value("index", -1);
        if (matches.empty())
            throw ToolFailure("No ImGui item matches " + target.dump() + " in the last frame; show its panel first (ui_snapshot lists them)");
        if (matches.size() > 1 && index < 0)
            throw ToolFailure(std::to_string(matches.size()) + " ImGui items match " + target.dump() + "; add \"index\" (0-based) or use a ref");
        if (index >= int(matches.size()))
            throw ToolFailure("index " + std::to_string(index) + " is past the " + std::to_string(matches.size()) + " matches");
        const auto& [canvas, item] = matches[index < 0 ? 0 : std::size_t(index)];
        t.kind         = UiTarget::Kind::ImGuiItem;
        t.canvas       = canvas;
        t.window       = canvas->get_wxglcanvas();
        t.imgui_id     = item.id;
        t.imgui_label  = target.contains("imgui") ? target["imgui"].get<std::string>() : target["imgui_name"].get<std::string>();
        t.imgui_name   = imgui_item_name(item);
        t.imgui_window = item.window;
        t.imgui_twins  = imgui_twins(wxGetApp().imgui()->recorded_items(canvas), t.imgui_name, item.window);
        t.x = item.x, t.y = item.y, t.w = item.w, t.h = item.h;
        return t;
    }

    const int index = target.value("index", -1);
    if (target.contains("row")) {
        const auto matches = match_rows(target);
        if (matches.empty()) {
            // The rows a person would look at instead: those whose label holds the one asked for.
            const std::string wanted = lower(row_segments(target["row"]).back());
            json              similar = json::array(), any = json::array();
            for (const auto& [list, row] : match_rows(json{{"row", json::array()}, {"within", target.value("within", "")}})) {
                const std::string path = joined(row.path, ">");
                if (!row.path.back().empty() && similar.size() < 30 && lower(row.path.back()).find(wanted) != std::string::npos)
                    similar.push_back(path);
                if (any.size() < 30)
                    any.push_back(path);
            }
            throw ToolFailure("No shown list has a row " + target["row"].dump() + "; take a ui_snapshot", {{"rows", similar.empty() ? any : similar}});
        }
        if (matches.size() > 1 && index < 0)
            throw ToolFailure(std::to_string(matches.size()) + " rows match " + target.dump() + "; add \"index\" (0-based) or use a ref");
        if (index >= int(matches.size()))
            throw ToolFailure("index " + std::to_string(index) + " is past the " + std::to_string(matches.size()) + " matches");
        const auto& [list, row] = matches[index < 0 ? 0 : std::size_t(index)];
        t.kind     = UiTarget::Kind::Row;
        t.window   = list;
        t.row_path = row.path;
        t.row_twin = row.twin;
        t.column   = target.value("column", "");
        return t;
    }
    if (target.contains("tab")) {
        const auto matches = match_tabs(target);
        if (matches.empty())
            throw ToolFailure("No tab " + target.dump() + "; take a ui_snapshot");
        if (matches.size() > 1 && index < 0)
            throw ToolFailure(std::to_string(matches.size()) + " tabs match " + target.dump() + "; add \"index\" (0-based) or use a ref");
        if (index >= int(matches.size()))
            throw ToolFailure("index " + std::to_string(index) + " is past the " + std::to_string(matches.size()) + " matches");
        const auto& [book, page] = matches[index < 0 ? 0 : std::size_t(index)];
        t.kind   = UiTarget::Kind::Window;
        t.window = book;
        t.page   = page;
        t.name   = into_u8(book->GetPageText(size_t(page)));
        return t;
    }

    // A wx window by option key, class, label or name, optionally within a titled top-level window.
    if (!target.contains("option") && !target.contains("class") && !target.contains("label") && !target.contains("name"))
        throw ToolFailure("A locator needs option, label, name or class (or menu, tab, row, gltoolbar, gizmo, imgui, imgui_name)");
    const std::vector<wxWindow*> matches = match_windows(target);
    if (matches.empty())
        throw ToolFailure("Nothing matches " + target.dump() + "; take a ui_snapshot");
    if (matches.size() > 1 && index < 0)
        throw ToolFailure(std::to_string(matches.size()) + " elements match " + target.dump() + "; add \"index\" (0-based) or use a ref");
    if (index >= int(matches.size()))
        throw ToolFailure("index " + std::to_string(index) + " is past the " + std::to_string(matches.size()) + " matches");
    t.kind   = UiTarget::Kind::Window;
    t.window = matches[index < 0 ? 0 : std::size_t(index)];
    return t;
}

std::vector<wxWindow*> UiInspector::match_windows(const json& locator)
{
    const std::string option = locator.value("option", "");
    const std::string cls    = locator.value("class", "");
    const std::string label  = locator.value("label", "");
    const std::string name   = locator.value("name", "");
    const std::string within = locator.value("within", "");
    std::vector<wxWindow*> matches;
    std::function<void(wxWindow*)> visit = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return;
        if ((option.empty() || option_key_of(w) == option) &&
            (cls.empty() || short_class(w) == cls || (cls == "webview" && dynamic_cast<wxWebView*>(w) != nullptr)) &&
            (label.empty() || label_of(w) == label || value_of(w) == label) && (name.empty() || into_u8(w->GetName()) == name)) {
            matches.push_back(w);
            if (!option.empty())
                return; // not the inner text control too
        }
        for (wxWindow* child : w->GetChildren())
            if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr)
                visit(child);
    };
    for (wxTopLevelWindow* tlw : top_level_windows(m_service))
        if (within.empty() || label_of(tlw) == within)
            visit(tlw);
    if (matches.size() > 1 && !locator.contains("class")) {
        // A row's label and the field it names share the text; the field is what an agent acts on.
        std::vector<wxWindow*> controls;
        for (wxWindow* w : matches) {
            const std::string k = kind_of(w);
            if (k != "label" && k != "control" && k != "window" && k != "dialog" && !k.empty())
                controls.push_back(w);
        }
        if (controls.size() == 1)
            return controls;
    }
    return matches;
}

// {imgui: <label>} finds items by the label they are listed by, {imgui_name: <name>} by name, and
// both together by both; text is never matched.
std::vector<std::pair<GLCanvas3D*, ImGuiWrapper::RecordedItem>> UiInspector::match_imgui(const json& locator)
{
    const bool        by_label = locator.contains("imgui"), by_name = locator.contains("imgui_name");
    const std::string label    = locator.value("imgui", ""), name = locator.value("imgui_name", "");
    const std::string window   = locator.value("window", "");
    std::vector<GLCanvas3D*> canvases = ui_canvases();
    if (locator.contains("canvas")) {
        GLCanvas3D* named = canvas_by_name(locator["canvas"].get<std::string>());
        canvases          = named != nullptr ? std::vector<GLCanvas3D*>{named} : std::vector<GLCanvas3D*>{};
    }
    std::vector<std::pair<GLCanvas3D*, ImGuiWrapper::RecordedItem>> out;
    for (GLCanvas3D* c : canvases) {
        // A hidden canvas keeps the items of the last frame it drew.
        if (!c->get_wxglcanvas()->IsShownOnScreen())
            continue;
        const auto items  = wxGetApp().imgui()->recorded_items(c);
        const auto labels = by_label ? imgui_display_labels(items, wxGetApp().imgui()->recorded_texts(c)) : std::vector<std::string>();
        for (std::size_t i = 0; i < items.size(); ++i)
            if ((!by_label || labels[i] == label) && (!by_name || imgui_item_name(items[i]) == name) &&
                (window.empty() || imgui_window_label(items[i].window) == window))
                out.emplace_back(c, items[i]);
    }
    return out;
}

// {row: <labels>} finds rows of the shown lists by the last labels of their paths; an empty array finds
// every row.
std::vector<std::pair<wxWindow*, ListRow>> UiInspector::match_rows(const json& locator)
{
    const std::string      within = locator.value("within", "");
    std::vector<wxWindow*> roots;
    for (wxTopLevelWindow* tlw : top_level_windows(m_service))
        if (within.empty() || label_of(tlw) == within)
            roots.push_back(tlw);
    const json& row = locator["row"];
    return find_rows(roots, row.is_array() && row.empty() ? std::vector<std::string>() : row_segments(row));
}

std::vector<std::pair<wxBookCtrlBase*, int>> UiInspector::match_tabs(const json& locator)
{
    const std::string text   = locator["tab"].get<std::string>();
    const std::string within = locator.value("within", "");
    std::vector<std::pair<wxBookCtrlBase*, int>> out;
    std::function<void(wxWindow*)> visit = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return;
        if (auto* book = dynamic_cast<wxBookCtrlBase*>(w))
            for (size_t i = 0; i < book->GetPageCount(); ++i)
                if (into_u8(book->GetPageText(i)) == text)
                    out.emplace_back(book, int(i));
        for (wxWindow* child : w->GetChildren())
            if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr)
                visit(child);
    };
    for (wxTopLevelWindow* tlw : top_level_windows(m_service))
        if (within.empty() || label_of(tlw) == within)
            visit(tlw);
    return out;
}

json UiInspector::locator_for(const UiTarget& t)
{
    json loc = t.locator;
    if (loc.is_object()) // a row's is built below
        loc.erase("index");
    int position = -1, count = 0;
    if (t.kind == UiTarget::Kind::ImGuiItem && (loc.contains("imgui") || loc.contains("imgui_name"))) {
        auto find = [&] {
            const auto matches = match_imgui(loc);
            count              = int(matches.size());
            for (int i = 0; i < count; ++i)
                if (matches[i].first == t.canvas && matches[i].second.id == t.imgui_id)
                    position = i;
        };
        find();
        // A ref taken while an item showed another label (Text's font button) or had another name
        // ("Layer 3", now "Layer 2") no longer finds it that way; its current name does.
        if (position < 0 && !t.imgui_name.empty()) {
            loc.erase("imgui");
            loc["imgui_name"] = t.imgui_name;
            find();
        }
    } else if (t.kind == UiTarget::Kind::Row) {
        // The row's whole path, which names it after other rows come and go, with its cell and window.
        loc = {{"row", row_locator(t.row_path)}};
        if (!t.column.empty())
            loc["column"] = t.column;
        if (wxWindow* top = owning_window(t.window.get()); top != nullptr && top != wxGetApp().mainframe)
            loc["within"] = label_of(top);
        const auto matches = match_rows(loc);
        count              = int(matches.size());
        for (int i = 0, twin = 0; i < count; ++i)
            if (matches[i].first == t.window.get() && matches[i].second.path == t.row_path && twin++ == t.row_twin)
                position = i;
    } else if (t.kind == UiTarget::Kind::Window && t.page >= 0 && loc.contains("tab")) {
        const auto matches = match_tabs(loc);
        count              = int(matches.size());
        for (int i = 0; i < count; ++i)
            if (matches[i].first == t.window.get() && matches[i].second == t.page)
                position = i;
    } else if (t.kind == UiTarget::Kind::Window && t.page < 0 && !loc.contains("menu")) {
        const auto matches = match_windows(loc);
        count              = int(matches.size());
        for (int i = 0; i < count; ++i)
            if (matches[i] == t.window.get())
                position = i;
    }
    if (count > 1 && position >= 0)
        loc["index"] = position;
    return loc;
}

void UiInspector::reveal(UiTarget& target)
{
    if (wxWindow* w = target.kind == UiTarget::Kind::Window ? target.window.get() : nullptr)
        scroll_into_view(w);
    if (target.kind != UiTarget::Kind::ImGuiItem)
        return;
    // The item where a frame drawn now puts it: a panel that just opened can still be settling its
    // size. A clipped item is scrolled to the middle of its window, then of each window it is a child
    // of, until it shows. ImGui applies a scroll at the window's next Begin(), so each step takes a
    // frame to apply and one to draw the item where it now is. A window that just appeared clips all
    // of its items in its first frame, which only sizes it: up to two more frames come first.
    target.canvas->set_as_dirty();
    target.canvas->render();
    ImGuiWrapper::RecordedItem item = imgui_record(target);
    for (int frame = 0; item.clipped && frame < 2; ++frame) {
        target.canvas->set_as_dirty();
        target.canvas->render();
        item = imgui_record(target);
    }
    for (ImGuiWindow* w = ImGui::FindWindowByName(item.window.c_str()); item.clipped && w != nullptr;
         w = (w->Flags & ImGuiWindowFlags_ChildWindow) != 0 ? w->ParentWindow : nullptr) {
        if (w->ScrollMax.y <= 0.f)
            continue;
        ImGui::SetScrollFromPosY(w, item.y + item.h / 2.f - w->Pos.y, 0.5f);
        for (int frame = 0; frame < 2; ++frame) {
            target.canvas->set_as_dirty();
            target.canvas->render();
        }
        item = imgui_record(target);
    }
    if (item.clipped)
        throw ToolFailure("\"" + target.imgui_label + "\" is outside the visible part of its panel, which does not scroll to it; "
                          "make the window taller with set_view window_size");
    target.x = item.x, target.y = item.y, target.w = item.w, target.h = item.h;
}

ImGuiWrapper::RecordedItem UiInspector::imgui_record(const UiTarget& t) const
{
    if (!is_live_canvas(t.canvas) || wxGetApp().imgui() == nullptr)
        throw ToolFailure("The 3D view of the ImGui item is gone");
    const auto items = wxGetApp().imgui()->recorded_items(t.canvas);
    if (const auto at = imgui_find_item(items, t.imgui_id, t.imgui_name, t.imgui_window, t.imgui_twins))
        return items[*at];
    throw ToolFailure("\"" + t.imgui_label + "\" is no longer shown; take a new ui_snapshot");
}

GLCanvas3D* UiInspector::canvas_of(const UiTarget* target) const
{
    if (target != nullptr && target->canvas != nullptr && is_live_canvas(target->canvas))
        return target->canvas;
    if (target != nullptr && target->window.get() != nullptr)
        for (GLCanvas3D* c : ui_canvases())
            if (c->get_wxglcanvas() == target->window.get())
                return c;
    return target == nullptr ? shown_canvas() : nullptr;
}

// ---- actions ----------------------------------------------------------------------------------

bool UiInspector::place_row(const UiTarget& t)
{
    auto* ctrl = t.kind == UiTarget::Kind::Row ? dynamic_cast<wxDataViewCtrl*>(t.window.get()) : nullptr;
    const std::optional<ListRow> row = ctrl != nullptr ? find_row(*ctrl, t.row_path, t.row_twin) : std::nullopt;
    if (!row)
        return true; // nothing to place; the click says why
    wxDataViewColumn* col = data_view_column(*ctrl, t.column);
    ctrl->EnsureVisible(row->item, col);
    scroll_into_view(ctrl);
    return !ctrl->GetItemRect(row->item, col).IsEmpty();
}

json UiInspector::click(const UiTarget& t, const ClickOptions& o)
{
    switch (t.kind) {
    case UiTarget::Kind::MenuItem: {
        wxMenu*     owner = nullptr;
        wxMenuItem* item  = find_menu_item(t.menu_id, nullptr, &owner);
        if (item == nullptr || owner == nullptr)
            throw ToolFailure("The menu item is gone; take a new ui_snapshot");
        if (!item->IsEnabled())
            throw ToolFailure("The menu item " + t.menu_path + " is disabled");
        int checked = -1;
        if (item->IsCheckable()) {
            item->Check(!item->IsChecked());
            checked = item->IsChecked() ? 1 : 0;
        }
        if (!owner->SendEvent(t.menu_id, checked)) {
            wxCommandEvent evt(wxEVT_MENU, t.menu_id);
            evt.SetInt(checked);
            send(wxGetApp().mainframe, evt);
        }
        return {{"clicked", t.menu_path}};
    }
    case UiTarget::Kind::ToolbarItem: {
        GLToolbar& tb = t.canvas->get_main_toolbar();
        toolbar_items(tb);
        GLToolbarItem* item = tb.get_item(t.name);
        if (item == nullptr)
            throw ToolFailure("The toolbar item is gone");
        if (item->is_disabled())
            throw ToolFailure("The toolbar item " + t.name + " is disabled");
        // The toolbar ignores actions while another togglable item is pressed.
        for (GLToolbarItem* other : tb.get_items())
            if (other != nullptr && other != item && other->is_pressed() && (other->is_left_toggable() || other->is_right_toggable()))
                throw ToolFailure("Close " + other->get_name() + " first: the toolbar ignores clicks while it is active");
        const bool was   = item->is_pressed();
        const bool right = o.button == "right";
        if (o.button == "middle")
            throw ToolFailure("Toolbar items take a left or right click");
        if (right)
            tb.force_right_action(tb.get_item_id(t.name), *t.canvas);
        else
            tb.force_left_action(tb.get_item_id(t.name), *t.canvas);
        t.canvas->set_as_dirty();
        t.canvas->render();
        if ((right ? item->is_right_toggable() : item->is_left_toggable()) && item->is_pressed() == was)
            throw ToolFailure("The toolbar did not react to " + t.name);
        return {{"clicked", t.name}, {"pressed", item->is_pressed()}};
    }
    case UiTarget::Kind::Gizmo: {
        if (o.button != "left" || o.double_click)
            throw ToolFailure("A gizmo opens with a single left click");
        GLGizmosManager& gizmos = t.canvas->get_gizmos_manager();
        const auto       type   = GLGizmosManager::EType(t.gizmo);
        GLGizmoBase*     g      = gizmos.get_gizmo(type);
        if (g == nullptr || !gizmos.is_enabled())
            throw ToolFailure("The " + t.name + " gizmo is not available in this view");
        if (!g->is_activable() && gizmos.get_current_type() != type)
            throw ToolFailure("The " + t.name + " gizmo cannot be opened now (select an object first?)");
        // What the gizmo's toolbar icon does, which for Text starts creating a text part when no text
        // is selected; the caller waits for that job.
        const GLGizmosManager::ToolbarClick outcome = gizmos.click_toolbar_icon(type);
        // Two frames: a panel's first one only sizes it, so its items are where they show from the second.
        for (int frame = 0; frame < 2; ++frame) {
            t.canvas->set_as_dirty();
            t.canvas->render();
        }
        switch (outcome) {
        case GLGizmosManager::ToolbarClick::Opened: return {{"gizmo", t.name}, {"active", true}};
        case GLGizmosManager::ToolbarClick::Closed: return {{"gizmo", t.name}, {"active", false}};
        case GLGizmosManager::ToolbarClick::CreationStarted: return {{"gizmo", t.name}, {"creating", true}};
        case GLGizmosManager::ToolbarClick::Nothing: break;
        }
        if (type == GLGizmosManager::Svg)
            throw ToolFailure("The SVG gizmo opens on a selected SVG part: select one, or add one with run_action add_primitive_svg");
        throw ToolFailure("The " + t.name + " gizmo did not open (select an object first?)");
    }
    case UiTarget::Kind::ImGuiItem: {
        UiTarget shown = t;
        reveal(shown);
        const wxPoint pos = imgui_to_canvas(*shown.canvas, shown.x + shown.w / 2.f, shown.y + shown.h / 2.f);
        if (const std::string cover = covered_by(*shown.canvas, pos, shown.imgui_window); !cover.empty())
            throw ToolFailure("\"" + shown.imgui_label + "\" is under " + cover +
                              ", which would take the click; close it, or enlarge the window with set_view window_size");
        canvas_click(*shown.canvas, pos, o);
        return {{"clicked", shown.imgui_label}, {"at", {pos.x, pos.y}}};
    }
    case UiTarget::Kind::Row: {
        wxWindow*                    list = t.window.get();
        const std::optional<ListRow> row  = list != nullptr ? find_row(*list, t.row_path, t.row_twin) : std::nullopt;
        if (!row)
            throw ToolFailure("The row is gone; take a new ui_snapshot");
        return click_row(*list, *row, t.column, o);
    }
    case UiTarget::Kind::PlateIcon: {
        // What the canvas does for a click on the icon's picking id, refused where the canvas takes
        // no plate clicks.
        GLCanvas3D& canvas = *t.canvas;
        if (!is_live_canvas(t.canvas) || !canvas.get_wxglcanvas()->IsShownOnScreen())
            throw ToolFailure("Plate icons are on the Prepare view; set_view {\"tab\": \"prepare\"} first");
        if (o.button == "middle" || o.double_click || o.at_point)
            throw ToolFailure("A plate icon takes a single left or right click");
        if (canvas.is_layers_editing_enabled())
            throw ToolFailure("Plate icons take no clicks while variable layer height is being edited; close it with "
                              "ui_click {\"gltoolbar\": \"layersediting\"} first");
        if (const auto open = canvas.get_gizmos_manager().get_current_type(); open != GLGizmosManager::Undefined)
            throw ToolFailure("Close the " + gizmo_name(open) + " gizmo first: plate icons take no clicks while it is open");
        Plater&        plater = *wxGetApp().plater();
        PartPlateList& plates = plater.get_partplate_list();
        if (t.plate >= plates.get_plate_count())
            throw ToolFailure(no_such_plate(t.plate, plates.get_plate_count()));
        const int id = t.plate * kPlateIconCount + t.plate_action;
        if (plate_picking_ids(canvas).count(id) == 0)
            throw ToolFailure("Plate " + std::to_string(t.plate) + " shows no " + t.name + " icon");
        const bool right   = o.button == "right";
        const bool arrange = t.plate_action == plate_icon_action("arrange");
        const bool job     = arrange || t.plate_action == plate_icon_action("orient");
        Worker&    worker  = plater.get_ui_job_worker();
        const bool idle    = worker.is_idle();
        // The arrange icon does nothing while this flag is set, which an arrange that was cancelled
        // or failed leaves behind.
        const bool arrange_stuck = arrange && plater.m_arrange_running.load();
        // A right click's plate menu is listed or run by the caller; where it would open matters only
        // to a menu that is shown.
        const wxSize size = canvas.get_wxglcanvas()->GetClientSize();
        canvas.click_plate(id, right, Vec2d(size.x / 2., size.y / 2.));
        canvas.set_as_dirty();
        json out{{"clicked", t.name}, {"plate", t.plate}};
        if (right)
            return out;
        if (t.plate_action == 0 || t.plate_action == plate_icon_action("move_to_front") || t.plate_action == plate_icon_action("delete"))
            out["current"] = plates.get_curr_plate_index();
        if (t.plate_action == plate_icon_action("delete"))
            out["plates"] = plates.get_plate_count();
        if (t.plate_action == plate_icon_action("lock"))
            out["locked"] = plates.is_locked(t.plate);
        if (t.plate_action == plate_icon_action("imex_mode"))
            out["imex_mode"] = plates.get_plate(t.plate)->get_imex_mode();
        if (job) {
            out["started"] = idle && !worker.is_idle();
            if (!out["started"].get<bool>() && arrange_stuck)
                out["note"] = "The arrange icon does nothing after an arrange was cancelled or failed, until another arrange "
                              "finishes; arrange with the arrange tool or run_action obj_arrange";
        }
        return out;
    }
    case UiTarget::Kind::Window: break;
    }

    wxWindow* w = t.window.get();
    if (w == nullptr)
        throw ToolFailure("The element is gone; take a new ui_snapshot");
    if (!w->IsEnabled())
        throw ToolFailure("The element is disabled");
    scroll_into_view(w);
    if (t.page >= 0) {
        auto* book = dynamic_cast<wxBookCtrlBase*>(w);
        if (book == nullptr || t.page >= int(book->GetPageCount()))
            throw ToolFailure("The tab is gone; take a new ui_snapshot");
        book->SetSelection(size_t(t.page)); // sends the page-changing and page-changed events
        return {{"clicked", t.name}, {"page", t.page}};
    }
    if (o.at_point && dynamic_cast<wxGLCanvas*>(w) == nullptr) {
        if (is_native_control(w))
            throw ToolFailure("x/y clicks a point of a custom-drawn widget; this is a native control, so use ui_click without x/y");
        click_window(w, o);
        return {{"clicked", label_of(w)}, {"at", {o.x, o.y}}};
    }
    if (dynamic_cast<wxGLCanvas*>(w) != nullptr) {
        GLCanvas3D* canvas = canvas_of(&t);
        if (canvas == nullptr)
            throw ToolFailure("Not a 3D canvas");
        const wxSize  size = w->GetClientSize();
        const wxPoint pos  = o.at_point ? wxPoint(o.x, o.y) : wxPoint(size.x / 2, size.y / 2);
        canvas_click(*canvas, pos, o);
        return {{"clicked", "canvas"}, {"at", {pos.x, pos.y}}};
    }
    const bool plain_left = o.button == "left" && !o.double_click && o.modifiers.empty();
    if (plain_left) {
        // Controls whose owners listen to command events get exactly the event a click produces.
        if (kind_of(w) == "button") {
            command(w, wxEVT_BUTTON);
            return {{"clicked", label_of(w)}};
        }
        if (auto* c = dynamic_cast<wxCheckBox*>(w)) {
            c->SetValue(!c->GetValue());
            command(w, wxEVT_CHECKBOX, c->GetValue() ? 1 : 0);
            return {{"clicked", label_of(w)}, {"checked", c->GetValue()}};
        }
        if (auto* tb = dynamic_cast<wxBitmapToggleButton*>(w)) {
            tb->SetValue(!tb->GetValue());
            command(w, wxEVT_TOGGLEBUTTON, tb->GetValue() ? 1 : 0);
            return {{"clicked", label_of(w)}, {"checked", tb->GetValue()}};
        }
        if (auto* tb = dynamic_cast<wxToggleButton*>(w)) {
            tb->SetValue(!tb->GetValue());
            command(w, wxEVT_TOGGLEBUTTON, tb->GetValue() ? 1 : 0);
            return {{"clicked", label_of(w)}, {"checked", tb->GetValue()}};
        }
        if (auto* r = dynamic_cast<wxRadioButton*>(w)) {
            r->SetValue(true);
            command(w, wxEVT_RADIOBUTTON, 1);
            return {{"clicked", label_of(w)}};
        }
    }
    click_window(w, o);
    return {{"clicked", label_of(w)}};
}

json UiInspector::input(const UiTarget& t, const json& value, bool submit)
{
    if (t.kind == UiTarget::Kind::ImGuiItem)
        return imgui_input(t, value, submit);
    if (t.kind == UiTarget::Kind::Row) {
        wxWindow*                    list = t.window.get();
        const std::optional<ListRow> row  = list != nullptr ? find_row(*list, t.row_path, t.row_twin) : std::nullopt;
        if (!row)
            throw ToolFailure("The row is gone; take a new ui_snapshot");
        return input_row(*list, *row, t.column, value);
    }
    if (t.kind != UiTarget::Kind::Window || t.window.get() == nullptr)
        throw ToolFailure("ui_input works on controls, list rows and ImGui fields");
    wxWindow* w = t.window.get();
    if (!w->IsEnabled())
        throw ToolFailure("The control is disabled");
    scroll_into_view(w);
    const std::string text = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "1" : "0") : value.dump();

    if (checked_of(w) >= 0) {
        const bool want = value.is_boolean() ? value.get<bool>() : (text == "1" || lower(text) == "true");
        if ((checked_of(w) == 1) != want)
            click(t, ClickOptions{});
        return {{"checked", checked_of(w) == 1}};
    }
    if (auto* spin = dynamic_cast<::SpinInput*>(w)) {
        spin->SetValue(value.is_number() ? value.get<int>() : std::stoi(text));
        command(w, wxEVT_SPINCTRL, spin->GetValue());
        return {{"value", spin->GetValue()}};
    }
    if (auto* spin = dynamic_cast<wxSpinCtrl*>(w)) {
        spin->SetValue(value.is_number() ? value.get<int>() : std::stoi(text));
        wxSpinEvent evt(wxEVT_SPINCTRL, w->GetId());
        evt.SetPosition(spin->GetValue());
        send(w, evt);
        return {{"value", spin->GetValue()}};
    }
    if (auto* spin = dynamic_cast<wxSpinCtrlDouble*>(w)) {
        spin->SetValue(value.is_number() ? value.get<double>() : std::stod(text));
        wxSpinDoubleEvent evt(wxEVT_SPINCTRLDOUBLE, w->GetId(), spin->GetValue());
        send(w, evt);
        return {{"value", spin->GetValue()}};
    }
    if (auto* slider = dynamic_cast<wxSlider*>(w)) {
        slider->SetValue(value.is_number() ? value.get<int>() : std::stoi(text));
        command(w, wxEVT_SLIDER, slider->GetValue());
        return {{"value", slider->GetValue()}};
    }
    if (auto* items = dynamic_cast<wxItemContainer*>(w); items != nullptr && (dynamic_cast<wxChoice*>(w) != nullptr ||
                                                                             dynamic_cast<wxComboBox*>(w) != nullptr ||
                                                                             dynamic_cast<::ComboBox*>(w) != nullptr ||
                                                                             dynamic_cast<wxListBox*>(w) != nullptr)) {
        int index = value.is_number_integer() ? value.get<int>() : items->FindString(from_u8(text), true);
        if (index == wxNOT_FOUND || index < 0 || index >= int(items->GetCount())) {
            json options = json::array();
            for (unsigned i = 0; i < items->GetCount() && i < 50; ++i)
                options.push_back(into_u8(items->GetString(i)));
            throw ToolFailure("\"" + text + "\" is not one of the options", {{"options", options}});
        }
        items->SetSelection(index);
        const wxEventType type = dynamic_cast<wxChoice*>(w) != nullptr ? wxEVT_CHOICE : dynamic_cast<wxListBox*>(w) != nullptr ? wxEVT_LISTBOX : wxEVT_COMBOBOX;
        command(w, type, index, items->GetString(unsigned(index)));
        return {{"selected", into_u8(items->GetString(unsigned(index)))}, {"index", index}};
    }
    if (wxTextEntry* te = text_entry(w)) {
        wxWindow* field = dynamic_cast<wxWindow*>(te);
        te->SetValue(from_u8(text)); // sends wxEVT_TEXT
        if (submit && field != nullptr) {
            command(field, wxEVT_TEXT_ENTER, 0, from_u8(text));
            // Orca's setting fields commit when they lose the focus.
            wxFocusEvent lost(wxEVT_KILL_FOCUS, field->GetId());
            send(field, lost);
        }
        return {{"value", into_u8(te->GetValue())}};
    }
    throw ToolFailure("This element (" + short_class(w) + ") takes no input; use ui_click");
}

// Sets an ImGui control the way a person does, chosen by its kind, and reads the value back.
json UiInspector::imgui_input(UiTarget t, const json& value, bool submit)
{
    reveal(t);
    const ImGuiWrapper::RecordedItem item = imgui_record(t);
    const std::string kind = imgui_kind(item);
    const std::string text = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "true" : "false") : value.dump();
    if ((item.flags & ImGuiItemStatusFlags_Checkable) != 0) {
        const bool want = value.is_boolean() ? value.get<bool>() : (text == "1" || lower(text) == "true");
        if (((item.flags & ImGuiItemStatusFlags_Checked) != 0) != want)
            click(t, ClickOptions{});
        const bool now = (imgui_record(t).flags & ImGuiItemStatusFlags_Checked) != 0;
        if (now != want)
            throw ToolFailure("\"" + t.imgui_label + "\" is still " + (now ? "on" : "off") + " after a click");
        return {{"checked", now}};
    }

    if (kind == "combo") {
        // Open it, then click the option, which its popup lists from the next frame on.
        if ((item.flags & ImGuiItemStatusFlags_Opened) == 0) {
            click(t, ClickOptions{});
            // A new popup sizes itself on its first frame; its options are where they show from the next.
            t.canvas->set_as_dirty();
            t.canvas->render();
        }
        // The options: the popup's items that show a label or were named as options (a filtered list's
        // search field and clear button are neither), picked by text or by 0-based index.
        const auto items  = wxGetApp().imgui()->recorded_items(t.canvas);
        const auto labels = imgui_display_labels(items, wxGetApp().imgui()->recorded_texts(t.canvas));
        json       options = json::array();
        UiTarget   option  = t;
        option.imgui_id    = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!in_popup(items[i]) || imgui_kind(items[i]) == "input" || (labels[i].empty() && items[i].kind != "option"))
                continue;
            const std::string shown = labels[i].empty() ? imgui_item_name(items[i]) : labels[i];
            const bool        wanted = value.is_number_integer() ? value.get<int>() == int(options.size()) :
                                                                   (shown == text || imgui_item_name(items[i]) == text);
            options.push_back(shown);
            if (option.imgui_id == 0 && wanted) {
                option.imgui_id     = items[i].id;
                option.imgui_label  = shown;
                option.imgui_name   = imgui_item_name(items[i]);
                option.imgui_window = items[i].window;
                option.x = items[i].x, option.y = items[i].y, option.w = items[i].w, option.h = items[i].h;
            }
        }
        if (option.imgui_id == 0) {
            key(nullptr, "Escape", "");
            throw ToolFailure("\"" + text + "\" is not one of the options of \"" + t.imgui_label + "\"", {{"options", options}});
        }
        click(option, ClickOptions{});
        // A list that can't use the option (Text's font list on a font it can't load) drops it and
        // stays open.
        bool open = false, listed = false;
        for (const auto& i : wxGetApp().imgui()->recorded_items(t.canvas))
            if (in_popup(i)) {
                open   = true;
                listed = listed || i.id == option.imgui_id;
            }
        if (open) {
            key(nullptr, "Escape", "");
            throw ToolFailure(listed ? "The options of \"" + t.imgui_label + "\" are still open after picking \"" + option.imgui_label + "\"" :
                                       "\"" + t.imgui_label + "\" did not take \"" + option.imgui_label + "\" and dropped it from its options");
        }
        // A list shortens a long option to fit ("Academy Engraved L .."); the combo shows it whole.
        const std::string shown  = imgui_value(imgui_record(t));
        std::string       picked = option.imgui_label;
        if (const auto dots = picked.rfind(" .."); dots != std::string::npos && dots + 3 == picked.size())
            picked.resize(dots);
        if (shown.compare(0, picked.size(), picked) != 0)
            throw ToolFailure("\"" + t.imgui_label + "\" shows \"" + shown + "\" after picking \"" + option.imgui_label + "\"", {{"value", shown}});
        return {{"selected", shown}};
    }

    // A slider is set through the number field beside it when it has one, as Orca pairs them.
    UiTarget field = t;
    std::string field_kind = kind;
    if (kind == "slider") {
        float best = FLT_MAX;
        for (const auto& i : wxGetApp().imgui()->recorded_items(t.canvas)) {
            const std::string k  = imgui_kind(i);
            const float       cy = i.y + i.h / 2.f;
            if (i.window != item.window || (k != "drag" && k != "input") || i.x < item.x + item.w - 1.f || cy < item.y || cy > item.y + item.h)
                continue;
            if (i.x - item.x < best) {
                best               = i.x - item.x;
                field.imgui_id     = i.id;
                field.imgui_name   = imgui_item_name(i);
                field.imgui_window = i.window;
                field.x = i.x, field.y = i.y, field.w = i.w, field.h = i.h;
                field_kind = k;
            }
        }
        if (field_kind == "slider" && (item.flags & ImGuiItemStatusFlags_Inputable) == 0)
            throw ToolFailure("\"" + t.imgui_label + "\" is a slider that takes no typing and has no number field beside it; drag it with ui_mouse");
    }
    if (field_kind != "input" && field_kind != "multiline" && field_kind != "drag" && field_kind != "slider")
        throw ToolFailure("\"" + t.imgui_label + "\" is a " + field_kind + ", which takes no typed value; use ui_click");

    // A Ctrl+click turns a slider or drag into a text field; an input field takes a plain click.
    ClickOptions focus;
    if (field_kind == "drag" || field_kind == "slider")
        focus.modifiers = {"ctrl"};
    click(field, focus);
    // Select all and delete it, so an empty value empties the field too.
    key(&field, "Ctrl+A", "");
    key(&field, "Delete", "");
    if (field_kind == "multiline") {
        // Return adds a line, so it goes between lines and never after the last; the gizmo applies
        // the text as it changes.
        std::size_t start = 0;
        for (;;) {
            const std::size_t end = text.find('\n', start);
            key(&field, "", text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            key(&field, "Return", "");
            start = end + 1;
        }
    } else {
        key(&field, "", text);
        if (!submit)
            return {{"typed", value}, {"note", "Not submitted: the field keeps the text until Return or a click elsewhere"}};
        key(&field, "Return", "");
    }
    const std::string shown = imgui_record(field).value;
    if (!imgui_value_matches(shown, field_kind == "multiline" ? json(text) : value))
        throw ToolFailure("\"" + t.imgui_label + "\" shows \"" + shown + "\" after typing \"" + text + "\"", {{"value", shown}});
    return {{"value", shown}};
}

wxWindow* UiInspector::key_target(const UiTarget* t) const
{
    if (t != nullptr)
        return t->window.get();
    wxWindow* focus = wxWindow::FindFocus();
    if (wxDialog* top = m_service.modals().top())
        return focus != nullptr && owning_window(focus) == top ? focus : top;
    if (focus != nullptr)
        return focus;
    GLCanvas3D* canvas = canvas_of(nullptr);
    return canvas != nullptr ? canvas->get_wxglcanvas() : nullptr;
}

json UiInspector::key(const UiTarget* t, const std::string& chord_text, const std::string& text)
{
    wxWindow* target = key_target(t);
    if (target == nullptr)
        throw ToolFailure("No window to send keys to");
    if (t != nullptr)
        scroll_into_view(target);
    // A 3D view draws a frame after each key, so ImGui fields take what was typed.
    GLCanvas3D* canvas = nullptr;
    if (dynamic_cast<wxGLCanvas*>(target) != nullptr)
        for (GLCanvas3D* c : ui_canvases())
            if (c->get_wxglcanvas() == target)
                canvas = c;

    auto deliver = [&](int key_code, int modifiers, wxChar uni) {
        auto make = [&](wxEventType type) {
            wxKeyEvent evt(type);
            evt.m_keyCode = key_code;
            evt.m_uniChar = uni;
            set_modifiers(evt, modifiers);
            evt.SetEventObject(target);
            return evt;
        };
        std::vector<int> codes = modifier_keys(modifiers);
        codes.push_back(key_code);
        HeldKeys held(codes);
        bool     handled = false;
        if (key_code != 0) {
            // As a real key press: CHAR_HOOK goes to the target first and travels up through its
            // parents (wxKeyEvent propagates it) to the top-level window, or stops at a dialog, so the
            // hooks of the panels holding the target see it. A hook that allows the next event lets
            // KEY_DOWN through, as on every port.
            wxKeyEvent hook = make(wxEVT_CHAR_HOOK);
            handled         = target->GetEventHandler()->ProcessEvent(hook) && !hook.IsNextEventAllowed();
        }
        if (!handled && key_code != 0) {
            wxKeyEvent down = make(wxEVT_KEY_DOWN);
            handled         = target->GetEventHandler()->ProcessEvent(down);
        }
        if (!handled && uni != WXK_NONE) {
            wxKeyEvent ch = make(wxEVT_CHAR);
            target->GetEventHandler()->ProcessEvent(ch);
        }
        if (canvas != nullptr)
            canvas->render();
        SyntheticInput::release_key(key_code);
        if (key_code != 0) {
            wxKeyEvent up = make(wxEVT_KEY_UP);
            target->GetEventHandler()->ProcessEvent(up);
        }
        if (canvas != nullptr)
            canvas->render();
    };

    if (!chord_text.empty()) {
        const auto chord = KeyChord::parse(chord_text);
        if (!chord || !chord->valid())
            throw ToolFailure("Cannot parse key chord \"" + chord_text + "\" (examples: Ctrl+S, Shift+Tab, Escape, Return, Delete, F5, Left)");
        wxChar uni = WXK_NONE;
        if (chord->key == WXK_RETURN || chord->key == WXK_TAB || chord->key == WXK_ESCAPE || chord->key == WXK_BACK)
            uni = wxChar(chord->key);
        else if (chord->key >= 32 && chord->key < 127 && (chord->modifiers & (wxMOD_CONTROL | wxMOD_ALT)) == 0)
            uni = wxChar((chord->modifiers & wxMOD_SHIFT) ? chord->key : std::tolower(chord->key));
        deliver(chord->key, chord->modifiers, uni);
    }
    for (wxChar c : from_u8(text)) {
        const int code = (c >= 'a' && c <= 'z') ? int(c - 'a' + 'A') : (c < 127 ? int(c) : 0);
        deliver(code, 0, c);
    }
    return {{"sent", chord_text.empty() ? json(nullptr) : json(chord_text)}, {"typed", text}, {"to", short_class(target)}};
}

json UiInspector::mouse(GLCanvas3D& canvas, const json& g, const std::string& owner)
{
    const std::string action = g.value("action", "click");
    static const std::set<std::string> actions{"move", "down", "up", "click", "drag", "wheel"};
    if (actions.count(action) == 0)
        throw ToolFailure("Unknown mouse action " + action + " (move, down, up, click, drag, wheel)");
    const std::vector<std::string> mod_names = g.value("modifiers", std::vector<std::string>{});
    const int         mods   = parse_modifiers(mod_names);
    const std::string button = g.value("button", "left");
    const bool        right = button == "right", middle = button == "middle", left = !right && !middle;
    const wxEventType down  = left ? wxEVT_LEFT_DOWN : right ? wxEVT_RIGHT_DOWN : wxEVT_MIDDLE_DOWN;
    const wxEventType up    = left ? wxEVT_LEFT_UP : right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP;

    // Every point is resolved before the first event, so a bad one leaves no button held and no
    // stroke half drawn.
    const bool by_pixel = g.contains("x") || g.contains("y");
    const std::string check     = g.value("check", "surface");
    const double      tolerance = g.value("tolerance", 0.);
    json              pixels    = json::array();
    const bool        design    = canvas_name(&canvas) == std::string("design");
    auto pixel_of = [&](const json& p) {
        if (p.is_array() && p.size() != 2)
            throw ToolFailure("A pixel is [x, y]");
        if (!p.is_array() && design)
            throw ToolFailure("Points of the model name Prepare objects; on the Design view give x/y (cad_call query_topology gives faces)");
        const wxPoint px = p.is_array() ? wxPoint(p[0].get<int>(), p[1].get<int>()) : target_pixel(canvas, p, check, tolerance);
        pixels.push_back({px.x, px.y});
        return px;
    };
    if (by_pixel == g.contains("at"))
        throw ToolFailure(by_pixel ? "Give the position as x and y or as at, not both" : "Give the position as x and y, or as at");
    const wxPoint        from = by_pixel ? wxPoint(g.value("x", 0), g.value("y", 0)) : pixel_of(g["at"]);
    std::vector<wxPoint> stops; // where a drag goes after `from`
    if (by_pixel)
        pixels.push_back({from.x, from.y});
    if (action == "drag") {
        for (const json& p : g.value("path", json::array()))
            stops.push_back(pixel_of(p));
        if (g.contains("to"))
            stops.push_back(pixel_of(g["to"]));
        if (stops.empty())
            throw ToolFailure("drag needs to or path");
    }
    // A gesture that starts at a model point under an ImGui window or a notification would go to that
    // instead, so it is refused. A press held from before goes on to the scene wherever it moves, and
    // a pixel given as x and y is the client's to aim, ImGui panels included.
    const bool continues_held = m_held && m_held->canvas == &canvas && (action == "move" || action == "up");
    if (!by_pixel && !continues_held)
        if (const std::string cover = covered_by(canvas, from); !cover.empty())
            throw ToolFailure("The model point at pixel (" + std::to_string(from.x) + ", " + std::to_string(from.y) + ") is under " + cover +
                              ", which would take the " + (action == "move" ? "pointer" : action == "wheel" ? "wheel" : "click") +
                              "; turn or zoom the camera with set_view, or close it");
    json out{{"action", action}, {"canvas", canvas_name(&canvas)}, {"pixels", pixels}};

    // A press an earlier `down` left held: a `move` drags with it and an `up` releases it; any other
    // gesture releases it first.
    if (m_held && (m_held->canvas != &canvas || (action != "move" && action != "up")))
        release_held(nullptr);
    if (m_held) {
        const HeldGesture h = *m_held;
        CanvasMouse       m(canvas, h.mods, true);
        if (action == "move") {
            m.keep_held();
            m_held->pos = from;
            m.event(wxEVT_MOTION, from, h.left, h.right, h.middle);
            out["held"] = true;
        } else {
            m_held.reset();
            m.event(h.left ? wxEVT_LEFT_UP : h.right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP, from, false, false, false);
        }
        return out;
    }

    if (action == "click") {
        ClickOptions o;
        o.button       = button;
        o.double_click = g.value("double", false);
        o.modifiers    = mod_names;
        canvas_click(canvas, from, o);
        return out;
    }
    if (mods != wxMOD_NONE && (action == "down" || action == "drag"))
        hover(canvas, from);
    CanvasMouse m(canvas, mods);
    if (action == "move") {
        m.event(wxEVT_MOTION, from, false, false, false);
    } else if (action == "down") {
        m.keep_held();
        m_held = HeldGesture{&canvas, from, mods, left, right, middle, owner};
        m.event(wxEVT_MOTION, from, false, false, false);
        m.event(down, from, left, right, middle);
        out["held"] = true;
    } else if (action == "up") {
        m.event(up, from, false, false, false);
    } else if (action == "drag") {
        const int steps = std::clamp(g.value("steps", 10), 1, 200);
        m.event(wxEVT_MOTION, from, false, false, false);
        m.event(down, from, left, right, middle);
        wxPoint at = from;
        for (const wxPoint& to : stops) {
            for (int i = 1; i <= steps; ++i)
                m.event(wxEVT_MOTION, wxPoint(at.x + (to.x - at.x) * i / steps, at.y + (to.y - at.y) * i / steps), left, right, middle);
            at = to;
        }
        m.event(up, at, false, false, false);
    } else { // wheel
        m.event(wxEVT_MOTION, from, false, false, false);
        m.event(wxEVT_MOUSEWHEEL, from, false, false, false, g.value("delta", 1) * 120);
    }
    return out;
}

void UiInspector::release_held(const std::string* owner)
{
    if (!m_held || (owner != nullptr && m_held->owner != *owner))
        return;
    const HeldGesture h = *m_held;
    m_held.reset();
    if (!is_live_canvas(h.canvas)) {
        SyntheticInput::clear();
        return;
    }
    CanvasMouse m(*h.canvas, h.mods, true);
    m.event(h.left ? wxEVT_LEFT_UP : h.right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP, h.pos, false, false, false);
}

// ---- popup and context menus ------------------------------------------------------------------

void UiInspector::capture_menus(bool on)
{
    if (on == m_capturing_menus)
        return;
    m_capturing_menus = on;
    SyntheticInput::set_menu_capture(on ? std::function<bool(wxMenu&, wxWindow&)>([this](wxMenu& menu, wxWindow& owner) { return on_menu(menu, owner); }) :
                                          nullptr);
    if (wxTheApp == nullptr)
        return;
    if (on)
        wxTheApp->Bind(wxEVT_IDLE, &UiInspector::on_idle, this);
    else
        wxTheApp->Unbind(wxEVT_IDLE, &UiInspector::on_idle, this);
}

void UiInspector::ui_activity(const std::string& tool, bool running)
{
    if (running) {
        wxEventLoopBase* const loop = wxEventLoopBase::GetActive();
        // The new window covers what an ended one in this loop still waited for.
        m_menu_windows.erase(std::remove_if(m_menu_windows.begin(), m_menu_windows.end(),
                                            [loop](const MenuWindow& w) { return !w.running && w.loop == loop; }),
                             m_menu_windows.end());
        m_menu_windows.push_back({loop, tool, true, {}});
        return;
    }
    // Closures end in the reverse order they start, so the newest one running is ending.
    for (auto it = m_menu_windows.rbegin(); it != m_menu_windows.rend(); ++it)
        if (it->running) {
            it->running = false;
            it->ended   = std::chrono::steady_clock::now();
            return;
        }
}

void UiInspector::on_idle(wxIdleEvent& event)
{
    event.Skip();
    // Idle comes once the queue is drained, so the events and CallAfter()s a call queued, and those
    // they queued, have run. Past kMenuWindowGrace the window is closed anyway (on_menu() ignores it
    // even before an idle comes), so that a person's menus show again while something keeps queuing
    // events.
    const bool queued = wxTheApp != nullptr && wxTheApp->HasPendingEvents();
    const auto now    = std::chrono::steady_clock::now();
    m_menu_windows.erase(std::remove_if(m_menu_windows.begin(), m_menu_windows.end(),
                                        [&](const MenuWindow& w) { return !w.running && (!queued || now - w.ended > kMenuWindowGrace); }),
                         m_menu_windows.end());
}

int UiInspector::arm_menu(const std::string& path)
{
    MenuCapture c;
    c.token = ++m_menu_tokens;
    c.path  = path;
    c.loop  = wxEventLoopBase::GetActive();
    m_menu_captures.push_back(std::move(c));
    return m_menu_tokens;
}

UiInspector::MenuCapture* UiInspector::menu_capture(int token)
{
    const auto it = std::find_if(m_menu_captures.begin(), m_menu_captures.end(), [token](const MenuCapture& c) { return c.token == token; });
    return it == m_menu_captures.end() ? nullptr : &*it;
}

json UiInspector::disarm_menu(int token)
{
    MenuCapture* found = menu_capture(token);
    if (found == nullptr)
        return json::object();
    const MenuCapture c = std::move(*found);
    drop_menu(token);
    if (!c.error.empty())
        throw ToolFailure(c.error, c.details);
    if (!c.fired) {
        if (!c.path.empty())
            throw ToolFailure("The input opened no menu, so \"" + c.path +
                              "\" was not run. A right click shows none on empty space while objects are selected, in the "
                              "Preview, or while a gizmo is open.");
        return json::object();
    }
    json out = c.result;
    if (c.closed > 0)
        out["menus_closed"] = c.closed;
    return out;
}

void UiInspector::drop_menu(int token)
{
    m_menu_captures.erase(std::remove_if(m_menu_captures.begin(), m_menu_captures.end(), [token](const MenuCapture& c) { return c.token == token; }),
                          m_menu_captures.end());
}

json UiInspector::closed_menus() const
{
    json       out = json::array();
    const auto now = std::chrono::steady_clock::now();
    for (const ClosedMenu& m : m_closed_menus) {
        json entry{{"tool", m.tool}, {"ago_s", std::chrono::duration_cast<std::chrono::seconds>(now - m.at).count()}};
        entry.update(m.listing);
        out.push_back(std::move(entry));
    }
    return out;
}

bool UiInspector::on_menu(wxMenu& menu, wxWindow& owner)
{
    wxEventLoopBase* const loop   = wxEventLoopBase::GetActive();
    const auto             now    = std::chrono::steady_clock::now();
    const auto             window = std::find_if(m_menu_windows.rbegin(), m_menu_windows.rend(), [&](const MenuWindow& w) {
        return w.loop == loop && (w.running || now - w.ended <= kMenuWindowGrace);
    });
    if (window == m_menu_windows.rend())
        return false; // no call is acting in this loop: a person's menu
    const std::string tool    = window->tool;
    const auto        capture = std::find_if(m_menu_captures.rbegin(), m_menu_captures.rend(), [loop](const MenuCapture& c) { return c.loop == loop; });

    // What wxWindowBase::PopupMenu() does before it shows a menu: the menu's wxEVT_UPDATE_UI handlers,
    // which find them through the invoking window, set each item's state as a person would see it.
    wxMenuInvokingWindowSetter invoker(menu, &owner);
    menu.UpdateUI();
    const std::vector<MenuEntry> entries = menu_entries(menu);
    if (capture == m_menu_captures.rend() || capture->fired) {
        if (capture != m_menu_captures.rend())
            ++capture->closed;
        m_closed_menus.push_back({tool, std::chrono::steady_clock::now(), menu_listing(entries)});
        if (m_closed_menus.size() > 3)
            m_closed_menus.pop_front();
        return true;
    }
    capture->fired  = true;
    capture->result = {{"popup_menu", menu_listing(entries)}};
    if (capture->path.empty())
        return true;
    const MenuEntry* entry = find_menu_entry(entries, capture->path, current_marker(), capture->error, capture->details);
    if (entry == nullptr)
        return true;
    capture->result = {{"ran", entry->path}};
    const int         token = capture->token, id = entry->id;
    const std::string ran   = entry->path;
    // As a pick from a shown menu: a check item toggles and a radio item is checked before the event.
    wxMenu*     holder = nullptr;
    wxMenuItem* item   = menu.FindItem(id, &holder);
    int         checked = -1;
    if (item->IsCheckable()) {
        item->Check(item->GetKind() == wxITEM_RADIO || !item->IsChecked());
        checked = item->IsChecked() ? 1 : 0;
    }
    // The command may open a dialog whose loop runs this call's later closures, which end the
    // capture, so it is looked up again afterwards instead of being held.
    if (!holder->SendEvent(id, checked))
        if (MenuCapture* c = menu_capture(token)) {
            c->result = json();
            c->error  = "Nothing handled \"" + ran + "\"";
        }
    return true;
}

std::vector<std::string> imgui_display_labels(const std::vector<ImGuiWrapper::RecordedItem>& items,
                                              const std::vector<ImGuiWrapper::RecordedText>& texts)
{
    std::vector<std::string> out(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        out[i] = clean_imgui_label(items[i].label);
        if (!out[i].empty() || !is_imgui_field(items[i].kind))
            continue;
        // The nearest text left of the field whose middle is on the field's row...
        const ImGuiWrapper::RecordedItem& field = items[i];
        const ImGuiWrapper::RecordedText* best  = nullptr;
        for (const ImGuiWrapper::RecordedText& text : texts) {
            const float cy = text.y + text.h / 2.f;
            if (text.window != field.window || text.x + text.w > field.x + 1.f || cy < field.y || cy > field.y + field.h)
                continue;
            if (best == nullptr || text.x + text.w > best->x + best->w)
                best = &text;
        }
        if (best == nullptr)
            continue;
        // ...unless another field sits between them: the text labels the first field after it.
        const bool first = std::none_of(items.begin(), items.end(), [&](const ImGuiWrapper::RecordedItem& other) {
            const float cy = other.y + other.h / 2.f;
            return &other != &field && other.window == field.window && is_imgui_field(other.kind) && other.x >= best->x + best->w - 1.f &&
                   other.x < field.x && cy >= field.y && cy <= field.y + field.h;
        });
        if (first)
            out[i] = clean_imgui_label(best->text);
    }
    return out;
}

std::string imgui_item_name(const ImGuiWrapper::RecordedItem& item)
{
    return item.name.empty() ? imgui_label_name(item.label) : item.name;
}

int imgui_twins(const std::vector<ImGuiWrapper::RecordedItem>& items, const std::string& name, const std::string& window)
{
    if (name.empty())
        return 0;
    return int(std::count_if(items.begin(), items.end(),
                             [&](const ImGuiWrapper::RecordedItem& i) { return i.window == window && imgui_item_name(i) == name; }));
}

std::optional<std::size_t> imgui_find_item(const std::vector<ImGuiWrapper::RecordedItem>& items,
                                           unsigned                                        id,
                                           const std::string&                              name,
                                           const std::string&                              window,
                                           int                                             twins)
{
    const int same_name = imgui_twins(items, name, window);
    if (twins > 0 && same_name != twins && std::max(twins, same_name) > 1)
        return std::nullopt;
    std::optional<std::size_t> named;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == id)
            return i;
        if (!name.empty() && items[i].window == window && imgui_item_name(items[i]) == name)
            named = i;
    }
    return same_name == 1 ? named : std::nullopt;
}

bool imgui_value_matches(const std::string& shown, const json& wanted)
{
    const std::string text = wanted.is_string() ? wanted.get<std::string>() : wanted.dump();
    if (shown == text)
        return true;
    // Numbers: the shown one printed with some decimals, and maybe a unit after it.
    char*        end   = nullptr;
    const double want  = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0')
        return false;
    const char*  begin = shown.c_str();
    const double got   = std::strtod(begin, &end);
    if (end == begin)
        return false;
    const char* dot      = std::find(begin, static_cast<const char*>(end), '.');
    const int   decimals = dot == end ? 0 : int(end - dot - 1);
    return std::abs(got - want) <= 0.5 * std::pow(10., -decimals) + 1e-9;
}

ImGuiWindow* imgui_window_at(const ImVec2& pos)
{
    // ImGui's own search (FindHoveredWindow()), run for a position the mouse is not at yet.
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (g == nullptr)
        return nullptr;
    const ImVec2 padding        = g->Style.TouchExtraPadding;
    const ImVec2 resize_padding = g->IO.ConfigWindowsResizeFromEdges ? g->WindowsHoverPadding : padding;
    for (int i = g->Windows.Size - 1; i >= 0; --i) {
        ImGuiWindow* w = g->Windows[i];
        if (!w->Active || w->Hidden || (w->Flags & ImGuiWindowFlags_NoMouseInputs) != 0)
            continue;
        const bool fixed = (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) != 0;
        ImRect     bb(w->OuterRectClipped);
        bb.Expand(fixed ? padding : resize_padding);
        if (!bb.Contains(pos))
            continue;
        if (w->HitTestHoleSize.x != 0) {
            const ImVec2 hole(w->Pos.x + float(w->HitTestHoleOffset.x), w->Pos.y + float(w->HitTestHoleOffset.y));
            if (ImRect(hole, ImVec2(hole.x + float(w->HitTestHoleSize.x), hole.y + float(w->HitTestHoleSize.y))).Contains(pos))
                continue;
        }
        return w;
    }
    return nullptr;
}

std::string imgui_window_description(const std::string& window, const std::vector<ImGuiWrapper::RecordedText>& texts)
{
    // NotificationManager names its windows "!!Ntfctn<id>"; their text tells which one it is.
    if (window.rfind("!!Ntfctn", 0) == 0) {
        for (const ImGuiWrapper::RecordedText& t : texts) {
            if (t.text.empty() || (t.window != window && t.window.rfind(window + "/", 0) != 0))
                continue;
            std::string text = t.text.substr(0, t.text.find('\n'));
            if (text.size() > 60) {
                std::size_t cut = 57; // not inside a UTF-8 sequence
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                    --cut;
                text = text.substr(0, cut) + "...";
            }
            return "the notification \"" + text + "\"";
        }
        return "a notification";
    }
    const std::string label = imgui_window_label(window);
    return label.empty() ? std::string("an ImGui window") : "the \"" + label + "\" window";
}

std::vector<SurfaceMesh> surface_meshes(const std::vector<GLVolume*>&             volumes,
                                        const GLVolume*                           target,
                                        const std::function<bool(const GLVolume*)>& own)
{
    std::vector<SurfaceMesh> meshes;
    for (const GLVolume* v : volumes) {
        if (v->mesh_raycaster == nullptr || !v->visible || (v->is_modifier && v != target) || (!v->is_active && !own(v)))
            continue;
        meshes.push_back({&v->mesh_raycaster->get_aabb_mesh(), v->world_matrix(), v == target, own(v),
                          "object " + std::to_string(v->object_idx()) + " instance " + std::to_string(v->instance_idx()) + " part " +
                              std::to_string(v->volume_idx())});
    }
    return meshes;
}

const std::vector<GLToolbarItem*>& toolbar_items(GLToolbar& toolbar, bool* changed)
{
    const bool any = toolbar.update_items_state();
    if (changed != nullptr)
        *changed = any;
    return toolbar.get_items();
}

}}} // namespace Slic3r::GUI::Mcp
