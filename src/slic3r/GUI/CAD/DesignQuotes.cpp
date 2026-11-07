#include "slic3r/GUI/CAD/DesignQuotes.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

constexpr double kPi = 3.14159265358979323846;

// World to canvas pixels, refusing a point behind the eye (it would come back mirrored).
struct Projector {
    Eigen::Matrix4d     to_clip;
    std::array<int, 4>  vp;
    Vec3d               eye;

    explicit Projector(const Camera& cam)
        : to_clip(cam.get_projection_matrix().matrix() * cam.get_view_matrix().matrix()), vp(cam.get_viewport()), eye(cam.get_position()) {}

    bool operator()(const Vec3d& w, ImVec2& out) const
    {
        const Eigen::Vector4d clip = to_clip * w.homogeneous();
        if (clip.w() <= 1e-9) return false;
        const Vec3d ndc = clip.head<3>() / clip.w();
        out = ImVec2(float(vp[0] + (ndc.x() * 0.5 + 0.5) * vp[2]), float(vp[1] + (1.0 - (ndc.y() * 0.5 + 0.5)) * vp[3]));
        return true;
    }
};

ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
ImVec2 operator*(ImVec2 a, float k) { return ImVec2(a.x * k, a.y * k); }
float  len(ImVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
ImVec2 unit(ImVec2 a) { const float l = len(a); return l > 1e-6f ? a * (1.f / l) : ImVec2(1.f, 0.f); }
ImVec2 perp(ImVec2 a) { return ImVec2(-a.y, a.x); }

struct Style {
    ImU32 line, text, chip, chip_hot, edge;
    float em, thin;
};

struct Rect { ImVec2 lo, hi; };
bool overlaps(const Rect& a, const Rect& b) { return a.lo.x < b.hi.x && b.lo.x < a.hi.x && a.lo.y < b.hi.y && b.lo.y < a.hi.y; }

// A filled arrowhead whose tip is at `tip`, the shaft behind it along -dir.
void arrow(ImDrawList* dl, ImVec2 tip, ImVec2 dir, const Style& s)
{
    const float L = 9.f * s.em, W = 3.2f * s.em;
    const ImVec2 back = tip - dir * L, n = perp(dir) * W;
    dl->AddTriangleFilled(tip, back + n, back - n, s.line);
}

// A dimension line from p0 to p1 with the arrows on the inside; too short for both, they point in from outside.
void dim_line(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Style& s)
{
    const ImVec2 d = unit(p1 - p0);
    dl->AddLine(p0, p1, s.line, s.thin);
    arrow(dl, p0, d * -1.f, s);
    arrow(dl, p1, d, s);
}

// The number, set in a break of the line, nudged down until it clears the numbers already placed. Returns the box it took.
Rect place_text(ImDrawList* dl, const std::string& text, ImVec2 at, std::vector<Rect>& taken, const Style& s, bool hot, bool dim_chip = true)
{
    const ImVec2 ts  = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pad(5.f * s.em, 2.f * s.em);
    Rect r{at - ts * 0.5f - pad, at + ts * 0.5f + pad};
    for (int tries = 0; tries < 8; ++tries) {
        bool clash = false;
        for (const Rect& o : taken) if (overlaps(r, o)) { clash = true; break; }
        if (!clash) break;
        const float dy = (r.hi.y - r.lo.y) + 2.f * s.em;
        r.lo.y += dy;
        r.hi.y += dy;
    }
    taken.push_back(r);
    dl->AddRectFilled(r.lo, r.hi, hot ? s.chip_hot : s.chip, 3.f * s.em);
    if (dim_chip) dl->AddRect(r.lo, r.hi, s.edge, 3.f * s.em, ImDrawCornerFlags_All, s.thin);
    dl->AddText(r.lo + pad, s.text, text.c_str());
    return r;
}

} // namespace

void DesignQuoteOverlay::render(ImGuiWrapper& imgui, const Camera& camera, float scale)
{
    (void) scale;
    m_hovered = false;
    if (!quotes) return;
    const std::vector<Quotes::Quote>& qs = quotes();
    if (qs.empty()) return;

    const Projector proj(camera);
    if (std::getenv("ORCA_CAD_UITRACE")) { ImVec2 t; const bool ok = proj(Vec3d(0, 0, 0), t); std::fprintf(stderr, "[UI] quotes n=%zu origin ok=%d (%.0f,%.0f) vp=%d,%d,%d,%d\n", qs.size(), int(ok), double(t.x), double(t.y), proj.vp[0], proj.vp[1], proj.vp[2], proj.vp[3]); }
    ImDrawList*     dl = ImGui::GetBackgroundDrawList();
    const float     em = imgui.get_style_scaling();

    Style st;
    st.em       = em;
    st.thin     = std::max(1.f, 1.1f * em);
    const ImVec4 accent = ImGuiWrapper::COL_ORCA;
    st.line     = ImGui::ColorConvertFloat4ToU32(accent);
    st.text     = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec4 bg   = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    bg.w        = 0.88f;
    st.chip     = ImGui::ColorConvertFloat4ToU32(bg);
    ImVec4 hot  = accent;
    hot.w       = 0.30f;
    st.chip_hot = ImGui::ColorConvertFloat4ToU32(ImVec4(bg.x * 0.7f + hot.x * 0.3f, bg.y * 0.7f + hot.y * 0.3f, bg.z * 0.7f + hot.z * 0.3f, 0.95f));
    ImVec4 edge = accent;
    edge.w      = 0.55f;
    st.edge     = ImGui::ColorConvertFloat4ToU32(edge);

    std::vector<Rect> taken;
    int               id = 0;

    // The window the number lives in: the only part of a quote that takes the mouse, and only when it can be edited.
    auto number = [&](const Quotes::Quote& q, const std::string& text, ImVec2 at) {
        const bool editable = q.feature >= 0 && bool(edit);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const ImVec2 pad(5.f * em, 2.f * em);
        const ImVec2 size = ts + pad * 2.f;
        bool hot_now = false;
        if (editable) {
            ImGui::SetNextWindowPos(ImVec2(at.x - size.x * 0.5f, at.y - size.y * 0.5f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
            if (ImGui::Begin(("##quote" + std::to_string(id)).c_str(), nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
                ImGui::InvisibleButton("##hit", size);
                hot_now = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) edit(q.feature);
                if (hot_now) ImGui::SetTooltip("%s", "Click to edit");
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            if (hot_now) m_hovered = true;
        }
        place_text(dl, text, at, taken, st, hot_now);
        ++id;
    };

    // ---- the overall size: three lines on the body's box, on the sides facing the eye ---------------------------------
    for (const Quotes::Quote& q : qs) {
        if (q.kind != Quotes::Kind::Extent) continue;
        const Vec3d lo = q.a, hi = q.b, c = 0.5 * (lo + hi);
        const double diag   = (hi - lo).norm();
        const double off    = 0.10 * diag + 1.0;
        const double gap    = 0.012 * diag, over = 0.02 * diag;
        const double sx     = proj.eye.x() > c.x() ? 1.0 : -1.0;
        const double sy     = proj.eye.y() > c.y() ? 1.0 : -1.0;
        const double sz     = proj.eye.z() > c.z() ? -1.0 : 1.0;   // the base nearer the eye: the bottom when seen from above
        const double zface  = sz < 0 ? lo.z() : hi.z();
        const double xface  = sx > 0 ? hi.x() : lo.x();
        const double yface  = sy > 0 ? hi.y() : lo.y();
        Vec3d  p0, p1, out;
        if (q.axis == 0)      { p0 = {lo.x(), yface, zface}; p1 = {hi.x(), yface, zface}; out = Vec3d(0, sy, sz).normalized(); }
        else if (q.axis == 1) { p0 = {xface, lo.y(), zface}; p1 = {xface, hi.y(), zface}; out = Vec3d(sx, 0, sz).normalized(); }
        else {
            // the vertical: on the corner that reads leftmost on screen, so the number is not drawn across the body
            double best = 1e300;
            Vec3d  cx(lo.x(), lo.y(), 0);
            for (double X : {lo.x(), hi.x()})
                for (double Y : {lo.y(), hi.y()}) {
                    ImVec2 s;
                    if (proj({X, Y, c.z()}, s) && s.x < best) { best = s.x; cx = {X, Y, 0}; }
                }
            p0  = {cx.x(), cx.y(), lo.z()};
            p1  = {cx.x(), cx.y(), hi.z()};
            out = Vec3d(cx.x() > c.x() ? 1 : -1, cx.y() > c.y() ? 1 : -1, 0).normalized();
        }
        const Vec3d dir = (p1 - p0).normalized();
        ImVec2 s0, s1, e0, e1, x0, x1;
        if (!proj(p0 + out * off, s0) || !proj(p1 + out * off, s1)) continue;   // the dimension line itself
        if (!proj(p0 + out * gap, e0) || !proj(p0 + out * (off + over), x0)) continue;   // its extension lines
        if (!proj(p1 + out * gap, e1) || !proj(p1 + out * (off + over), x1)) continue;
        if (len(s1 - s0) < 14.f * em) continue;
        dl->AddLine(e0, x0, st.line, st.thin);
        dl->AddLine(e1, x1, st.line, st.thin);
        dim_line(dl, s0, s1, st);
        (void) dir;
        number(q, q.text(), (s0 + s1) * 0.5f);
    }

    // ---- holes, shafts and rounds: at the end of the cylinder nearer the eye -------------------------------------------
    for (const Quotes::Quote& q : qs) {
        if (q.kind == Quotes::Kind::Extent || q.spots.empty()) continue;
        // the instance nearest the eye (a pattern of holes is dimensioned once)
        const Quotes::Spot* spot = &q.spots.front();
        double              dmin = 1e300;
        for (const Quotes::Spot& sp : q.spots) {
            const double d = std::min((sp.c0 - proj.eye).norm(), (sp.c1 - proj.eye).norm());
            if (d < dmin) { dmin = d; spot = &sp; }
        }
        const bool   near0 = (spot->c0 - proj.eye).norm() <= (spot->c1 - proj.eye).norm();
        const Vec3d  c     = near0 ? spot->c0 : spot->c1;
        const Vec3d  m     = near0 ? spot->m0 : spot->m1;
        const double r     = q.value;
        ImVec2 sc;
        if (!proj(c, sc)) continue;

        if (q.kind == Quotes::Kind::Diameter) {
            // the in-plane direction that reads longest on screen (the major axis of the circle's ellipse)
            Vec3d e1 = std::abs(spot->axis.z()) < 0.9 ? spot->axis.cross(Vec3d::UnitZ()).normalized() : spot->axis.cross(Vec3d::UnitX()).normalized();
            Vec3d e2 = spot->axis.cross(e1).normalized();
            ImVec2 a1, a2, b1, b2;
            if (!proj(c + e1 * r, a1) || !proj(c - e1 * r, a2) || !proj(c + e2 * r, b1) || !proj(c - e2 * r, b2)) continue;
            const ImVec2 v1 = (a1 - a2) * 0.5f, v2 = (b1 - b2) * 0.5f;
            const double th = 0.5 * std::atan2(2.0 * (v1.x * v2.x + v1.y * v2.y), double(v1.x * v1.x + v1.y * v1.y - v2.x * v2.x - v2.y * v2.y));
            Vec3d d = std::cos(th) * e1 + std::sin(th) * e2;
            ImVec2 p0, p1;
            if (!proj(c - d * r, p0) || !proj(c + d * r, p1)) continue;
            if (p1.y > p0.y || (p1.y == p0.y && p1.x < p0.x)) { std::swap(p0, p1); d = -d; }   // the leader goes up and out
            if (len(p1 - p0) < 10.f * em) continue;
            // the circle itself, so the eye finds the hole the number belongs to
            ImVec2 prev{};
            bool   have = false;
            for (int i = 0; i <= 48; ++i) {
                const double a = 2.0 * kPi * i / 48.0;
                ImVec2 p;
                if (!proj(c + (std::cos(a) * e1 + std::sin(a) * e2) * r, p)) { have = false; continue; }
                if (have) dl->AddLine(prev, p, st.edge, st.thin);
                prev = p;
                have = true;
            }
            const ImVec2 dir  = unit(p1 - p0);
            const ImVec2 tail = p1 + dir * (26.f * em);
            dl->AddLine(p0, tail, st.line, st.thin);
            arrow(dl, p0, dir * -1.f, st);
            arrow(dl, p1, dir, st);
            const ImVec2 ts = ImGui::CalcTextSize(q.text().c_str());
            number(q, q.text(), tail + ImVec2(ts.x * 0.5f + 8.f * em, 0.f));
        } else {
            ImVec2 sm;
            if (!proj(m, sm) || len(sm - sc) < 6.f * em) continue;
            const ImVec2 dir  = unit(sm - sc);
            const ImVec2 tail = sm + dir * (26.f * em);
            dl->AddLine(sc, tail, st.line, st.thin);
            arrow(dl, sm, dir, st);
            dl->AddCircleFilled(sc, 2.f * em, st.line);   // the centre of the arc
            const ImVec2 ts = ImGui::CalcTextSize(q.text().c_str());
            number(q, q.text(), tail + ImVec2(dir.x >= 0 ? ts.x * 0.5f + 8.f * em : -(ts.x * 0.5f + 8.f * em), 0.f));
        }
    }
}

} } // namespace Slic3r::GUI
