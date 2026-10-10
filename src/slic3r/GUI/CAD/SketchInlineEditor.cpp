#include "slic3r/GUI/CAD/SketchInlineEditor.hpp"

#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Color.hpp"

#include <functional>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>   // FindWindowByName / BringWindowToDisplayFront

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <wx/gdicmn.h>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

const char* const kWindowName = "##sketchvalue";

// Numbers are typed and shown with a POINT, whatever the locale: this field feeds a CAD kernel,
// and a decimal comma reaching it as a thousands separator is a silent order-of-magnitude error.
// Parsing accepts either separator because a keyboard's numeric pad may only offer one.
// Trailing zeros are dropped, so a count opens as "3" and a length as "37.457", not "37.46".
std::string fmt_value(double v, int digits = 3)
{
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    for (char* c = buf; *c; ++c)
        if (*c == ',') *c = '.';
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

bool parse_value(const char* text, double& out)
{
    if (text == nullptr) return false;
    std::string t(text);
    for (char& c : t)
        if (c == ',') c = '.';
    // strtod, not std::stod: no exceptions, and `end` tells us whether the WHOLE field was a
    // number. "12mm" must be refused, not silently read as 12.
    const char* b = t.c_str();
    char* end = nullptr;
    const double v = std::strtod(b, &end);
    if (end == b) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    out = v;
    return true;
}

// One machine-readable line per event of the click-edit contract, for the UX check that runs
// after every build (scripts/CAD/check-gui-click-edit.py). Deliberately NOT the same switch as
// ORCA_CAD_KEYTRACE: that one is a debugging firehose, this one is an assertion surface and its
// format is a contract the script parses.
//
// The pair that matters is `open` vs `commit`: the check always types a value DIFFERENT from the
// prefill, so a field that is on screen but not editable commits its prefill and the two lines
// disagree. A focus flag cannot show that — it read 0 even when typing worked — but the number
// the user actually gets can.
void ux_trace(const char* event, const std::string& title, const std::string& detail)
{
    if (!std::getenv("ORCA_CAD_UXTRACE")) return;
    std::fprintf(stderr, "[UX] %s title=%s %s\n", event, title.c_str(), detail.c_str());
    std::fflush(stderr);
}

} // namespace

void SketchInlineEditor::open(const wxPoint& canvas_px, double value, const std::string& title,
                              std::function<void(double)> on_commit,
                              std::function<void()> on_cancel)
{
    m_anchor = canvas_px;
    m_title  = title;
    m_err.clear();
    m_commit = std::move(on_commit);
    m_cancel = std::move(on_cancel);
    const std::string v = fmt_value(value);
    std::snprintf(m_buf, sizeof(m_buf), "%s", v.c_str());
    m_prefill_value = value;
    m_prefill_text  = v;
    m_active        = false;
    m_open          = true;
    // ImGui takes keyboard focus for one frame on request; asking on the frame the field first
    // appears is what makes typing land without a click. There is no window manager to consult.
    m_focus_pending = true;
    ux_trace("open", m_title, "prefill=" + v);
}

void SketchInlineEditor::close()
{
    m_open          = false;
    m_focus_pending = false;
    m_active        = false;
    m_commit        = nullptr;
    m_cancel        = nullptr;
    m_err.clear();
}

void SketchInlineEditor::cancel()
{
    if (m_open) do_cancel();
}

void SketchInlineEditor::commit()
{
    if (m_open) do_commit();
}

void SketchInlineEditor::do_cancel()
{
    ux_trace("cancel", m_title, "");
    auto cb = m_cancel;
    close();
    if (cb) cb();
}

bool SketchInlineEditor::refuse(const std::string& why)
{
    // Only meaningful while a commit callback runs and before it opened another field.
    if (!m_in_commit || m_open) return false;
    m_anchor        = m_last.anchor;
    m_title         = m_last.title;
    m_commit        = m_last.commit;
    m_cancel        = m_last.cancel;
    m_prefill_value = m_last.prefill_value;
    m_prefill_text  = m_last.prefill_text;
    std::snprintf(m_buf, sizeof(m_buf), "%s", m_last.buf.c_str());
    m_err           = why;
    m_open          = true;
    m_focus_pending = true;
    ux_trace("refused", m_title, "reason=" + why);
    return true;
}

void SketchInlineEditor::do_commit()
{
    double v = 0.0;
    // An untouched field commits the value it opened with, not its rounded display: Enter on a
    // 37.4567 line must not turn it into a 37.457 driving dimension.
    if (m_prefill_text == m_buf)
        v = m_prefill_value;
    else if (!parse_value(m_buf, v)) {
        // Refusing input in silence is indistinguishable from the app having frozen: the field
        // just sits there and the user has no idea what it wants. Say so in the title line and
        // keep editing.
        ux_trace("refused", m_title, std::string("typed=") + m_buf);
        m_err = (m_buf[0] == '\0') ? _u8L("Enter a number") : _u8L("Not a number");
        m_focus_pending = true;
        return;
    }
    ux_trace("commit", m_title, std::string("typed=") + m_buf + " value=" + fmt_value(v, 4));
    auto cb = m_commit;
    m_last = Closed{ m_anchor, m_title, std::string(m_buf), m_commit, m_cancel, m_prefill_value, m_prefill_text };
    close();
    // AFTER close(): the callback may open the next queued dimension (a rectangle queues Width
    // then Height), and doing that into a field that still believes it is open would drop the
    // second one's prefill on the floor.
    m_in_commit = true;
    if (cb) cb(v);
    m_in_commit = false;
    m_last = Closed{};
}

bool SketchInlineEditor::render(ImGuiWrapper& imgui, float scale)
{
    if (!m_open) return false;

    ImGuiWrapper::push_common_window_style(scale);
    imgui.set_next_window_pos((float) m_anchor.x, (float) m_anchor.y, ImGuiCond_Always, 0.5f, 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
    // NoInputs is what every other sketch overlay sets and is exactly what this one must not:
    // it is the only overlay in the tab that the user types into.
    imgui.begin(std::string(kWindowName),
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration
                    | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    if (!m_title.empty() || !m_err.empty()) {
        if (m_err.empty()) {
            imgui.text(m_title);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(ColorRGBA(0.91f, 0.42f, 0.42f, 1.0f)));
            imgui.text(m_err);
            ImGui::PopStyleColor();
        }
    }

    if (m_focus_pending) {
        ImGui::SetKeyboardFocusHere();
        m_focus_pending = false;
    }
    ImGui::PushItemWidth(90.0f * scale);
    // EnterReturnsTrue so Enter commits from inside the widget; AutoSelectAll so the prefill is
    // replaced by the first digit typed, which is what "pre-selected" meant when this was a
    // wxTextCtrl and is what makes typing a value a single gesture.
    // No CharsDecimal: it lets only the LOCALE's decimal separator through, so in a C locale a
    // comma could not be typed at all, and it admits '*' and '/' that the parser then refuses.
    // parse_value() is the one judge of what is a number.
    const bool entered = ImGui::InputText("##sketchvalue_in", m_buf, sizeof(m_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue
                                              | ImGuiInputTextFlags_AutoSelectAll);
    m_active = ImGui::IsItemActive();
    // Tab accepts the value like Enter does (and moves on to the next queued field, if any).
    const bool tabbed = m_active && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Tab));
    // MEASUREMENT, not a fix: one line per frame saying whether ImGui believes it owns the
    // keyboard and whether our widget is the active one. "Typing does not arrive" has two very
    // different causes — no FRAMES (this canvas repaints on demand only, so an idle canvas never
    // processes ImGui's queued characters) versus frames that run while the input is not active —
    // and they are indistinguishable from outside.
    if (std::getenv("ORCA_CAD_UXTRACE")) {
        const ImGuiIO& io = ImGui::GetIO();
        std::fprintf(stderr, "[UX] frame title=%s want_text=%d want_kb=%d active=%d buf=%s\n",
                     m_title.c_str(), (int) io.WantTextInput, (int) io.WantCaptureKeyboard,
                     (int) ImGui::IsItemActive(), m_buf);
        std::fflush(stderr);
    }
    ImGui::PopItemWidth();
    imgui.end();
    ImGui::PopStyleVar();
    ImGuiWrapper::pop_common_window_style();

    // Keep the frames coming while the field is up — see request_frame's note in the header.
    if (m_open && request_frame)
        request_frame();

    // Act AFTER end(): do_commit can reopen the field for the next queued dimension, and that
    // must not happen inside this frame's window.
    if (entered || tabbed)
        do_commit();
    else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
        do_cancel();
    return true;
}

void SketchInlineEditor::bring_to_front()
{
    // Not gated on m_open: a commit closes the field after it was drawn, and that frame still
    // shows it. A window that did not Begin this frame is not rendered, so lifting it is inert.
    if (ImGuiWindow* w = ImGui::FindWindowByName(kWindowName))
        ImGui::BringWindowToDisplayFront(w);
}

}} // namespace Slic3r::GUI
