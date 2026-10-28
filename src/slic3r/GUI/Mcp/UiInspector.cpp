#include "UiInspector.hpp"

#include "McpService.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "slic3r/GUI/BBLTopbar.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/KeyChord.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/SideButton.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/SpinInput.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <imgui/imgui_internal.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/ctrlsub.h>
#include <wx/dataview.h>
#include <wx/dialog.h>
#include <wx/glcanvas.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/radiobut.h>
#include <wx/scrolwin.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/arrstr.h>
#include <wx/utils.h>
#include <wx/webview.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
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
    if (dynamic_cast<wxListBox*>(w) != nullptr || dynamic_cast<wxDataViewCtrl*>(w) != nullptr)
        return "list";
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

// Stable English gizmo names for refs and locators: the names Orca uses for the gizmos it names,
// and the enum's own for the rest.
std::string gizmo_name(GLGizmosManager::EType type)
{
    if (std::string name = get_name_from_gizmo_etype(type); !name.empty())
        return name;
    switch (type) {
    case GLGizmosManager::Svg: return "SVG";
    case GLGizmosManager::Measure: return "Measure";
    case GLGizmosManager::Assembly: return "Assembly";
    case GLGizmosManager::Simplify: return "Simplify";
    case GLGizmosManager::BrimEars: return "Brim Ears";
    default: return "Gizmo " + std::to_string(int(type));
    }
}

GLGizmosManager::EType gizmo_from_name(const std::string& name)
{
    for (int t = 0; t < int(GLGizmosManager::Undefined); ++t)
        if (lower(gizmo_name(GLGizmosManager::EType(t))) == lower(name))
            return GLGizmosManager::EType(t);
    return GLGizmosManager::Undefined;
}

// wx calls Command "Control" on macOS and the Control key "raw control"; elsewhere cmd/meta is the
// Windows/Super key.
void set_modifiers(wxKeyboardState& state, const std::vector<std::string>& mods)
{
    for (const std::string& m : mods) {
        const std::string l = lower(m);
        if (l == "ctrl" || l == "control")
#ifdef __APPLE__
            state.SetRawControlDown(true);
#else
            state.SetControlDown(true);
#endif
        else if (l == "shift")
            state.SetShiftDown(true);
        else if (l == "alt" || l == "option")
            state.SetAltDown(true);
        else if (l == "cmd" || l == "meta" || l == "super")
#ifdef __APPLE__
            state.SetControlDown(true);
#else
            state.SetMetaDown(true);
#endif
        else
            throw ToolFailure("Unknown modifier " + m + " (ctrl, shift, alt, cmd)");
    }
}

std::vector<int> modifier_keys(const std::vector<std::string>& mods)
{
    std::vector<int> keys;
    for (const std::string& m : mods) {
        const std::string l = lower(m);
        if (l == "ctrl" || l == "control")
#ifdef __APPLE__
            keys.push_back(WXK_RAW_CONTROL);
#else
            keys.push_back(WXK_CONTROL);
#endif
        else if (l == "shift")
            keys.push_back(WXK_SHIFT);
        else if (l == "alt" || l == "option")
            keys.push_back(WXK_ALT);
        else if (l == "cmd" || l == "meta" || l == "super")
#ifdef __APPLE__
            keys.push_back(WXK_CONTROL);
#else
            keys.push_back(WXK_WINDOWS_LEFT);
#endif
    }
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

wxMouseEvent mouse_event(wxEventType type, const wxPoint& pos, bool left, bool right, bool middle, const std::vector<std::string>& mods)
{
    wxMouseEvent evt(type);
    evt.SetPosition(pos);
    evt.SetLeftDown(left);
    evt.SetRightDown(right);
    evt.SetMiddleDown(middle);
    set_modifiers(evt, mods);
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

// A mouse click on a plain window at its centre: down, up, and the double-click or context-menu
// event a real click produces.
void click_window(wxWindow* w, const ClickOptions& o)
{
    const wxSize   size = w->GetClientSize();
    const wxPoint  pos(size.x / 2, size.y / 2);
    const bool     right = o.button == "right", middle = o.button == "middle", left = !right && !middle;
    const auto     down  = left ? wxEVT_LEFT_DOWN : right ? wxEVT_RIGHT_DOWN : wxEVT_MIDDLE_DOWN;
    const auto     up    = left ? wxEVT_LEFT_UP : right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP;
    wxMouseEvent   d     = mouse_event(down, pos, left, right, middle, o.modifiers);
    send(w, d);
    wxMouseEvent u = mouse_event(up, pos, false, false, false, o.modifiers);
    send(w, u);
    if (o.double_click && left) {
        wxMouseEvent dc = mouse_event(wxEVT_LEFT_DCLICK, pos, true, false, false, o.modifiers);
        send(w, dc);
        wxMouseEvent u2 = mouse_event(wxEVT_LEFT_UP, pos, false, false, false, o.modifiers);
        send(w, u2);
    }
    if (right) {
        wxContextMenuEvent menu(wxEVT_CONTEXT_MENU, w->GetId(), w->ClientToScreen(pos));
        send(w, menu);
    }
}

// Delivers synthesized mouse events to a 3D canvas, drawing a frame after each so ImGui sees the
// hover, the press and the release on separate frames, as with a real mouse.
class CanvasMouse
{
public:
    CanvasMouse(GLCanvas3D& canvas, std::vector<std::string> mods) : m_canvas(canvas), m_mods(std::move(mods))
    {
        for (int k : modifier_keys(m_mods))
            SyntheticInput::press_key(k);
    }
    ~CanvasMouse()
    {
        SyntheticInput::clear();
        m_canvas.set_as_dirty();
    }
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
    GLCanvas3D&              m_canvas;
    std::vector<std::string> m_mods;
};

void canvas_click(GLCanvas3D& canvas, const wxPoint& pos, const ClickOptions& o)
{
    CanvasMouse m(canvas, o.modifiers);
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

const char* canvas_name(GLCanvas3D* canvas)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return "";
    if (canvas == plater->get_view3D_canvas3D())
        return "3d";
    if (canvas == plater->get_preview_canvas3D())
        return "preview";
    if (canvas == plater->get_assmeble_canvas3D())
        return "assemble";
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
    if (name == "3d" || name.empty())
        return plater->get_view3D_canvas3D();
    return nullptr;
}

bool is_live_canvas(GLCanvas3D* canvas)
{
    const auto canvases = live_canvases();
    return canvas != nullptr && std::find(canvases.begin(), canvases.end(), canvas) != canvases.end();
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
        alive                    = [weak] { return weak.get() != nullptr; };
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
    case UiTarget::Kind::ImGuiItem: {
        kind = 'g';
        GLCanvas3D* canvas = target.canvas;
        const unsigned id = target.imgui_id;
        alive = [canvas, id] {
            if (!is_live_canvas(canvas) || wxGetApp().imgui() == nullptr)
                return false;
            for (const auto& item : wxGetApp().imgui()->recorded_items(canvas))
                if (item.id == id)
                    return true;
            return false;
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
    const std::string cname = canvas_name(&canvas);
    GLToolbar&        toolbar = canvas.get_main_toolbar();
    for (GLToolbarItem* item : toolbar.get_items()) {
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
    GLGizmosManager& gizmos = canvas.get_gizmos_manager();
    if (gizmos.is_enabled())
        for (int type = 0; type < int(GLGizmosManager::Undefined); ++type) {
            GLGizmoBase* g = gizmos.get_gizmo(GLGizmosManager::EType(type));
            if (g == nullptr || !g->is_selectable())
                continue;
            UiNode node;
            node.kind    = "gizmo";
            node.label   = gizmo_name(GLGizmosManager::EType(type));
            node.enabled = g->is_activable();
            node.checked = gizmos.get_current_type() == GLGizmosManager::EType(type) ? 1 : 0;
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
    if (!o.include_imgui || wxGetApp().imgui() == nullptr)
        return;
    for (const ImGuiWrapper::RecordedItem& item : wxGetApp().imgui()->recorded_items(&canvas)) {
        const std::string label = clean_imgui_label(item.label);
        if (label.empty())
            continue;
        const wxPoint at = imgui_to_canvas(canvas, item.x, item.y);
        const wxPoint br = imgui_to_canvas(canvas, item.x + item.w, item.y + item.h);
        UiNode        node;
        node.kind  = "imgui";
        node.label = label;
        if (item.flags & ImGuiItemStatusFlags_Checkable)
            node.checked = (item.flags & ImGuiItemStatusFlags_Checked) ? 1 : 0;
        node.cls   = clean_imgui_label(item.window);
        node.depth = depth;
        node.x = at.x, node.y = at.y, node.w = br.x - at.x, node.h = br.y - at.y;
        UiTarget t;
        t.kind         = UiTarget::Kind::ImGuiItem;
        t.window       = canvas.get_wxglcanvas();
        t.canvas       = &canvas;
        t.imgui_id     = item.id;
        t.imgui_label  = label;
        t.imgui_window = item.window;
        t.x = item.x, t.y = item.y, t.w = item.w, t.h = item.h;
        t.locator      = {{"imgui", label}, {"window", clean_imgui_label(item.window)}, {"canvas", cname}};
        push(std::move(node), std::move(t), nodes, omitted, o);
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
    const bool interesting  = top || (!kind.empty() && (kind != "label" || !label.empty())) || !name.empty();
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
        const wxPoint pos  = top ? w->GetScreenPosition() : client_pos_in_tlw(w);
        const wxSize  size = w->GetSize();
        node.x = pos.x, node.y = pos.y, node.w = size.x, node.h = size.y;
        UiTarget t;
        t.kind   = UiTarget::Kind::Window;
        t.window = w;
        wxWindow* tlw = wxGetTopLevelParent(w);
        t.locator = {{"class", node.cls}};
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
        for (GLCanvas3D* canvas : live_canvases())
            if (canvas->get_wxglcanvas() == w)
                add_canvas_items(*canvas, child_depth, nodes, omitted, o);
    // Web views and canvases are listed by their own means; Orca's composite inputs are one
    // control to the agent, not their inner text field and buttons.
    if (kind == "webview" || kind == "canvas" || kind == "text" || kind == "spin" || kind == "combo")
        return;
    for (wxWindow* child : w->GetChildren())
        if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr)
            walk(child, child_depth, nodes, omitted, o);
}

json UiInspector::snapshot(const SnapshotOptions& o)
{
    std::vector<UiNode> nodes;
    std::size_t         omitted = 0;
    std::vector<wxWindow*> roots;
    if (o.root == "dialog") {
        if (wxDialog* d = m_service.modals().top())
            roots.push_back(d);
        else
            throw ToolFailure("No modal dialog is open");
    } else if (o.root == "main") {
        if (wxGetApp().mainframe == nullptr)
            throw ToolFailure("The main window is not up yet");
        roots.push_back(wxGetApp().mainframe);
    } else if (o.root == "all" || o.root.empty()) {
        for (wxTopLevelWindow* tlw : top_level_windows(m_service))
            roots.push_back(tlw);
    } else {
        const UiTarget t = resolve(json(o.root));
        if (t.kind != UiTarget::Kind::Window || t.window.get() == nullptr)
            throw ToolFailure("root must be all, main, dialog or the ref of a window");
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
        if (out.kind == UiTarget::Kind::ImGuiItem) // the item may have moved since the snapshot
            for (const auto& item : wxGetApp().imgui()->recorded_items(out.canvas))
                if (item.id == out.imgui_id)
                    out.x = item.x, out.y = item.y, out.w = item.w, out.h = item.h;
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
    if (target.contains("gltoolbar") || target.contains("gizmo") || target.contains("imgui")) {
        t.canvas = canvas_by_name(target.value("canvas", target.contains("imgui") ? "" : "3d"));
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
        const std::string label  = target["imgui"].get<std::string>();
        const std::string window = target.value("window", "");
        std::vector<GLCanvas3D*> canvases = target.contains("canvas") ? std::vector<GLCanvas3D*>{t.canvas} : live_canvases();
        for (GLCanvas3D* c : canvases)
            for (const auto& item : wxGetApp().imgui()->recorded_items(c))
                if (clean_imgui_label(item.label) == label && (window.empty() || clean_imgui_label(item.window) == window)) {
                    t.kind         = UiTarget::Kind::ImGuiItem;
                    t.canvas       = c;
                    t.window       = c->get_wxglcanvas();
                    t.imgui_id     = item.id;
                    t.imgui_label  = label;
                    t.imgui_window = item.window;
                    t.x = item.x, t.y = item.y, t.w = item.w, t.h = item.h;
                    return t;
                }
        throw ToolFailure("No ImGui item \"" + label + "\" in the last frame; show its panel first");
    }

    // A wx window by option key, class, label or name, optionally within a titled top-level window.
    const std::string option = target.value("option", "");
    const std::string cls    = target.value("class", "");
    const std::string label  = target.value("label", "");
    const std::string name   = target.value("name", "");
    const std::string within = target.value("within", "");
    const int         index  = target.value("index", -1);
    if (option.empty() && cls.empty() && label.empty() && name.empty())
        throw ToolFailure("A locator needs option, label, name or class (or menu, gltoolbar, gizmo, imgui)");
    std::vector<wxWindow*> matches;
    std::function<void(wxWindow*)> visit = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return;
        if ((option.empty() || option_key_of(w) == option) && (cls.empty() || short_class(w) == cls) &&
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
    if (matches.empty())
        throw ToolFailure("Nothing matches " + target.dump() + "; take a ui_snapshot");
    if (matches.size() > 1 && index < 0) {
        // A row's label and the field it names share the text; the field is what an agent acts on.
        std::vector<wxWindow*> controls;
        for (wxWindow* w : matches) {
            const std::string k = kind_of(w);
            if (k != "label" && k != "control" && k != "window" && k != "dialog" && !k.empty())
                controls.push_back(w);
        }
        if (controls.size() == 1)
            matches = controls;
    }
    if (matches.size() > 1 && index < 0)
        throw ToolFailure(std::to_string(matches.size()) + " elements match " + target.dump() + "; add \"index\" (0-based) or use a ref");
    const std::size_t i = index < 0 ? 0 : std::size_t(index);
    if (i >= matches.size())
        throw ToolFailure("index " + std::to_string(i) + " is past the " + std::to_string(matches.size()) + " matches");
    t.kind   = UiTarget::Kind::Window;
    t.window = matches[i];
    return t;
}

GLCanvas3D* UiInspector::canvas_of(const UiTarget* target) const
{
    if (target != nullptr && target->canvas != nullptr && is_live_canvas(target->canvas))
        return target->canvas;
    if (target != nullptr && target->window.get() != nullptr)
        for (GLCanvas3D* c : live_canvases())
            if (c->get_wxglcanvas() == target->window.get())
                return c;
    if (target == nullptr)
        if (Plater* plater = wxGetApp().plater())
            return plater->get_current_canvas3D();
    return nullptr;
}

// ---- actions ----------------------------------------------------------------------------------

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
        GLToolbar&     tb   = t.canvas->get_main_toolbar();
        GLToolbarItem* item = tb.get_item(t.name);
        if (item == nullptr)
            throw ToolFailure("The toolbar item is gone");
        if (item->is_disabled())
            throw ToolFailure("The toolbar item " + t.name + " is disabled");
        // The toolbar ignores actions while another togglable item is pressed.
        for (GLToolbarItem* other : tb.get_items())
            if (other != nullptr && other != item && other->is_pressed() && (other->is_left_toggable() || other->is_right_toggable()))
                throw ToolFailure("Close " + other->get_name() + " first: the toolbar ignores clicks while it is active");
        const bool was = item->is_pressed();
        tb.force_left_action(tb.get_item_id(t.name), *t.canvas);
        t.canvas->set_as_dirty();
        t.canvas->render();
        if (item->is_left_toggable() && item->is_pressed() == was)
            throw ToolFailure("The toolbar did not react to " + t.name);
        return {{"clicked", t.name}, {"pressed", item->is_pressed()}};
    }
    case UiTarget::Kind::Gizmo: {
        GLGizmosManager& gizmos = t.canvas->get_gizmos_manager();
        GLGizmoBase*     g      = gizmos.get_gizmo(GLGizmosManager::EType(t.gizmo));
        if (g == nullptr || !g->is_activable())
            throw ToolFailure("The " + t.name + " gizmo cannot be opened now (select an object first?)");
        gizmos.open_gizmo(GLGizmosManager::EType(t.gizmo));
        t.canvas->set_as_dirty();
        t.canvas->render();
        return {{"gizmo", t.name}, {"active", gizmos.get_current_type() == GLGizmosManager::EType(t.gizmo)}};
    }
    case UiTarget::Kind::ImGuiItem: {
        const wxPoint pos = imgui_to_canvas(*t.canvas, t.x + t.w / 2.f, t.y + t.h / 2.f);
        canvas_click(*t.canvas, pos, o);
        return {{"clicked", t.imgui_label}, {"at", {pos.x, pos.y}}};
    }
    case UiTarget::Kind::Window: break;
    }

    wxWindow* w = t.window.get();
    if (w == nullptr)
        throw ToolFailure("The element is gone; take a new ui_snapshot");
    if (!w->IsEnabled())
        throw ToolFailure("The element is disabled");
    scroll_into_view(w);
    if (o.at_point || dynamic_cast<wxGLCanvas*>(w) != nullptr) {
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
    if (t.kind == UiTarget::Kind::ImGuiItem) {
        // Focus the field, select what is there, type the value.
        click(t, ClickOptions{});
        key(&t, "Ctrl+A", "");
        key(&t, "", value.is_string() ? value.get<std::string>() : value.dump());
        if (submit)
            key(&t, "Return", "");
        return {{"typed", value}};
    }
    if (t.kind != UiTarget::Kind::Window || t.window.get() == nullptr)
        throw ToolFailure("ui_input works on controls and ImGui fields");
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

wxWindow* UiInspector::key_target(const UiTarget* t) const
{
    if (t != nullptr)
        return t->window.get();
    wxWindow* focus = wxWindow::FindFocus();
    if (wxDialog* top = m_service.modals().top())
        return focus != nullptr && wxGetTopLevelParent(focus) == top ? focus : top;
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
    GLCanvas3D* canvas = nullptr;
    for (GLCanvas3D* c : live_canvases())
        if (c->get_wxglcanvas() == target)
            canvas = c;
    wxWindow* tlw = wxGetTopLevelParent(target);

    auto deliver = [&](int key_code, int modifiers, wxChar uni) {
        auto make = [&](wxEventType type) {
            wxKeyEvent evt(type);
            evt.m_keyCode = key_code;
            evt.m_uniChar = uni;
            evt.SetControlDown((modifiers & wxMOD_CONTROL) != 0);
            evt.SetShiftDown((modifiers & wxMOD_SHIFT) != 0);
            evt.SetAltDown((modifiers & wxMOD_ALT) != 0);
            evt.SetMetaDown((modifiers & wxMOD_META) != 0);
#ifdef __APPLE__
            evt.SetRawControlDown((modifiers & wxMOD_RAW_CONTROL) != 0);
#endif
            evt.SetEventObject(target);
            return evt;
        };
        std::vector<int> held;
        if (modifiers & wxMOD_CONTROL)
            held.push_back(WXK_CONTROL);
        if (modifiers & wxMOD_SHIFT)
            held.push_back(WXK_SHIFT);
        if (modifiers & wxMOD_ALT)
            held.push_back(WXK_ALT);
        for (int k : held)
            SyntheticInput::press_key(k);
        SyntheticInput::press_key(key_code);
        bool handled = false;
        if (tlw != nullptr && key_code != 0) {
            wxKeyEvent hook = make(wxEVT_CHAR_HOOK);
            handled         = tlw->GetEventHandler()->ProcessEvent(hook);
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
        SyntheticInput::clear();
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

json UiInspector::mouse(GLCanvas3D& canvas, const json& g)
{
    const std::string action = g.value("action", "click");
    const std::vector<std::string> mods = g.value("modifiers", std::vector<std::string>{});
    const std::string button = g.value("button", "left");
    const bool right = button == "right", middle = button == "middle", left = !right && !middle;
    const wxPoint from(g.value("x", 0), g.value("y", 0));
    CanvasMouse m(canvas, mods);
    if (action == "move") {
        m.event(wxEVT_MOTION, from, false, false, false);
    } else if (action == "down") {
        m.event(wxEVT_MOTION, from, false, false, false);
        m.event(left ? wxEVT_LEFT_DOWN : right ? wxEVT_RIGHT_DOWN : wxEVT_MIDDLE_DOWN, from, left, right, middle);
    } else if (action == "up") {
        m.event(left ? wxEVT_LEFT_UP : right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP, from, false, false, false);
    } else if (action == "click") {
        ClickOptions o;
        o.button       = button;
        o.double_click = g.value("double", false);
        o.modifiers    = mods;
        canvas_click(canvas, from, o);
    } else if (action == "drag") {
        if (!g.contains("to") || !g["to"].is_array() || g["to"].size() != 2)
            throw ToolFailure("drag needs to: [x, y]");
        const wxPoint to(g["to"][0].get<int>(), g["to"][1].get<int>());
        const int     steps = std::clamp(g.value("steps", 10), 1, 200);
        m.event(wxEVT_MOTION, from, false, false, false);
        m.event(left ? wxEVT_LEFT_DOWN : right ? wxEVT_RIGHT_DOWN : wxEVT_MIDDLE_DOWN, from, left, right, middle);
        for (int i = 1; i <= steps; ++i) {
            const wxPoint p(from.x + (to.x - from.x) * i / steps, from.y + (to.y - from.y) * i / steps);
            m.event(wxEVT_MOTION, p, left, right, middle);
        }
        m.event(left ? wxEVT_LEFT_UP : right ? wxEVT_RIGHT_UP : wxEVT_MIDDLE_UP, to, false, false, false);
    } else if (action == "wheel") {
        const int clicks = g.value("delta", 1);
        m.event(wxEVT_MOTION, from, false, false, false);
        m.event(wxEVT_MOUSEWHEEL, from, false, false, false, clicks * 120);
    } else {
        throw ToolFailure("Unknown mouse action " + action + " (move, down, up, click, drag, wheel)");
    }
    return {{"action", action}, {"canvas", canvas_name(&canvas)}};
}

}}} // namespace Slic3r::GUI::Mcp
