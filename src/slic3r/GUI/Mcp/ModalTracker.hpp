#pragma once

#include <nlohmann/json.hpp>

#include <wx/modalhook.h>

#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

class wxDialog;
class wxWindow;

namespace Slic3r { namespace GUI { namespace Mcp {

// A pre-registered answer for modal dialogs whose title or message matches `pattern`.
struct AutoAnswerRule
{
    std::string pattern; // ECMAScript regex, case-insensitive, searched in "title\nmessage"
    std::string button;  // yes|no|ok|cancel|close|apply|<button label>|<numeric id>
    std::regex  compiled;
    int         uses = 0; // how often it answered a dialog
};

// Builds a rule, compiling its pattern. Throws ToolFailure on an invalid pattern.
AutoAnswerRule make_rule(const std::string& pattern, const std::string& button);

// Every modal dialog shown through ShowModal - Orca's own, wxMessageDialog and the native file
// dialogs alike - passes through this hook. It keeps the stack of open modals for the guards and
// the tools, answers dialogs that match a registered rule, and publishes a description snapshot
// that other threads may read. A matching native dialog is answered without showing; a matching wx
// dialog opens and has the rule's button pressed (press_dialog_button()), so its handler runs.
class ModalTracker final : public wxModalDialogHook
{
public:
    ModalTracker();
    ~ModalTracker() override;

    // UI thread only.
    const std::vector<wxDialog*>& stack() const { return m_stack; }
    wxDialog*                     top() const { return m_stack.empty() ? nullptr : m_stack.back(); }

    // Rule sets are layered so a script can add its own and drop exactly those when it ends.
    // Returns the layer id to pass to pop_rules(). Any thread.
    int  push_rules(std::vector<AutoAnswerRule> rules);
    void pop_rules(int layer);
    nlohmann::json rules_json() const;

    // [{title, message, class, native, buttons:[{id,label}]}] of the open modals, outermost first.
    // Any thread.
    nlohmann::json snapshot() const;
    bool           any_open() const;

protected:
    int  Enter(wxDialog* dialog) override;
    void Exit(wxDialog* dialog) override;

private:
    void publish();

    std::vector<wxDialog*> m_stack;

    mutable std::mutex                                     m_mutex;
    nlohmann::json                                         m_snapshot = nlohmann::json::array();
    std::vector<std::pair<int, std::vector<AutoAnswerRule>>> m_layers;
    int                                                    m_next_layer = 1;
};

// {title, message, class, native, buttons:[{id,label}]} for a dialog. UI thread only.
nlohmann::json describe_dialog(wxDialog* dialog);

// True for dialogs implemented by the OS (wxMessageDialog, file and directory pickers): they have
// no wx children to inspect or click and can only be pre-answered.
bool is_native_dialog(wxDialog* dialog);
// True for file and directory pickers, which auto-answer rules can only cancel.
bool is_picker(wxDialog* dialog);

// The id a button name or label stands for in `dialog`, or wxID_NONE. UI thread only.
int resolve_dialog_button(wxDialog* dialog, const std::string& button);

// Presses the button with `id` in `dialog` so the dialog's own handler runs, or ends the dialog with
// that id when no such button exists. Returns whether a button was pressed. UI thread only, while
// the dialog is modal.
bool press_dialog_button(wxDialog* dialog, int id);

// Demangled C++ class name of a window, e.g. "Slic3r::GUI::MessageDialog".
std::string class_name(const wxWindow* window);

}}} // namespace Slic3r::GUI::Mcp
