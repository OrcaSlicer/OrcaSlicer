#include "slic3r/GUI/CAD/SketchInlineEditor.hpp"

#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Color.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>   // BringWindowToDisplayFront / GetCurrentWindow

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Slic3r {
namespace GUI {

namespace {

// Numbers are typed and shown with a POINT, whatever the locale: this field feeds a CAD kernel,
// and a decimal comma reaching it as a thousands separator is a silent order-of-magnitude error.
// Parsing accepts either separator because a keyboard's numeric pad may only offer one.
std::string fmt_value(double v, int digits = 2)
{
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    for (char* c = buf; *c; ++c)
        if (*c == ',') *c = '.';
    return std::string(buf);
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
// SNAPORCA_KEYTRACE: that one is a debugging firehose, this one is an assertion surface and its
// format is a contract the script parses.
//
// The pair that matters is `open` vs `commit`: the check always types a value DIFFERENT from the
// prefill, so a field that is on screen but not editable commits its prefill and the two lines
// disagree. A focus flag cannot show that — it read 0 even when typing worked — but the number
// the user actually gets can.
void ux_trace(const char* event, const std::string& title, const std::string& detail)
{
    if (!std::getenv("SNAPORCA_UXTRACE")) return;
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

void SketchInlineEditor::do_commit()
{
    double v = 0.0;
    if (!parse_value(m_buf, v)) {
        // Refusing input in silence is indistinguishable from the app having frozen: the field
        // just sits there and the user has no idea what it wants. Say so in the title line and
        // keep editing.
        ux_trace("refused", m_title, std::string("typed=") + m_buf);
        m_err = (m_buf[0] == '\0') ? into_u8(_L("Enter a number")) : into_u8(_L("Not a number"));
        m_focus_pending = true;
        return;
    }
    ux_trace("commit", m_title, std::string("typed=") + m_buf + " value=" + fmt_value(v, 4));
    auto cb = m_commit;
    close();
    // AFTER close(): the callback may open the next queued dimension (a rectangle queues Width
    // then Height), and doing that into a field that still believes it is open would drop the
    // second one's prefill on the floor.
    if (cb) cb(v);
}

bool SketchInlineEditor::render(ImGuiWrapper& imgui, float scale)
{
    if (!m_open) return false;

    ImGuiWrapper::push_common_window_style(scale);
    imgui.set_next_window_pos((float) m_anchor.x, (float) m_anchor.y, ImGuiCond_Always, 0.5f, 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
    // NoInputs is what every other sketch overlay sets and is exactly what this one must not:
    // it is the only overlay in the tab that the user types into.
    imgui.begin(std::string("##sketchvalue"),
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration
                    | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

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
    const bool entered = ImGui::InputText("##sketchvalue_in", m_buf, sizeof(m_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue
                                              | ImGuiInputTextFlags_AutoSelectAll
                                              | ImGuiInputTextFlags_CharsDecimal);
    // MEASUREMENT, not a fix: one line per frame saying whether ImGui believes it owns the
    // keyboard and whether our widget is the active one. "Typing does not arrive" has two very
    // different causes — no FRAMES (this canvas repaints on demand only, so an idle canvas never
    // processes ImGui's queued characters) versus frames that run while the input is not active —
    // and they are indistinguishable from outside.
    if (std::getenv("SNAPORCA_UXTRACE")) {
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
    if (entered)
        do_commit();
    else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
        do_cancel();
    return true;
}

}} // namespace Slic3r::GUI
