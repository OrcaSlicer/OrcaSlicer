#include "ModalTracker.hpp"

#include "McpTypes.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
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
#include <cstdlib>
#include <exception>
#include <mutex>
#include <regex>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
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
#if defined(__GNUC__) || defined(__clang__)
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

json describe_dialog(wxDialog* dialog)
{
    json out{{"title", into_u8(dialog->GetTitle())}, {"class", class_name(dialog)}, {"native", is_native_dialog(dialog)}};
    std::string message;
    json        buttons = json::array();
    if (auto* md = dynamic_cast<wxMessageDialogBase*>(dialog)) {
        message = into_u8(md->GetMessage());
        if (!md->GetExtendedMessage().empty())
            message += "\n" + into_u8(md->GetExtendedMessage());
    } else if (auto* fd = dynamic_cast<wxFileDialogBase*>(dialog)) {
        message = into_u8(fd->GetMessage());
    } else {
        collect(dialog, message, buttons);
    }
    if (message.size() > kMaxMessage)
        message = message.substr(0, kMaxMessage) + "...";
    while (!message.empty() && (message.back() == '\n' || message.back() == ' '))
        message.pop_back();
    out["message"] = message;
    out["buttons"] = std::move(buttons);
    return out;
}

int resolve_dialog_button(wxDialog* dialog, const std::string& button)
{
    const std::string name = lower(button);
    static const std::pair<const char*, int> standard[] = {{"yes", wxID_YES},       {"no", wxID_NO},       {"ok", wxID_OK},
                                                           {"cancel", wxID_CANCEL}, {"close", wxID_CLOSE}, {"apply", wxID_APPLY}};
    // A button whose label matches wins, so "Don't save" or "Keep" reach the dialog's own ids.
    if (dialog != nullptr && !is_native_dialog(dialog)) {
        std::string message;
        json        buttons = json::array();
        collect(dialog, message, buttons);
        for (const json& b : buttons)
            if (normalize_label(from_u8(b["label"].get<std::string>())) == name)
                return b["id"].get<int>();
    }
    for (const auto& [key, id] : standard)
        if (name == key)
            return id;
    if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) || c == '-'; }))
        return std::atoi(name.c_str());
    return wxID_NONE;
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

ModalTracker::ModalTracker()
{
    Register();
    // A modal opened before the tracker existed (the server turned on from Preferences, itself
    // modal) blocks the event loop all the same; the guards and dialog tools must see it.
    for (wxWindow* w : wxTopLevelWindows)
        if (auto* dialog = dynamic_cast<wxDialog*>(w); dialog != nullptr && dialog->IsModal())
            m_stack.push_back(dialog);
    publish();
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
        const json        info = describe_dialog(dialog);
        const std::string text = info["title"].get<std::string>() + "\n" + info["message"].get<std::string>();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto layer = m_layers.rbegin(); layer != m_layers.rend(); ++layer)
                for (AutoAnswerRule& rule : layer->second)
                    if (std::regex_search(text, rule.compiled)) {
                        const int id = resolve_dialog_button(dialog, rule.button);
                        // A file or folder picker answered with anything but cancel would return
                        // without a path, so only cancel answers one.
                        if (id != wxID_NONE && (id == wxID_CANCEL || !is_picker(dialog))) {
                            ++rule.uses;
                            BOOST_LOG_TRIVIAL(info) << "MCP: auto-answered dialog \"" << info["title"].get<std::string>() << "\" with "
                                                    << rule.button << " (rule \"" << rule.pattern << "\")";
                            return id; // ShowModal() returns it without showing the dialog
                        }
                    }
        }
        m_stack.push_back(dialog);
        publish();
    } catch (...) {
    }
    return wxID_NONE;
}

void ModalTracker::Exit(wxDialog* dialog)
{
    try {
        m_stack.erase(std::remove(m_stack.begin(), m_stack.end(), dialog), m_stack.end());
        publish();
    } catch (...) {
    }
}

}}} // namespace Slic3r::GUI::Mcp
