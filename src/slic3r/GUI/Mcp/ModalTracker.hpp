#pragma once

#include <nlohmann/json.hpp>

#include <wx/gdicmn.h>
#include <wx/modalhook.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
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

// Whether the rule's pattern is found in "title\nmessage" or in "title\ntext", where `text` is the
// dialog's own text (describe_dialog()), or in the title alone for a dialog with no text (a web
// dialog), so ^Title$ matches it. A rule written against a dialog's wording finds it in its text,
// however its controls read in its message.
bool rule_matches(const AutoAnswerRule& rule, const std::string& title, const std::string& message, const std::string& text);

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

    // Rule sets are layered so a run can add its own and drop exactly those when it ends.
    // Returns the layer id to pass to pop_rules(). A layer pushed `paused` is never in force until resumed (set_paused()).
    // Any thread.
    int  push_rules(std::vector<AutoAnswerRule> rules, bool paused = false);
    void pop_rules(int layer);
    // A paused layer answers no dialog, and rule_in_force() is false for it, until it is resumed: a Run Script session's
    // rules act only while its work runs. Any thread.
    void set_paused(int layer, bool paused);
    nlohmann::json rules_json() const;
    // For an answer that runs or ends later (a queued press, a web dialog's start_web_answer()): whether
    // layer `layer` (0: any) still has the rule and is not paused. Any thread.
    bool rule_in_force(int layer, const std::string& pattern, const std::string& button) const;
    void count_use(int layer, const std::string& pattern, const std::string& button);

    // [{title, message, class, native, buttons:[{id,label}]}] of the open modals, outermost first.
    // On the UI thread the dialogs are described as they are now, and that is published; other
    // threads read the last description published. Any thread.
    nlohmann::json snapshot();
    bool           any_open() const;

    // What runs right before a rule presses a button, its wx one or its page's: as before any other
    // click, the presses mouse_gesture downs left on pages end first. UI thread only.
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
    std::set<int>                                          m_paused_layers;
    int                                                    m_next_layer = 1;
};

// {title, message, class, native, buttons:[{id,label,enabled?}], web?} for a dialog. A wx dialog's
// message reads it as a person does: its text, and its controls with their values and states, row by
// row (DialogReader in the .cpp); `enabled` is false for a disabled button. `web` is true for a dialog
// with a web view, whose page holds its controls (list_widgets root=dialog lists them). `text`, when
// given, receives the dialog's own text: its static text and html, one per line, without its
// controls, which auto-answer rules also search (rule_matches()). UI thread only.
nlohmann::json describe_dialog(wxDialog* dialog, std::string* text = nullptr);

// A piece of a dialog's message: what one child window, or one container's children, read as.
struct MessageCell
{
    wxRect      rect; // in the parent's coordinates
    std::string text; // one or more lines
    // A toggle with no label of its own (Orca's CheckBox and RadioBox): its mark, which the cell after
    // it on its row completes. `suffix` follows that label, and `window` is the toggle, named another
    // way when nothing completes it.
    bool        toggle = false;
    std::string suffix;
    wxWindow*   window = nullptr;
    // A control with its own mark and label (a labelled checkbox, a selector), which labels no toggle.
    bool        marked = false;
};
// A parent's cells in reading order, rows top to bottom and each row left to right, where two cells
// are on one row when either one's vertical centre lies within the other's span; a cell over twice as
// tall as the other shares its row only with a cell beside its first line or centred on it, so a column
// of text does not draw the rows beside it into one. Equal positions keep their order. In a row, a
// toggle takes the cell to its right as its label ("[x] Remember my choice."), or with nothing there
// the topmost one to its left ("Show splash screen | [x]"), and single-line cells side by side join
// with " | " ("Layer height | 0.2 | 0.28").
std::vector<MessageCell> layout_cells(std::vector<MessageCell> cells);
// A selector's options in one line, the selected one marked: "(*) A | ( ) B".
std::string selector_line(const std::vector<std::string>& options, int selected);
// Whether a setting's option key names a secret: a password, API key, token or secret.
bool is_secret_key(const std::string& key);

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
enum class DialogPress;
// Why `dialog` would not take a press of its wx button `id`: Disabled for a shown button that is
// disabled (an invalid name, an unticked box), Hidden for one only a page the dialog does not show
// has, the dialog's own way of saying not now. Cancel and Close on a hidden page are not refused: they
// end the dialog as they end one without them. nullopt when the press would go ahead.
std::optional<DialogPress> refused_press(wxDialog* dialog, int id);
// The id of the shown, enabled wx button a name, label or id stands for in `dialog`, or wxID_NONE:
// a web dialog whose rule names none of its wx buttons is answered on its page. UI thread only.
int native_dialog_button(wxDialog* dialog, const std::string& button);
// The standard id a button name stands for (yes, no, ok, cancel, close, apply) or a numeric id, or
// wxID_NONE.
int standard_dialog_button(const std::string& button);

// What press_dialog_button() did.
enum class DialogPress
{
    Pressed,  // the button's handler ran (a message box's press is queued to its box)
    Ended,    // no shown button has the id, so the dialog ended with it
    Disabled, // the button is shown but disabled: nothing happened
    Hidden,   // the button is only on a page the dialog does not show: nothing happened
    Failed,   // a message box that could not be pressed: it is no longer shown, or the press was not sent
};
// Presses the button with `id` in `dialog` so the dialog's own handler runs, or ends the dialog with
// that id when no such button exists, so `cancel` closes a dialog without a Cancel button. A button
// refused_press() refuses is neither pressed nor stood in for: the dialog disabled or hid it on
// purpose. A message box's button is pressed by message to its box, which closes once its own loop
// runs again; it has no other way to end. UI thread only, while the dialog is modal.
DialogPress press_dialog_button(wxDialog* dialog, int id);

// Demangled C++ class name of a window, e.g. "Slic3r::GUI::MessageDialog".
std::string class_name(const wxWindow* window);

}}} // namespace Slic3r::GUI::Mcp
