#pragma once

#include <nlohmann/json.hpp>

#include <wx/modalhook.h>

#include <cstdint>
#include <functional>
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
    std::string pattern; // ECMAScript regex, case-insensitive (rule_matches())
    std::string button;  // yes|no|ok|cancel|close|apply|<button label>|<numeric id>
    std::regex  compiled;
    int         uses = 0; // how often it answered a dialog
};

// Builds a rule, compiling its pattern. Throws ToolFailure on an invalid pattern.
AutoAnswerRule make_rule(const std::string& pattern, const std::string& button);

// Whether the rule's pattern is found in "title\nmessage", or also in the title alone for a dialog
// with no message (a web dialog), so ^Title$ matches it.
bool rule_matches(const AutoAnswerRule& rule, const std::string& title, const std::string& message);

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
    // A number for each dialog while it is open, never reused, so a dialog that closed is told apart
    // from another opened since at its address (0 for one not open). UI thread only.
    std::uint64_t serial(const wxDialog* dialog) const;
    bool          is_open(std::uint64_t serial) const;

    // Rule sets are layered so a script can add its own and drop exactly those when it ends.
    // Returns the layer id to pass to pop_rules(). Any thread.
    int  push_rules(std::vector<AutoAnswerRule> rules);
    void pop_rules(int layer);
    nlohmann::json rules_json() const;
    // For a web dialog's answer, which ends later (start_web_answer()): whether layer `layer` still
    // has the rule, and counting its use. Any thread.
    bool rule_in_force(int layer, const std::string& pattern, const std::string& button) const;
    void count_use(int layer, const std::string& pattern, const std::string& button);

    // [{title, message, class, native, buttons:[{id,label}]}] of the open modals, outermost first.
    // Any thread.
    nlohmann::json snapshot() const;
    bool           any_open() const;

    // What runs right before a rule presses a button, its wx one or its page's: as before any other
    // click, the presses ui_mouse downs left on pages end first. UI thread only.
    void set_before_press(std::function<void()> fn) { m_before_press = std::move(fn); }
    void before_press() const
    {
        if (m_before_press)
            m_before_press();
    }

protected:
    int  Enter(wxDialog* dialog) override;
    void Exit(wxDialog* dialog) override;

private:
    void publish();
    void push(wxDialog* dialog);

    std::vector<wxDialog*>     m_stack;
    std::vector<std::uint64_t> m_serials; // m_stack's dialogs' serials
    std::uint64_t              m_last_serial = 0;
    std::function<void()>      m_before_press;

    mutable std::mutex                                     m_mutex;
    nlohmann::json                                         m_snapshot = nlohmann::json::array();
    std::vector<std::pair<int, std::vector<AutoAnswerRule>>> m_layers;
    int                                                    m_next_layer = 1;
};

// {title, message, class, native, buttons:[{id,label}], web?} for a dialog; `web` is true for a
// dialog with a web view, whose page holds its controls (ui_snapshot root=dialog lists them). UI
// thread only.
nlohmann::json describe_dialog(wxDialog* dialog);

// True for dialogs implemented by the OS (wxMessageDialog, file and directory pickers): they have
// no wx children to inspect or click. An open message box can still be pressed on Windows and Linux
// (message_box_buttons()); a picker can only be pre-answered.
bool is_native_dialog(wxDialog* dialog);
// True for file and directory pickers, which auto-answer rules can only cancel.
bool is_picker(wxDialog* dialog);
// The buttons [{id, label}] of a native message box (wxMessageDialog) that can be pressed while it is
// open, which describe_dialog() lists and the dialog functions below press by message to the OS's box.
// Empty for any other dialog, and on macOS, where wx runs no events while an alert is open, so no
// call reaches one. UI thread only.
nlohmann::json message_box_buttons(wxDialog* dialog);

// The id a button name or label stands for in `dialog`, or wxID_NONE. UI thread only.
int resolve_dialog_button(wxDialog* dialog, const std::string& button);
// The id of `dialog`'s wx button, or its message box button, labelled `label` (mnemonics and case
// aside), or wxID_NONE.
int dialog_button_by_label(wxDialog* dialog, const std::string& label);
// Whether `dialog` has a shown, enabled wx button with `id`, or a message box button.
bool has_dialog_button(wxDialog* dialog, int id);
// The id of the shown, enabled wx button a name, label or id stands for in `dialog`, or wxID_NONE:
// a web dialog whose rule names none of its wx buttons is answered on its page. UI thread only.
int native_dialog_button(wxDialog* dialog, const std::string& button);
// The standard id a button name stands for (yes, no, ok, cancel, close, apply) or a numeric id, or
// wxID_NONE.
int standard_dialog_button(const std::string& button);

// Presses the button with `id` in `dialog` so the dialog's own handler runs, or ends the dialog with
// that id when no such button exists. A message box's button is pressed by message to its box, which
// closes once its own loop runs again; it has no other way to end. Returns whether a button was
// pressed. UI thread only, while the dialog is modal.
bool press_dialog_button(wxDialog* dialog, int id);

// Demangled C++ class name of a window, e.g. "Slic3r::GUI::MessageDialog".
std::string class_name(const wxWindow* window);

}}} // namespace Slic3r::GUI::Mcp
