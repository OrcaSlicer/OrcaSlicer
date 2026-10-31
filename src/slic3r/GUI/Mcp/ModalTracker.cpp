#include "ModalTracker.hpp"

#include "McpTypes.hpp"
#include "WebPages.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/html/htmlwin.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <wx/vector.h>
#include <wx/window.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <tuple>
#include <typeinfo>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <cwchar>
#include <windows.h>
#include <commctrl.h>
#endif

#ifdef __WXGTK__
#include <wx/stockitem.h>

#include <gtk/gtk.h>
#endif

// Itanium-ABI demangling; clang-cl defines __clang__ but follows the MSVC ABI and has no cxxabi.h.
#ifndef _MSC_VER
#include <cxxabi.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

constexpr std::size_t kMaxMessage = 2000;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// "&Yes" -> "yes": mnemonics and case don't matter when matching a label.
std::string normalize_label(const wxString& label)
{
    wxString l = wxStripMenuCodes(label);
    l.Trim().Trim(false);
    return lower(into_u8(l));
}

void collect(wxWindow* w, std::string& message, json& buttons)
{
    for (wxWindow* child : w->GetChildren()) {
        if (!child->IsShown())
            continue;
        if (auto* html = dynamic_cast<wxHtmlWindow*>(child)) {
            message += into_u8(html->ToText()) + "\n";
            continue;
        }
        if (dynamic_cast<wxButton*>(child) != nullptr || dynamic_cast<::Button*>(child) != nullptr) {
            const wxString label = wxStripMenuCodes(child->GetLabel());
            if (!label.empty())
                buttons.push_back({{"id", child->GetId()}, {"label", into_u8(label)}});
            continue;
        }
        if (auto* text = dynamic_cast<wxStaticText*>(child)) {
            const wxString t = text->GetLabel();
            if (!t.empty())
                message += into_u8(t) + "\n";
        }
        collect(child, message, buttons);
    }
}

} // namespace

std::string class_name(const wxWindow* window)
{
    if (window == nullptr)
        return {};
    const char* raw = typeid(*window).name();
#ifndef _MSC_VER
    int   status    = 0;
    char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    std::string out = status == 0 && demangled != nullptr ? demangled : raw;
    std::free(demangled);
    return out;
#else
    std::string out = raw;
    for (const char* prefix : {"class ", "struct "})
        if (out.rfind(prefix, 0) == 0)
            out.erase(0, std::char_traits<char>::length(prefix));
    return out;
#endif
}

bool is_picker(wxDialog* dialog)
{
    return dynamic_cast<wxFileDialogBase*>(dialog) != nullptr || dynamic_cast<wxDirDialogBase*>(dialog) != nullptr;
}

bool is_native_dialog(wxDialog* dialog)
{
    return dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr || dynamic_cast<wxFileDialogBase*>(dialog) != nullptr ||
           dynamic_cast<wxDirDialogBase*>(dialog) != nullptr;
}

namespace {

// The label a message box shows on its button `id`, without a mnemonic: wx's "&", or on GTK, where wx
// keeps the labels in GTK's own form, its "_" ("_Yes", "__" for an underscore). wxGTK's default labels
// before GTK 4 are stock ids ("gtk-yes"), which GTK shows as the stock item's label, as wx's own is.
wxString shown_label(int id, const wxString& label)
{
#ifdef __WXGTK__
    if (label.StartsWith("gtk-"))
        return wxGetStockLabel(id, wxSTOCK_NOFLAGS);
    wxString out;
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '_') {
            if (i + 1 < label.size() && label[i + 1] == '_')
                out += label[++i];
            continue;
        }
        out += label[i];
    }
    return out;
#else
    (void) id;
    return wxStripMenuCodes(label);
#endif
}

} // namespace

json message_box_buttons(wxDialog* dialog)
{
    json out = json::array();
#if defined(_WIN32) || defined(__WXGTK__)
    const auto* box = dynamic_cast<wxMessageDialogBase*>(dialog);
    if (box == nullptr)
        return out;
    auto add = [&out](int id, const wxString& label) { out.push_back({{"id", id}, {"label", into_u8(shown_label(id, label))}}); };
    // The buttons wx gives the box for its style. A Help button is left out: on Windows it shows help
    // without closing the box.
    const long style = box->GetMessageDialogStyle();
    if (style & wxYES_NO) {
        add(wxID_YES, box->GetYesLabel());
        add(wxID_NO, box->GetNoLabel());
    } else
        add(wxID_OK, box->GetOKLabel());
    if (style & wxCANCEL)
        add(wxID_CANCEL, box->GetCancelLabel());
#else
    // On macOS wx runs none of its events while an alert is open, so no call could reach one to press it.
    (void) dialog;
#endif
    return out;
}

namespace {

bool has_box_button(wxDialog* dialog, int id)
{
    const json buttons = message_box_buttons(dialog);
    return std::any_of(buttons.begin(), buttons.end(), [id](const json& b) { return b["id"].get<int>() == id; });
}

#ifdef _WIN32
// Whether `hwnd` is a shown, enabled dialog-class window titled `title`: a box under another modal one
// is disabled.
bool is_box_window(HWND hwnd, const std::wstring& title)
{
    wchar_t cls[16];
    if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd) || GetClassNameW(hwnd, cls, 16) == 0 || std::wcscmp(cls, L"#32770") != 0)
        return false;
    std::wstring text(std::size_t(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(std::size_t(std::max(0, GetWindowTextW(hwnd, text.data(), int(text.size())))));
    return text == title;
}

// The message box or task dialog wx shows for `box`, a window of this thread with the box's caption:
// the thread's active window when it is one, which of two boxes with one caption is the one shown
// last, else the first such window.
HWND shown_box(const wxMessageDialogBase& box)
{
    struct Search
    {
        std::wstring title;
        HWND         found = nullptr;
    } search{box.GetTitle().ToStdWstring()};
    if (HWND active = GetActiveWindow(); active != nullptr && is_box_window(active, search.title))
        return active;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(param);
            if (!is_box_window(hwnd, s.title))
                return TRUE;
            s.found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}
#endif

// Presses the button `id` (one of message_box_buttons()) of the open native message box `dialog`.
// The press is queued to the box, which closes once its own loop runs again, after the caller returns.
bool press_message_box(wxDialog* dialog, int id)
{
    if (!has_box_button(dialog, id))
        return false;
#ifdef _WIN32
    const auto& box  = dynamic_cast<const wxMessageDialogBase&>(*dialog);
    const HWND  hwnd = shown_box(box);
    if (hwnd == nullptr)
        return false;
    int win = id == wxID_YES ? IDYES : id == wxID_NO ? IDNO : id == wxID_CANCEL ? IDCANCEL : IDOK;
    // A message box's buttons are its children, which tell it of a click by WM_COMMAND.
    if (HWND button = GetDlgItem(hwnd, win))
        return PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(win, BN_CLICKED), reinterpret_cast<LPARAM>(button)) != 0;
    // A task dialog, which wx shows where Windows has one, takes a click by message. wx gives a box
    // with only OK a Cancel button labelled OK, so that Escape closes it.
    if (id == wxID_OK && (box.GetMessageDialogStyle() & (wxYES_NO | wxCANCEL)) == 0)
        win = IDCANCEL;
    return PostMessageW(hwnd, TDM_CLICK_BUTTON, WPARAM(win), 0) != 0;
#elif defined(__WXGTK__)
    auto* widget = static_cast<GtkWidget*>(dialog->GetHandle());
    if (widget == nullptr || !GTK_IS_DIALOG(widget))
        return false;
    // gtk_dialog_run() returns this response once its loop runs again, and wx maps it back to the id.
    gtk_dialog_response(GTK_DIALOG(widget), id == wxID_YES    ? GTK_RESPONSE_YES :
                                            id == wxID_NO     ? GTK_RESPONSE_NO :
                                            id == wxID_CANCEL ? GTK_RESPONSE_CANCEL :
                                                                GTK_RESPONSE_OK);
    return true;
#else
    return false;
#endif
}

} // namespace

json describe_dialog(wxDialog* dialog)
{
    json out{{"title", into_u8(dialog->GetTitle())}, {"class", class_name(dialog)}, {"native", is_native_dialog(dialog)}};
    std::string message;
    json        buttons = json::array();
    if (auto* md = dynamic_cast<wxMessageDialogBase*>(dialog)) {
        message = into_u8(md->GetMessage());
        if (!md->GetExtendedMessage().empty())
            message += "\n" + into_u8(md->GetExtendedMessage());
        buttons = message_box_buttons(dialog);
    } else if (auto* fd = dynamic_cast<wxFileDialogBase*>(dialog)) {
        message = into_u8(fd->GetMessage());
    } else if (auto* dd = dynamic_cast<wxDirDialogBase*>(dialog)) {
        message = into_u8(dd->GetMessage());
    } else {
        collect(dialog, message, buttons);
    }
    if (message.size() > kMaxMessage)
        message = message.substr(0, kMaxMessage) + "...";
    while (!message.empty() && (message.back() == '\n' || message.back() == ' '))
        message.pop_back();
    out["message"] = message;
    out["buttons"] = std::move(buttons);
    if (!is_native_dialog(dialog) && web_view_in(dialog, false) != nullptr)
        out["web"] = true;
    return out;
}

int dialog_button_by_label(wxDialog* dialog, const std::string& label)
{
    if (dialog == nullptr)
        return wxID_NONE;
    const std::string name    = lower(label);
    json              buttons = json::array();
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr)
        buttons = message_box_buttons(dialog);
    else if (!is_native_dialog(dialog)) {
        std::string message;
        collect(dialog, message, buttons);
    }
    for (const json& b : buttons)
        if (normalize_label(from_u8(b["label"].get<std::string>())) == name)
            return b["id"].get<int>();
    return wxID_NONE;
}

int standard_dialog_button(const std::string& button)
{
    const std::string name = lower(button);
    static const std::pair<const char*, int> standard[] = {{"yes", wxID_YES},       {"no", wxID_NO},       {"ok", wxID_OK},
                                                           {"cancel", wxID_CANCEL}, {"close", wxID_CLOSE}, {"apply", wxID_APPLY}};
    for (const auto& [key, id] : standard)
        if (name == key)
            return id;
    if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) || c == '-'; }))
        return std::atoi(name.c_str());
    return wxID_NONE;
}

int resolve_dialog_button(wxDialog* dialog, const std::string& button)
{
    // A button whose label matches wins, so "Don't save" or "Keep" reach the dialog's own ids.
    if (const int id = dialog_button_by_label(dialog, button); id != wxID_NONE)
        return id;
    return standard_dialog_button(button);
}

namespace {

wxWindow* dialog_button(wxDialog* dialog, int id)
{
    wxWindow* found = nullptr;
    std::function<void(wxWindow*)> find = [&](wxWindow* w) {
        for (wxWindow* child : w->GetChildren()) {
            if (found != nullptr || dynamic_cast<wxTopLevelWindow*>(child) != nullptr)
                continue;
            if (child->GetId() == id && child->IsShown() && child->IsEnabled() &&
                (dynamic_cast<wxButton*>(child) != nullptr || class_name(child).find("Button") != std::string::npos))
                found = child;
            else
                find(child);
        }
    };
    find(dialog);
    return found;
}

} // namespace

bool has_dialog_button(wxDialog* dialog, int id)
{
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr)
        return has_box_button(dialog, id);
    return dialog != nullptr && dialog_button(dialog, id) != nullptr;
}

bool press_dialog_button(wxDialog* dialog, int id)
{
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr)
        return press_message_box(dialog, id);
    wxWindow* pressed = dialog_button(dialog, id);
    if (pressed == nullptr) {
        dialog->EndModal(id);
        return false;
    }
    wxCommandEvent evt(wxEVT_BUTTON, id);
    evt.SetEventObject(pressed);
    pressed->GetEventHandler()->ProcessEvent(evt);
    return true;
}

AutoAnswerRule make_rule(const std::string& pattern, const std::string& button)
{
    AutoAnswerRule rule;
    rule.pattern = pattern;
    rule.button  = button;
    try {
        rule.compiled = std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error& e) {
        throw ToolFailure("Invalid pattern \"" + pattern + "\": " + e.what());
    }
    return rule;
}

bool rule_matches(const AutoAnswerRule& rule, const std::string& title, const std::string& message)
{
    return std::regex_search(title + "\n" + message, rule.compiled) || (message.empty() && std::regex_search(title, rule.compiled));
}

int native_dialog_button(wxDialog* dialog, const std::string& button)
{
    const int id = resolve_dialog_button(dialog, button);
    return id != wxID_NONE && has_dialog_button(dialog, id) ? id : int(wxID_NONE);
}

ModalTracker::ModalTracker()
{
    Register();
    // A modal opened before the tracker existed (the server turned on from Preferences, itself
    // modal) blocks the event loop all the same; the guards and dialog tools must see it.
    for (wxWindow* w : wxTopLevelWindows)
        if (auto* dialog = dynamic_cast<wxDialog*>(w); dialog != nullptr && dialog->IsModal())
            push(dialog);
    publish();
}

void ModalTracker::push(wxDialog* dialog)
{
    m_stack.push_back(dialog);
    m_serials.push_back(++m_last_serial);
}

std::uint64_t ModalTracker::serial(const wxDialog* dialog) const
{
    const auto it = std::find(m_stack.begin(), m_stack.end(), dialog);
    return it == m_stack.end() ? 0 : m_serials[std::size_t(it - m_stack.begin())];
}

bool ModalTracker::is_open(std::uint64_t serial) const
{
    return serial != 0 && std::find(m_serials.begin(), m_serials.end(), serial) != m_serials.end();
}

ModalTracker::~ModalTracker() { Unregister(); }

int ModalTracker::push_rules(std::vector<AutoAnswerRule> rules)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int layer = m_next_layer++;
    m_layers.emplace_back(layer, std::move(rules));
    return layer;
}

void ModalTracker::pop_rules(int layer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layers.erase(std::remove_if(m_layers.begin(), m_layers.end(), [layer](const auto& l) { return l.first == layer; }), m_layers.end());
}

json ModalTracker::rules_json() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    json out = json::array();
    for (const auto& [layer, rules] : m_layers)
        for (const AutoAnswerRule& r : rules)
            out.push_back({{"layer", layer}, {"match", r.pattern}, {"button", r.button}, {"uses", r.uses}});
    return out;
}

bool ModalTracker::rule_in_force(int layer, const std::string& pattern, const std::string& button) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [id, rules] : m_layers)
        if (layer == 0 || id == layer)
            for (const AutoAnswerRule& r : rules)
                if (r.pattern == pattern && r.button == button)
                    return true;
    return false;
}

void ModalTracker::count_use(int layer, const std::string& pattern, const std::string& button)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, rules] : m_layers)
        if (layer == 0 || id == layer)
            for (AutoAnswerRule& r : rules)
                if (r.pattern == pattern && r.button == button) {
                    ++r.uses;
                    return;
                }
}

json ModalTracker::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool ModalTracker::any_open() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_snapshot.empty();
}

void ModalTracker::publish()
{
    json snap = json::array();
    for (wxDialog* d : m_stack)
        snap.push_back(describe_dialog(d));
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = std::move(snap);
}

int ModalTracker::Enter(wxDialog* dialog)
{
    // An exception must never escape into ShowModal().
    try {
        const json        info    = describe_dialog(dialog);
        const std::string title   = info["title"].get<std::string>();
        const std::string message = info["message"].get<std::string>();
        const bool        web     = info.value("web", false);
        // A web dialog's rule whose button is not one of its wx buttons is answered on its page.
        std::optional<std::tuple<int, std::string, std::string>> web_rule;
        const int         answer = [&] {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto layer = m_layers.rbegin(); layer != m_layers.rend(); ++layer)
                for (AutoAnswerRule& rule : layer->second)
                    if (rule_matches(rule, title, message)) {
                        if (web && native_dialog_button(dialog, rule.button) == wxID_NONE) {
                            web_rule.emplace(layer->first, rule.pattern, rule.button);
                            return int(wxID_NONE);
                        }
                        const int id = resolve_dialog_button(dialog, rule.button);
                        // A file or folder picker answered with anything but cancel would return
                        // without a path, so only cancel answers one.
                        if (id != wxID_NONE && (id == wxID_CANCEL || !is_picker(dialog))) {
                            ++rule.uses;
                            BOOST_LOG_TRIVIAL(info) << "MCP: auto-answered dialog \"" << info["title"].get<std::string>() << "\" with "
                                                    << rule.button << " (rule \"" << rule.pattern << "\")";
                            return id;
                        }
                    }
            return int(wxID_NONE);
        }();
        // A native dialog has no handlers to run: ShowModal() returns the id without showing it.
        if (answer != wxID_NONE && is_native_dialog(dialog))
            return answer;
        push(dialog);
        publish();
        // A wx dialog's buttons do its work in their handlers (Transfer, Discard, Clone's Fill), which
        // a returned id would skip. So it opens, and the button is pressed once its modal loop runs, as
        // dialog_respond does. Queued on the dialog, the press goes with it if it is destroyed first;
        // a modal opened above it before then leaves it to dialog_respond. The tracker lives as long
        // as the app.
        if (answer != wxID_NONE)
            dialog->CallAfter([this, dialog, answer] {
                if (top() == dialog) {
                    before_press();
                    press_dialog_button(dialog, answer);
                }
            });
        if (web_rule) {
            const auto& [layer, pattern, button] = *web_rule;
            const int standard = standard_dialog_button(button);
            start_web_answer(*this, dialog, layer, pattern, button, standard == wxID_CANCEL || standard == wxID_CLOSE ? standard : int(wxID_NONE));
        }
    } catch (...) {
    }
    return wxID_NONE;
}

void ModalTracker::Exit(wxDialog* dialog)
{
    try {
        for (std::size_t i = m_stack.size(); i-- > 0;)
            if (m_stack[i] == dialog) {
                m_stack.erase(m_stack.begin() + std::ptrdiff_t(i));
                m_serials.erase(m_serials.begin() + std::ptrdiff_t(i));
            }
        publish();
    } catch (...) {
    }
}

}}} // namespace Slic3r::GUI::Mcp
