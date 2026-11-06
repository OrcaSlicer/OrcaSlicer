#include "ModalTracker.hpp"

#include "McpTypes.hpp"
#include "UiInspector.hpp"
#include "WebPages.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/RadioBox.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"

#include <boost/log/trivial.hpp>

#include <wx/bookctrl.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/gdicmn.h>
#include <wx/html/htmlwin.h>
#include <wx/msgdlg.h>
#include <wx/popupwin.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/textentry.h>
#include <wx/thread.h>
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
// Rows of one list a message shows; ui_snapshot lists them all.
constexpr std::size_t kMaxListRows = 25;

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

bool is_button(const wxWindow* w) { return dynamic_cast<const wxButton*>(w) != nullptr || dynamic_cast<const ::Button*>(w) != nullptr; }

// At most kMaxMessage bytes, cut where a character starts, without trailing blank lines.
void fit_message(std::string& s)
{
    if (s.size() > kMaxMessage) {
        std::size_t cut = kMaxMessage;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s = s.substr(0, cut) + "...";
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
}

// The dialog's own text, which rules search (rule_matches()): its static text and html, one per line,
// in child order, through every shown child but its buttons. Its controls' values are left out, so a
// rule written against the dialog's wording matches however the controls read.
void own_text(wxWindow* w, std::string& text)
{
    for (wxWindow* child : w->GetChildren()) {
        if (!child->IsShown())
            continue;
        if (auto* html = dynamic_cast<wxHtmlWindow*>(child)) {
            text += into_u8(html->ToText()) + "\n";
            continue;
        }
        if (is_button(child))
            continue;
        if (auto* label = dynamic_cast<wxStaticText*>(child)) {
            const wxString t = label->GetLabel();
            if (!t.empty())
                text += into_u8(t) + "\n";
        }
        own_text(child, text);
    }
}

// Whether `w` and the windows between it and its dialog are enabled. The dialog itself is left out:
// one under another modal is disabled as a whole, which says nothing about its controls.
bool enabled_in_dialog(const wxWindow* w)
{
    for (; w != nullptr && dynamic_cast<const wxTopLevelWindow*>(w) == nullptr; w = w->GetParent())
        if (!w->IsThisEnabled())
            return false;
    return true;
}

// The text a field edits: a TextInput's own text control, or the field itself.
wxTextEntry* field_entry(wxWindow* w)
{
    if (auto* input = dynamic_cast<::TextInput*>(w))
        return input->GetTextCtrl();
    return dynamic_cast<wxTextEntry*>(w);
}

// A field whose value is a secret: password style, a setting for a password or key, or a field its
// dialog names kSecretField (a printer's access code, which it shows in plain text).
bool is_secret_field(wxWindow* w)
{
    auto* inner = dynamic_cast<wxWindow*>(field_entry(w));
    if (w->GetName() == kSecretField || (inner != nullptr && inner->GetName() == kSecretField))
        return true;
    if (inner != nullptr && inner->HasFlag(wxTE_PASSWORD))
        return true;
    return is_secret_key(option_key_of(w));
}

// A field's value without the credentials the URLs in it carry, each word of it on its own. A host
// setting's value without a scheme counts as a URL too: the print hosts put http:// before it and sign
// in with its user:password@.
std::string redacted_value(wxWindow* w, const std::string& value)
{
    const bool  host = value.find('@') != std::string::npos && lower(option_key_of(w)).find("host") != std::string::npos;
    std::string out;
    for (std::size_t i = 0; i < value.size();) {
        const bool        space = std::isspace(static_cast<unsigned char>(value[i])) != 0;
        std::size_t       end   = i;
        while (end < value.size() && (std::isspace(static_cast<unsigned char>(value[end])) != 0) == space)
            ++end;
        const std::string word = value.substr(i, end - i);
        if (!space && word.find("://") != std::string::npos)
            out += redact_url(word);
        else if (!space && host && word.find('@') != std::string::npos)
            out += redact_url("http://" + word).substr(std::char_traits<char>::length("http://"));
        else
            out += word;
        i = end;
    }
    return out;
}

// A toggle's mark: [x] / [ ] / [-] for a checkbox or switch, (*) / ( ) for a radio button.
std::string toggle_mark(wxWindow* w, const std::string& kind)
{
    const int checked = checked_of(w);
    if (kind == "radio" || dynamic_cast<RadioBox*>(w) != nullptr)
        return checked == 1 ? "(*)" : "( )";
    return checked == 1 ? "[x]" : checked == 2 ? "[-]" : "[ ]";
}

// A toggle nothing completed, named by what is its own: the setting it edits, its tooltip, else its
// window name. Not by the text beside it, which the layout gave whatever it labels.
void complete_toggle(MessageCell& cell)
{
    if (!cell.toggle)
        return;
    std::string name;
    if (cell.window != nullptr) {
        // A setting field's tooltip ends in its "parameter name" line, which names nothing.
        wxString tip = cell.window->GetToolTipText().BeforeFirst('\n');
        if (tip.StartsWith(_L("parameter name") + ": "))
            tip.clear();
        if (const std::string key = option_key_of(cell.window); !key.empty())
            name = option_label(key);
        if (name.empty() && !tip.Trim().Trim(false).empty())
            name = into_u8(tip);
        if (name.empty())
            if (const std::string window_name = meaningful_name(cell.window); !window_name.empty())
                name = "(" + window_name + ")";
    }
    cell.text += (name.empty() ? "" : " " + name) + cell.suffix;
    cell.toggle = false;
    cell.suffix.clear();
}

std::string as_lines(std::vector<MessageCell> cells)
{
    std::string out;
    for (MessageCell& cell : cells) {
        complete_toggle(cell);
        out += (out.empty() ? "" : "\n") + cell.text;
    }
    return out;
}

// Reads a wx dialog's message (describe_dialog()): each shown child becomes a cell, a container's
// cells are laid out in rows (layout_cells()), and its buttons are listed apart. Once the cells read
// so far fill the message, only buttons are looked for: a dialog costs its window count, whatever the
// text, values and rows it holds.
class DialogReader
{
public:
    explicit DialogReader(json& buttons) : m_buttons(buttons) {}

    std::string message(wxWindow* dialog) { return as_lines(read(dialog)); }

private:
    std::vector<MessageCell> read(wxWindow* parent)
    {
        std::vector<MessageCell> cells;
        for (wxWindow* child : parent->GetChildren())
            if (std::optional<MessageCell> cell = read_child(child); cell && !cell->text.empty())
                cells.push_back(std::move(*cell));
        return layout_cells(std::move(cells));
    }

    // A container's cells, as one cell at the container: a single one keeps what it is, so a row's
    // cells in panels of their own (UnsavedChangesDialog's name, old and new value) still join.
    std::optional<MessageCell> container(wxWindow* w, const std::string& head = {})
    {
        std::vector<MessageCell> inner = read(w);
        MessageCell              cell;
        if (head.empty() && inner.size() == 1)
            cell = std::move(inner.front());
        else {
            cell.text = head;
            if (!inner.empty())
                cell.text += (head.empty() ? "" : "\n") + as_lines(std::move(inner));
        }
        cell.rect = w->GetRect();
        if (cell.text.empty())
            return std::nullopt;
        return cell;
    }

    std::optional<MessageCell> content(wxWindow* w, std::string text, const std::string& suffix = {})
    {
        if (text.empty())
            return std::nullopt;
        MessageCell cell;
        cell.rect = w->GetRect();
        cell.text = std::move(text) + suffix;
        m_chars += cell.text.size();
        return cell;
    }

    bool full() const { return m_chars > kMaxMessage; }

    std::optional<MessageCell> read_child(wxWindow* w)
    {
        // A nested dialog is described on its own, and a popup belongs to no dialog.
        if (!w->IsShown() || dynamic_cast<wxTopLevelWindow*>(w) != nullptr || dynamic_cast<wxPopupWindow*>(w) != nullptr)
            return std::nullopt;
        if (is_button(w)) {
            // A selector's option only selects it, so it is read with its selector.
            const wxString label = wxStripMenuCodes(w->GetLabel());
            if (!label.empty() && selector_option(w).selector == nullptr) {
                json button{{"id", w->GetId()}, {"label", into_u8(label)}};
                if (!enabled_in_dialog(w))
                    button["enabled"] = false;
                m_buttons.push_back(std::move(button));
            }
            return std::nullopt;
        }
        const std::string kind     = kind_of(w);
        const std::string disabled = enabled_in_dialog(w) ? "" : " (disabled)";
        if (kind == "canvas" || kind == "webview")
            return std::nullopt;
        if (auto* box = dynamic_cast<wxStaticBox*>(w)) {
            const std::string label = full() ? std::string() : into_u8(box->GetLabelText().Trim().Trim(false));
            m_chars += label.size();
            std::optional<MessageCell> cell = container(box, label);
            // Boxed controls that are its siblings read after its title, not beside it.
            if (cell && box->GetChildren().empty())
                cell->rect.height = box->GetCharHeight();
            return cell;
        }
        if (std::optional<SelectorChoices> choices = selector_choices(w)) {
            auto*                      book = dynamic_cast<wxBookCtrlBase*>(w);
            wxWindow*                  page = book != nullptr ? book->GetCurrentPage() : nullptr;
            const std::string          line = full() ? std::string() : selector_line(choices->options, choices->selected);
            std::optional<MessageCell> cell;
            if (page == nullptr)
                cell = content(w, line, disabled);
            else {
                // A notebook's tabs, then the page shown.
                m_chars += line.size();
                cell = container(page, line);
                if (cell)
                    cell->rect = w->GetRect();
            }
            if (cell)
                cell->marked = true;
            return cell;
        }
        const bool leaf = !kind.empty() && kind != "tabs" && kind != "window" && kind != "dialog" && kind != "popup";
        if (!leaf && dynamic_cast<wxHtmlWindow*>(w) == nullptr && dynamic_cast<wxGauge*>(w) == nullptr)
            return container(w);
        if (full())
            return std::nullopt;
        if (auto* html = dynamic_cast<wxHtmlWindow*>(w))
            return content(w, into_u8(html->ToText()));
        if (auto* gauge = dynamic_cast<wxGauge*>(w))
            return content(w, "[" + std::to_string(gauge->GetRange() > 0 ? gauge->GetValue() * 100 / gauge->GetRange() : 0) + "%]");
        if (kind == "checkbox" || kind == "toggle" || kind == "radio") {
            const std::string mark  = toggle_mark(w, kind);
            const std::string label = into_u8(wxStripMenuCodes(w->GetLabel()).Trim().Trim(false));
            if (!label.empty()) {
                std::optional<MessageCell> cell = content(w, mark + " " + label, disabled);
                cell->marked = true;
                return cell;
            }
            std::optional<MessageCell> cell = content(w, mark);
            cell->toggle = true;
            cell->suffix = disabled;
            cell->window = w;
            return cell;
        }
        if (kind == "combo" || kind == "choice") {
            // Orca's ComboBox shows its placeholder ("Select Vendor") until something is picked.
            auto* combo = dynamic_cast<::ComboBox*>(w);
            return content(w, "[" + (combo != nullptr ? into_u8(combo->GetValue()) : value_of(w)) + " ▾]", disabled);
        }
        if (kind == "spin" || kind == "slider")
            return content(w, "[" + value_of(w) + "]", disabled);
        if (kind == "text") {
            const std::string value = value_of(w);
            if (is_secret_field(w))
                return content(w, value.empty() ? "[]" : "[******]", disabled);
            wxTextEntry* entry = field_entry(w);
            if (entry != nullptr && !entry->IsEditable())
                return content(w, redacted_value(w, value));
            return content(w, "[" + redacted_value(w, value) + "]", disabled);
        }
        if (kind == "list")
            return list(w);
        if (kind == "label" || kind == "control")
            return content(w, into_u8(w->GetLabel()));
        return std::nullopt;
    }

    // One line per shown row, up to kMaxListRows: its label and cells, its check, whether it is
    // selected; a tree's rows indented by depth.
    std::optional<MessageCell> list(wxWindow* w)
    {
        const std::vector<ListRow> rows = list_rows(w, kMaxListRows + 1);
        std::string                text;
        for (std::size_t i = 0; i < rows.size() && i < kMaxListRows; ++i) {
            const ListRow&    row   = rows[i];
            const std::string label = row.path.empty() ? std::string() : row.path.back();
            std::string       line  = std::string(std::size_t(2 * row.depth), ' ') + "- ";
            if (row.checked >= 0)
                line += row.checked == 1 ? "[x] " : row.checked == 2 ? "[-] " : "[ ] ";
            line += label;
            if (!row.value.empty())
                line += (label.empty() ? "" : " | ") + row.value;
            if (row.selected)
                line += " (selected)";
            if (!row.enabled)
                line += " (disabled)";
            text += (text.empty() ? "" : "\n") + line;
        }
        if (rows.size() > kMaxListRows)
            text += "\n- … more rows (ui_snapshot lists them)";
        return content(w, text);
    }

    json&       m_buttons;
    std::size_t m_chars = 0;
};

} // namespace

std::vector<MessageCell> layout_cells(std::vector<MessageCell> cells)
{
    auto centre   = [](const wxRect& r) { return r.y + r.height / 2; };
    auto same_row = [&](const MessageCell& a, const MessageCell& b) {
        const bool         a_in_b  = b.rect.y <= centre(a.rect) && centre(a.rect) <= b.rect.GetBottom();
        const bool         b_in_a  = a.rect.y <= centre(b.rect) && centre(b.rect) <= a.rect.GetBottom();
        const MessageCell& taller  = a.rect.height >= b.rect.height ? a : b;
        const MessageCell& shorter = a.rect.height >= b.rect.height ? b : a;
        if (taller.rect.height <= 2 * std::max(1, shorter.rect.height))
            return a_in_b || b_in_a;
        // A cell over twice as tall as the other (a wrapped label, a column of text) shares its row with
        // a cell beside its first line or centred on it; the rows of a column beside it below that line
        // keep rows of their own. The first line is as tall as the shorter cell, or as one of the taller
        // cell's lines where those are shorter.
        const int lines = int(std::count(taller.text.begin(), taller.text.end(), '\n')) + 1;
        const int line  = std::min(shorter.rect.height, std::max(1, taller.rect.height / lines));
        const int c     = centre(shorter.rect);
        return (taller.rect.y <= c && c <= taller.rect.y + line) || (a_in_b && b_in_a);
    };
    std::stable_sort(cells.begin(), cells.end(), [](const MessageCell& a, const MessageCell& b) { return a.rect.y < b.rect.y; });
    std::vector<std::vector<MessageCell>> rows;
    for (MessageCell& cell : cells) {
        const bool joins_row = !rows.empty() && std::any_of(rows.back().begin(), rows.back().end(),
                                                            [&](const MessageCell& other) { return same_row(other, cell); });
        if (!joins_row)
            rows.emplace_back();
        rows.back().push_back(std::move(cell));
    }
    // Joined cells meet at one separator, whatever spaces their labels end in ("Start PA: ").
    auto join = [](MessageCell& left, const std::string& separator, const std::string& right) {
        while (!left.text.empty() && left.text.back() == ' ')
            left.text.pop_back();
        left.text += separator + right.substr(std::min(right.find_first_not_of(' '), right.size()));
    };
    std::vector<MessageCell> out;
    for (std::vector<MessageCell>& row : rows) {
        std::stable_sort(row.begin(), row.end(), [](const MessageCell& a, const MessageCell& b) { return a.rect.x < b.rect.x; });
        const std::size_t first = out.size();
        for (MessageCell& cell : row) {
            if (out.size() > first) {
                MessageCell& prev  = out.back();
                // Cells that overlap were not laid out side by side (a dialog not laid out yet).
                const bool   apart = prev.rect.GetRight() < cell.rect.x;
                if (apart && prev.toggle && !cell.toggle && !cell.marked) {
                    join(prev, " ", cell.text + prev.suffix);
                    prev.toggle = false;
                    prev.marked = true;
                    prev.suffix.clear();
                    prev.window = nullptr;
                    prev.rect.Union(cell.rect);
                    continue;
                }
                const bool single = prev.text.find('\n') == std::string::npos && cell.text.find('\n') == std::string::npos;
                if (apart && !prev.toggle && !cell.toggle && single) {
                    join(prev, " | ", cell.text);
                    prev.marked = prev.marked || cell.marked;
                    prev.rect.Union(cell.rect);
                    continue;
                }
            }
            out.push_back(std::move(cell));
        }
        // A toggle with nothing to its right takes a label to its left: "Show splash screen | [x]", as
        // Preferences lays its switches out. Of a title and its hint stacked there, the title.
        for (std::size_t i = first; i < out.size();) {
            std::size_t label = out.size();
            for (std::size_t j = first; out[i].toggle && j < i; ++j)
                if (!out[j].toggle && !out[j].marked && out[j].text.find('\n') == std::string::npos && out[j].rect.GetRight() < out[i].rect.x &&
                    (label == out.size() || out[j].rect.y < out[label].rect.y))
                    label = j;
            if (label == out.size()) {
                ++i;
                continue;
            }
            join(out[label], " | ", out[i].text + out[i].suffix);
            out[label].marked = true;
            out[label].rect.Union(out[i].rect);
            out.erase(out.begin() + std::ptrdiff_t(i));
        }
    }
    return out;
}

std::string selector_line(const std::vector<std::string>& options, int selected)
{
    std::string out;
    for (std::size_t i = 0; i < options.size(); ++i)
        out += (i == 0 ? "" : " | ") + std::string(int(i) == selected ? "(*) " : "( ) ") + options[i];
    return out;
}

bool is_secret_key(const std::string& key)
{
    const std::string k = lower(key);
    for (const char* word : {"password", "apikey", "api_key", "token", "secret"})
        if (k.find(word) != std::string::npos)
            return true;
    return false;
}

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

namespace {

// Whether `dialog` can return `id`: a message box only the ids of the buttons its style gives it, on
// every platform (on macOS too, where no call presses them, but ShowModal() still returns one); any
// other dialog any id.
bool box_answers(wxDialog* dialog, int id)
{
    const auto* box = dynamic_cast<wxMessageDialogBase*>(dialog);
    if (box == nullptr)
        return true;
    const long style = box->GetMessageDialogStyle();
    if (style & wxYES_NO) {
        if (id == wxID_YES || id == wxID_NO)
            return true;
    } else if (id == wxID_OK)
        return true;
    return id == wxID_CANCEL && (style & wxCANCEL) != 0;
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

json describe_dialog(wxDialog* dialog, std::string* text)
{
    json out{{"title", into_u8(dialog->GetTitle())}, {"class", class_name(dialog)}, {"native", is_native_dialog(dialog)}};
    std::string message;
    std::string own;
    json        buttons = json::array();
    if (auto* md = dynamic_cast<wxMessageDialogBase*>(dialog)) {
        message = into_u8(md->GetMessage());
        if (!md->GetExtendedMessage().empty())
            message += "\n" + into_u8(md->GetExtendedMessage());
        buttons = message_box_buttons(dialog);
        own     = message;
    } else if (auto* fd = dynamic_cast<wxFileDialogBase*>(dialog)) {
        message = own = into_u8(fd->GetMessage());
    } else if (auto* dd = dynamic_cast<wxDirDialogBase*>(dialog)) {
        message = own = into_u8(dd->GetMessage());
    } else {
        message = DialogReader(buttons).message(dialog);
        if (text != nullptr)
            own_text(dialog, own);
    }
    fit_message(message);
    out["message"] = message;
    out["buttons"] = std::move(buttons);
    if (!is_native_dialog(dialog) && web_view_in(dialog, false) != nullptr)
        out["web"] = true;
    if (text != nullptr) {
        fit_message(own);
        *text = std::move(own);
    }
    return out;
}

int dialog_button_by_label(wxDialog* dialog, const std::string& label)
{
    if (dialog == nullptr)
        return wxID_NONE;
    const std::string name = lower(label);
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr) {
        for (const json& b : message_box_buttons(dialog))
            if (normalize_label(from_u8(b["label"].get<std::string>())) == name)
                return b["id"].get<int>();
        return wxID_NONE;
    }
    if (is_native_dialog(dialog))
        return wxID_NONE;
    // The buttons a person sees, a selector's options among them: pressing one of those selects it.
    int                            found = wxID_NONE;
    std::function<void(wxWindow*)> find  = [&](wxWindow* w) {
        for (wxWindow* child : w->GetChildren()) {
            if (found != wxID_NONE || !child->IsShown() || dynamic_cast<wxTopLevelWindow*>(child) != nullptr ||
                dynamic_cast<wxPopupWindow*>(child) != nullptr)
                continue;
            if (is_button(child)) {
                // The label the dialog's description shows for it: a selector option's (an icon one's
                // tooltip), or the button's without its mnemonic, so "A&&B" is found as "A&B".
                wxString shown = selector_option(child).selector != nullptr ? from_u8(selector_option_label(child)) :
                                                                              wxStripMenuCodes(child->GetLabel());
                if (!shown.Trim().Trim(false).empty() && lower(into_u8(shown)) == name)
                    found = child->GetId();
                continue;
            }
            if (dynamic_cast<wxHtmlWindow*>(child) == nullptr)
                find(child);
        }
    };
    find(dialog);
    return found;
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

// The button with `id` among `dialog`'s windows, outside nested dialogs and popups, and whether it
// shows: it and every window above it are shown. A shown one wins over one on a hidden page.
struct FoundButton
{
    wxWindow* button = nullptr;
    bool      shown  = false;
};

FoundButton find_button(wxDialog* dialog, int id)
{
    FoundButton                          found;
    std::function<void(wxWindow*, bool)> find = [&](wxWindow* w, bool shown) {
        for (wxWindow* child : w->GetChildren()) {
            if (found.shown || dynamic_cast<wxTopLevelWindow*>(child) != nullptr || dynamic_cast<wxPopupWindow*>(child) != nullptr)
                continue;
            const bool here = shown && child->IsShown();
            if (child->GetId() == id && (dynamic_cast<wxButton*>(child) != nullptr || class_name(child).find("Button") != std::string::npos)) {
                if (here || found.button == nullptr)
                    found = {child, here};
            } else
                find(child, here);
        }
    };
    find(dialog, true);
    return found;
}

} // namespace

bool has_dialog_button(wxDialog* dialog, int id)
{
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr)
        return has_box_button(dialog, id);
    if (dialog == nullptr)
        return false;
    const FoundButton found = find_button(dialog, id);
    return found.shown && enabled_in_dialog(found.button);
}

std::optional<DialogPress> refused_press(wxDialog* dialog, int id)
{
    if (dialog == nullptr || is_native_dialog(dialog))
        return std::nullopt;
    const FoundButton found = find_button(dialog, id);
    if (found.button == nullptr)
        return std::nullopt;
    if (found.shown)
        return enabled_in_dialog(found.button) ? std::nullopt : std::optional<DialogPress>(DialogPress::Disabled);
    // Cancel and Close end a dialog that shows neither, as they end one without them.
    return id == wxID_CANCEL || id == wxID_CLOSE ? std::nullopt : std::optional<DialogPress>(DialogPress::Hidden);
}

DialogPress press_dialog_button(wxDialog* dialog, int id)
{
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr)
        return press_message_box(dialog, id) ? DialogPress::Pressed : DialogPress::Failed;
    if (const std::optional<DialogPress> refused = refused_press(dialog, id))
        return *refused;
    const FoundButton found = find_button(dialog, id);
    if (!found.shown) {
        dialog->EndModal(id);
        return DialogPress::Ended;
    }
    wxCommandEvent evt(wxEVT_BUTTON, id);
    evt.SetEventObject(found.button);
    found.button->GetEventHandler()->ProcessEvent(evt);
    return DialogPress::Pressed;
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

bool rule_matches(const AutoAnswerRule& rule, const std::string& title, const std::string& message, const std::string& text)
{
    return std::regex_search(title + "\n" + message, rule.compiled) || std::regex_search(title + "\n" + text, rule.compiled) ||
           (text.empty() && std::regex_search(title, rule.compiled));
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

json ModalTracker::snapshot()
{
    // On the UI thread the dialogs are described as they are now: text a dialog filled in once it
    // showed, a field typed in, a page switched. Should describing fail, the last description stands.
    if (wxIsMainThread()) {
        try {
            publish();
        } catch (...) {
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool ModalTracker::any_open() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_snapshot.empty();
}

namespace {

// A dialog that can't be described, listed by its title and kind so the guards and the tools still see
// it and rules match it by its title: a web one too, as describe_dialog() would have flagged it.
json undescribed(wxDialog* dialog)
{
    json entry{{"title", into_u8(dialog->GetTitle())}, {"class", class_name(dialog)}, {"native", is_native_dialog(dialog)},
               {"message", ""},
               {"buttons", json::array()}};
    try {
        if (!is_native_dialog(dialog) && web_view_in(dialog, false) != nullptr)
            entry["web"] = true;
    } catch (...) {
    }
    return entry;
}

} // namespace

void ModalTracker::publish()
{
    json snap = json::array();
    for (wxDialog* d : m_stack) {
        try {
            snap.push_back(describe_dialog(d));
        } catch (...) {
            snap.push_back(undescribed(d));
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = std::move(snap);
}

int ModalTracker::Enter(wxDialog* dialog)
{
    // An exception must never escape into ShowModal().
    try {
        // A window hidden from automation (the Python scripts setting) is the person's alone: no rule
        // answers it, and leaving it off the stack keeps it out of the dialog tools and get_state.
        if (dialog != nullptr && dialog->GetName() == kHiddenFromAutomation)
            return wxID_NONE;
        // A description that fails leaves the dialog matched by its title alone, and tracked all the same.
        std::string text;
        json        info;
        try {
            info = describe_dialog(dialog, &text);
        } catch (...) {
            info = undescribed(dialog);
            text.clear();
        }
        const std::string title   = info["title"].get<std::string>();
        const std::string message = info["message"].get<std::string>();
        const bool        web     = info.value("web", false);
        const bool        native  = is_native_dialog(dialog);
        // A web dialog's rule whose button is not one of its wx buttons is answered on its page, and a
        // wx dialog's rule once its button is pressed.
        std::optional<std::tuple<int, std::string, std::string>> web_rule, wx_rule;
        const int         answer = [&] {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto layer = m_layers.rbegin(); layer != m_layers.rend(); ++layer)
                for (AutoAnswerRule& rule : layer->second)
                    if (rule_matches(rule, title, message, text)) {
                        if (web && native_dialog_button(dialog, rule.button) == wxID_NONE) {
                            web_rule.emplace(layer->first, rule.pattern, rule.button);
                            return int(wxID_NONE);
                        }
                        const int id = resolve_dialog_button(dialog, rule.button);
                        // A file or folder picker answered with anything but cancel would return
                        // without a path, so only cancel answers one; a message box only with a button
                        // it has.
                        if (id != wxID_NONE && (id == wxID_CANCEL || !is_picker(dialog)) && box_answers(dialog, id)) {
                            if (native) {
                                ++rule.uses;
                                BOOST_LOG_TRIVIAL(info) << "MCP: auto-answered dialog \"" << title << "\" with " << rule.button
                                                        << " (rule \"" << rule.pattern << "\")";
                            } else
                                wx_rule.emplace(layer->first, rule.pattern, rule.button);
                            return id;
                        }
                    }
            return int(wxID_NONE);
        }();
        // A native dialog has no handlers to run: ShowModal() returns the id without showing it.
        if (answer != wxID_NONE && native)
            return answer;
        push(dialog);
        try {
            publish();
        } catch (...) {
        }
        // A wx dialog's buttons do its work in their handlers (Transfer, Discard, Clone's Fill), which
        // a returned id would skip. So it opens, and the button is pressed once its modal loop runs, as
        // dialog_respond does; the rule counts as used once that pressed the button or ended the
        // dialog. A button disabled by then leaves the dialog open. Queued on the dialog, the press goes
        // with it if it is destroyed first; a modal opened above it before then leaves it to
        // dialog_respond. The tracker lives as long as the app.
        if (wx_rule)
            dialog->CallAfter([this, dialog, answer, title, rule = *wx_rule] {
                if (top() != dialog)
                    return;
                const auto& [layer, pattern, button] = rule;
                before_press();
                if (const DialogPress press = press_dialog_button(dialog, answer); press == DialogPress::Disabled || press == DialogPress::Hidden) {
                    BOOST_LOG_TRIVIAL(info) << "MCP: auto-answer rule \"" << pattern << "\" for dialog \"" << title << "\": its "
                                            << button << " button is " << (press == DialogPress::Hidden ? "not shown" : "disabled")
                                            << "; the dialog stays open";
                    return;
                }
                count_use(layer, pattern, button);
                BOOST_LOG_TRIVIAL(info) << "MCP: auto-answered dialog \"" << title << "\" with " << button << " (rule \"" << pattern
                                        << "\")";
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
