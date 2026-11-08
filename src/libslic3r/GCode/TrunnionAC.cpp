#include "TrunnionAC.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {
namespace TrunnionAC {

namespace {

constexpr double kPi = 3.14159265358979323846;
inline double rad(double d) { return d * kPi / 180.; }
inline double deg(double r) { return r * 180. / kPi; }

// d folded into (-180, 180]
inline double wrap180(double d)
{
    d = std::fmod(d, 360.);
    if (d <= -180.) d += 360.;
    else if (d > 180.) d -= 360.;
    return d;
}

inline Vec3   operator+(const Vec3 &a, const Vec3 &b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3   operator-(const Vec3 &a, const Vec3 &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3   operator*(const Vec3 &a, double s) { return { a.x * s, a.y * s, a.z * s }; }
inline double dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double norm(const Vec3 &a) { return std::sqrt(dot(a, a)); }

inline Vec3 mul(const std::array<double, 9> &R, const Vec3 &v)
{
    return { R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z, R[6] * v.x + R[7] * v.y + R[8] * v.z };
}

// The tool axis an orientation points straight up: the third row of Rx(A) * Rz(C), or for an A/B
// machine of Rx(A) * Ry(B).
inline Vec3 axis_of(const Machine &m, const Orientation &o)
{
    const double a = rad(o.a), c = rad(o.c);
    if (m.kind == Kind::AB)
        return { -std::cos(a) * std::sin(c), std::sin(a), std::cos(a) * std::cos(c) };
    return { std::sin(a) * std::sin(c), std::sin(a) * std::cos(c), std::cos(a) };
}

// Machine Z of a G-code point at one orientation; the Z row of to_machine(), kept cheap because the
// swing evaluates it for every column of the part at every sampled angle.
struct ZRow
{
    double sa, ca, sc, cc;
    ZRow(const Orientation &o) : sa(std::sin(rad(o.a))), ca(std::cos(rad(o.a))), sc(std::sin(rad(o.c))), cc(std::cos(rad(o.c))) {}
    double z(const Machine &m, const Vec3 &p) const
    {
        const double px = p.x - m.origin_x - m.c_offset_x, py = p.y - m.origin_y - m.c_offset_y;
        if (m.kind == Kind::AB) {
            // Rx(A) * Ry(B) about the pivot (sc, cc hold sin B, cos B): Z row only.
            const double pz = p.z - m.pivot_height;
            return sa * py + ca * (-sc * px + cc * pz) + m.pivot_height;
        }
        return sa * (sc * px + cc * py + m.c_offset_y) + ca * (p.z - m.pivot_height) + m.pivot_height;
    }
};

std::string fmt(double v, int digits)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (! s.empty() && s.back() == '0') s.pop_back();
        if (! s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

constexpr int XYZ_DIGITS = 3;
constexpr int E_DIGITS   = 5;
constexpr int AC_DIGITS  = 3;

// ------------------------------------------------------------------------------------------------
// Material laid so far, in G-code coordinates.

inline int64_t key3(int64_t i, int64_t j, int64_t k)
{
    constexpr int64_t off = int64_t(1) << 20;
    return ((i + off) << 42) | ((j + off) << 21) | (k + off);
}
inline int64_t key2(int64_t i, int64_t j) { return key3(i, j, 0); }
inline int     cell(double v, double size) { return int(std::floor(v / size)); }

struct Material
{
    static constexpr double VOXEL  = 1.;   // dedupe grid for the envelope checks, mm
    static constexpr double BLOCK  = 8.;   // bucket for the envelope checks' spatial lookup, mm
    static constexpr double COLUMN = 2.;   // column grid for the swing, mm

    struct Sample { Vec3 p; uint32_t section; };
    std::unordered_set<int64_t>                     voxels;
    std::unordered_map<int64_t, std::vector<Sample>> blocks;
    struct Column { Vec3 top, bottom; };
    // The highest and lowest sample of every vertical column. Any other sample of a column lies
    // between those two up to the column width, so the swing only has to look at these: the machine Z
    // of a rigidly moved point is linear in the point, and its maximum over a set is reached on the
    // set's convex hull.
    std::unordered_map<int64_t, Column>             columns;

    bool empty() const { return voxels.empty(); }

    void add(const Vec3 &p, uint32_t section)
    {
        if (! voxels.insert(key3(cell(p.x, VOXEL), cell(p.y, VOXEL), cell(p.z, VOXEL))).second)
            return;
        blocks[key3(cell(p.x, BLOCK), cell(p.y, BLOCK), cell(p.z, BLOCK))].push_back({ p, section });
        auto [it, fresh] = columns.try_emplace(key2(cell(p.x, COLUMN), cell(p.y, COLUMN)), Column{ p, p });
        if (! fresh) {
            if (p.z > it->second.top.z)    it->second.top = p;
            if (p.z < it->second.bottom.z) it->second.bottom = p;
        }
    }
    void add_segment(const Vec3 &a, const Vec3 &b, uint32_t section)
    {
        const double len = norm(b - a);
        const int    n   = std::max(1, int(std::ceil(len / VOXEL)));
        for (int i = 0; i <= n; ++ i)
            add(a + (b - a) * (double(i) / n), section);
    }
};

} // namespace

// ------------------------------------------------------------------------------------------------

std::array<double, 9> rotation(const Orientation &o)
{
    const double sa = std::sin(rad(o.a)), ca = std::cos(rad(o.a)), sc = std::sin(rad(o.c)), cc = std::cos(rad(o.c));
    return { cc, -sc, 0., ca * sc, ca * cc, -sa, sa * sc, sa * cc, ca };
}

std::array<double, 9> rotation(const Machine &m, const Orientation &o)
{
    if (m.kind != Kind::AB)
        return rotation(o);
    // Rx(A) * Ry(B), B held in o.c.
    const double sa = std::sin(rad(o.a)), ca = std::cos(rad(o.a)), sb = std::sin(rad(o.c)), cb = std::cos(rad(o.c));
    return { cb, 0., sb, sa * sb, ca, -sa * cb, -ca * sb, sa, ca * cb };
}

Vec3 to_machine(const Machine &m, const Orientation &o, const Vec3 &p)
{
    const double sa = std::sin(rad(o.a)), ca = std::cos(rad(o.a)), sc = std::sin(rad(o.c)), cc = std::cos(rad(o.c));
    if (m.kind == Kind::AB) {
        // About the pivot: B (sc, cc) about Y first, then A about X.
        const double px = p.x - m.origin_x - m.c_offset_x, py = p.y - m.origin_y - m.c_offset_y, pz = p.z - m.pivot_height;
        const double qx = cc * px + sc * pz, qy = py, qz = -sc * px + cc * pz;
        return { qx + m.origin_x + m.c_offset_x, ca * qy - sa * qz + m.origin_y + m.c_offset_y, sa * qy + ca * qz + m.pivot_height };
    }
    // Workpiece coordinates relative to the C axis, turned by C, back to the bed centre, then about
    // the A axis at the pivot height.
    const double px = p.x - m.origin_x - m.c_offset_x, py = p.y - m.origin_y - m.c_offset_y;
    const double qx = cc * px - sc * py + m.c_offset_x;
    const double qy = sc * px + cc * py + m.c_offset_y;
    const double qz = p.z - m.pivot_height;
    return { qx + m.origin_x, ca * qy - sa * qz + m.origin_y, sa * qy + ca * qz + m.pivot_height };
}

Vec3 to_gcode(const Machine &m, const Orientation &o, const Vec3 &p)
{
    const double sa = std::sin(rad(o.a)), ca = std::cos(rad(o.a)), sc = std::sin(rad(o.c)), cc = std::cos(rad(o.c));
    if (m.kind == Kind::AB) {
        // Undo A, then B, about the pivot.
        const double mx = p.x - m.origin_x - m.c_offset_x, my = p.y - m.origin_y - m.c_offset_y, mz = p.z - m.pivot_height;
        const double qx = mx, qy = ca * my + sa * mz, qz = -sa * my + ca * mz;
        return { cc * qx - sc * qz + m.origin_x + m.c_offset_x, qy + m.origin_y + m.c_offset_y, sc * qx + cc * qz + m.pivot_height };
    }
    const double mx = p.x - m.origin_x, my = p.y - m.origin_y, mz = p.z - m.pivot_height;
    // Undo A, then C.
    const double qx = mx, qy = ca * my + sa * mz, qz = -sa * my + ca * mz;
    const double px = qx - m.c_offset_x, py = qy - m.c_offset_y;
    return { cc * px + sc * py + m.c_offset_x + m.origin_x, -sc * px + cc * py + m.c_offset_y + m.origin_y, qz + m.pivot_height };
}

bool solve(const Machine &m, const Vec3 &n_in, const Orientation &prev, Orientation &out)
{
    const double len = norm(n_in);
    if (! (len > 1e-12))
        return false;
    const Vec3   n   = n_in * (1. / len);
    if (m.kind == Kind::AB) {
        // n = (-cosA sinB, sinA, cosA cosB): one pose with |A| <= 90 (the other, A' = 180 - A with
        // B + 180, would turn the part upside down). B is not unwound; it has to fit its limits.
        const double a = deg(std::asin(std::clamp(n.y, -1., 1.)));
        const double b = (std::abs(n.x) < 1e-12 && std::abs(n.z) < 1e-12) ? prev.c : deg(std::atan2(-n.x, n.z));
        if (a < m.a_min - 1e-9 || a > m.a_max + 1e-9 || b < m.c_min - 1e-9 || b > m.c_max + 1e-9)
            return false;
        out.a = a;
        out.c = b;
        return true;
    }
    const double nxy = std::hypot(n.x, n.y);
    const double a1  = deg(std::acos(std::clamp(n.z, -1., 1.)));
    // Vertical: C is free, keep it where it is so nothing turns.
    const double c1  = nxy < 1e-9 ? prev.c : deg(std::atan2(n.x, n.y));

    struct Cand { Orientation o; bool ok = false; double cost = 0.; };
    auto make = [&](double a, double c) {
        Cand k;
        k.o.a = a;
        if (a < m.a_min - 1e-9 || a > m.a_max + 1e-9)
            return k;
        if (m.c_continuous) {
            k.o.c = prev.c + wrap180(c - prev.c);
        } else {
            // The equivalent of c inside the limits nearest the previous value.
            const double base = prev.c + wrap180(c - prev.c);
            bool   found = false;
            double best  = 0.;
            for (int turns = -3; turns <= 3; ++ turns) {
                const double v = base + 360. * turns;
                if (v < m.c_min - 1e-9 || v > m.c_max + 1e-9) continue;
                if (! found || std::abs(v - prev.c) < std::abs(best - prev.c)) { best = v; found = true; }
            }
            if (! found)
                return k;
            k.o.c = best;
        }
        k.ok   = true;
        k.cost = std::abs(k.o.a - prev.a) + std::abs(k.o.c - prev.c);
        return k;
    };
    // With A = 0 (or 180) the second solution is the same pose turned by a needless half turn of C.
    const bool on_pole = nxy < 1e-9;
    Cand pos = make(a1, c1);
    Cand neg = on_pole ? make(-a1, c1) : make(-a1, c1 + 180.);
    if (a1 < 1e-9)
        neg = pos;

    const Cand *pick = nullptr;
    switch (m.solution) {
    case Solution::NegativeA: pick = neg.ok ? &neg : (pos.ok ? &pos : nullptr); break;
    case Solution::PositiveA: pick = pos.ok ? &pos : (neg.ok ? &neg : nullptr); break;
    case Solution::SmallestMove:
    default:
        if (pos.ok && neg.ok)
            pick = (neg.cost <= pos.cost + 1e-9) ? &neg : &pos;
        else
            pick = pos.ok ? &pos : (neg.ok ? &neg : nullptr);
        break;
    }
    if (pick == nullptr)
        return false;
    out = pick->o;
    return true;
}

std::vector<Orientation> solve_sequence(const Machine &m, const std::vector<Vec3> &axes)
{
    std::vector<Orientation> out;
    out.reserve(axes.size());
    Orientation cur;
    for (const Vec3 &n : axes) {
        Orientation next;
        if (norm(n - axis_of(m, cur)) > 1e-4 && solve(m, n, cur, next))
            cur = next;
        out.push_back(cur);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------

namespace {

struct Word
{
    char        letter;
    double      value;
    std::string text;   // as written, e.g. "E.01234"
};

struct Parsed
{
    std::string       raw;          // without the line ending
    size_t            line_no = 0;
    char              kind = 0;     // 'G', 'M' or 0
    int               num  = -1;
    std::vector<Word> words;
    std::string       comment;      // from the first ';', inclusive
    // Input-order modal state as of the START of this line.
    double            modal_f = 0.;
    bool              abs_xyz = true;
    bool              rel_e   = false;
    double            e_before = 0.;   // absolute E before this line (absolute mode)

    bool              is_motion() const { return kind == 'G' && num >= 0 && num <= 3; }
    const Word       *word(char l) const
    {
        for (const Word &w : words) if (w.letter == l) return &w;
        return nullptr;
    }
    bool has(char l) const { return word(l) != nullptr; }
    bool has_xyz() const { return has('X') || has('Y') || has('Z'); }
    bool extrudes() const
    {
        const Word *e = word('E');
        if (e == nullptr) return false;
        return rel_e ? e->value > 0. : e->value > e_before;
    }
};

bool parse_number(const std::string &s, size_t &i, double &v, std::string &text)
{
    const size_t start = i;
    while (i < s.size() && (std::isdigit((unsigned char) s[i]) || s[i] == '.' || s[i] == '-' || s[i] == '+'))
        ++ i;
    if (i == start)
        return false;
    text = s.substr(start, i - start);
    char *end = nullptr;
    v = std::strtod(text.c_str(), &end);
    return end != text.c_str();
}

// Only the commands this pass interprets have their words parsed; everything else is carried raw.
void parse(Parsed &p)
{
    const std::string &s   = p.raw;
    const size_t       sc  = s.find(';');
    const std::string  body = sc == std::string::npos ? s : s.substr(0, sc);
    if (sc != std::string::npos)
        p.comment = s.substr(sc);
    size_t i = 0;
    while (i < body.size() && std::isspace((unsigned char) body[i])) ++ i;
    if (i >= body.size())
        return;
    const char k = char(std::toupper((unsigned char) body[i]));
    if (k != 'G' && k != 'M')
        return;
    ++ i;
    size_t j = i;
    while (j < body.size() && std::isdigit((unsigned char) body[j])) ++ j;
    if (j == i)
        return;
    // A sub-code ("G92.1") is not one of ours.
    if (j < body.size() && body[j] == '.')
        return;
    p.kind = k;
    p.num  = std::atoi(body.substr(i, j - i).c_str());
    const bool interpret = (k == 'G' && (p.num <= 3 || p.num == 28 || p.num == 90 || p.num == 91 || p.num == 92)) ||
                           (k == 'M' && (p.num == 82 || p.num == 83));
    if (! interpret)
        return;
    i = j;
    while (i < body.size()) {
        if (std::isspace((unsigned char) body[i])) { ++ i; continue; }
        const char l = char(std::toupper((unsigned char) body[i]));
        if (! std::isalpha((unsigned char) l)) { ++ i; continue; }
        ++ i;
        Word w { l, 0., std::string() };
        std::string num;
        if (parse_number(body, i, w.value, num)) {
            w.text = std::string(1, l) + num;
            p.words.push_back(std::move(w));
        } else {
            // A bare letter ("G28 X") means "this axis" with no value.
            w.text = std::string(1, l);
            w.value = std::numeric_limits<double>::quiet_NaN();
            p.words.push_back(std::move(w));
        }
    }
}

bool parse_tool_axis(const std::string &comment, Vec3 &n)
{
    // "; TOOL_AXIS:x,y,z" (GCodeProcessor::Reserved_Tags)
    static const std::string tag = "TOOL_AXIS:";
    size_t i = 1;
    while (i < comment.size() && comment[i] == ' ') ++ i;
    if (comment.compare(i, tag.size(), tag) != 0)
        return false;
    double v[3];
    const char *c = comment.c_str() + i + tag.size();
    for (int k = 0; k < 3; ++ k) {
        char *end = nullptr;
        v[k] = std::strtod(c, &end);
        if (end == c) return false;
        c = end;
        if (k < 2) {
            if (*c != ',') return false;
            ++ c;
        }
    }
    n = { v[0], v[1], v[2] };
    return true;
}

bool is_custom_role_tag(const std::string &raw)
{
    // "; FEATURE: Custom" -- written before the machine end G-code.
    return raw.rfind("; FEATURE: Custom", 0) == 0 || raw.rfind(";TYPE:Custom", 0) == 0;
}

class Converter
{
public:
    Converter(const Machine &m, std::ostream &out) : M(m), out(out) {}

    void feed(std::string raw)
    {
        if (! raw.empty() && raw.back() == '\r')
            raw.pop_back();
        Parsed p;
        p.raw     = std::move(raw);
        p.line_no = ++ report.lines_in;
        parse(p);
        // Input-order modal state: what the slicer believed when it wrote this line.
        p.modal_f  = in_f;
        p.abs_xyz  = in_abs;
        p.rel_e    = in_rel_e;
        p.e_before = in_e;
        if (p.kind == 'G' && p.num == 90) in_abs = true;
        if (p.kind == 'G' && p.num == 91) in_abs = false;
        if (p.kind == 'M' && p.num == 82) in_rel_e = false;
        if (p.kind == 'M' && p.num == 83) in_rel_e = true;
        if (p.kind == 'G' && p.num == 92) if (const Word *e = p.word('E'); e && ! std::isnan(e->value)) in_e = e->value;
        if (p.is_motion()) {
            if (const Word *f = p.word('F'); f && ! std::isnan(f->value)) in_f = f->value;
            if (const Word *e = p.word('E'); e && ! std::isnan(e->value)) in_e = in_rel_e ? in_e + e->value : e->value;
        }
        handle(p, false);
    }

    void finish()
    {
        if (pending)
            commit(false, report.lines_in);
        std::ostringstream s;
        s << "; A/C trunnion output: pivot height " << fmt(M.pivot_height, 3) << " mm, C axis offset "
          << fmt(M.c_offset_x, 3) << "," << fmt(M.c_offset_y, 3) << " mm, " << report.transitions << " section transitions";
        if (report.transitions > 0)
            s << ", highest lift Z" << fmt(report.highest_lift, 3);
        if (report.arcs_split > 0)
            s << ", " << report.arcs_split << " tilted arcs split into lines";
        emit(s.str());
    }

    Report report;

private:
    const Machine &M;
    std::ostream  &out;

    // Input-order modal state (see Parsed).
    double in_f = 0., in_e = 0.;
    bool   in_abs = true, in_rel_e = false;

    // State of the converted stream.
    Orientation cur;                 // applied on the machine
    Vec3        pw;                  // tip in G-code (workpiece) coordinates
    Vec3        pm;                  // tip in machine coordinates, as last commanded
    bool        known[3] = { false, false, false };
    std::string last_txt[3];         // last X/Y/Z value written, formatted
    bool        fw_abs = true;       // G90/G91 as the firmware has it
    bool        feed_dirty = false;  // our own lines changed the modal feed since the last F we passed on
    double      bc_b = -90., bc_c = 0.;   // modal B/C words of the slicer's B/C convention
    bool        in_body = false;     // past the first TOOL_AXIS tag: B/C words are the slicer's section staging
    bool        in_custom = false;   // inside a custom G-code block (start / end / printing-by-object G-code)

    std::optional<Vec3> pending;     // tool axis of a section change not executed yet
    std::vector<Parsed> buffer;      // lines between the change and its first extrusion
    Material            material;
    uint32_t            section = 0;  // counts executed section changes; tags the material laid
    std::vector<Vec3>   table_ring;

    bool all_known() const { return known[0] && known[1] && known[2]; }
    bool identity() const { return cur.a == 0. && cur.c == 0.; }

    [[noreturn]] void fail(size_t line_no, const std::string &msg) const
    {
        throw std::runtime_error("line " + std::to_string(line_no) + ": " + msg);
    }

    void emit(const std::string &s) { out << s << '\n'; }

    // A feed the slicer believed modal, put back after our own moves changed it.
    std::string with_feed(const Parsed &p, std::string body) const
    {
        if (feed_dirty && p.is_motion() && ! p.has('F') && p.modal_f > 0.)
            body += " F" + fmt(p.modal_f, 3);
        return body;
    }
    void note_feed(const Parsed &p)
    {
        if (p.is_motion() && (p.has('F') || (feed_dirty && p.modal_f > 0.)))
            feed_dirty = false;
    }

    void emit_raw(const Parsed &p)
    {
        if (p.kind == 'G' && p.num == 90) fw_abs = true;
        if (p.kind == 'G' && p.num == 91) fw_abs = false;
        if (feed_dirty && p.is_motion() && ! p.has('F') && p.modal_f > 0.) {
            const size_t sc   = p.raw.find(';');
            std::string  body = sc == std::string::npos ? p.raw : p.raw.substr(0, sc);
            while (! body.empty() && std::isspace((unsigned char) body.back())) body.pop_back();
            body += " F" + fmt(p.modal_f, 3);
            if (sc != std::string::npos) body += " " + p.raw.substr(sc);
            emit(body);
        } else
            emit(p.raw);
        note_feed(p);
    }

    // ---- position bookkeeping (input semantics) ----

    void apply_xyz(const Parsed &p, Vec3 &w, bool kn[3]) const
    {
        const char   ax[3] = { 'X', 'Y', 'Z' };
        double      *v[3]  = { &w.x, &w.y, &w.z };
        for (int i = 0; i < 3; ++ i)
            if (const Word *wd = p.word(ax[i]); wd && ! std::isnan(wd->value)) {
                if (p.abs_xyz) { *v[i] = wd->value; kn[i] = true; }
                else if (kn[i]) *v[i] += wd->value;   // relative from an unknown position stays unknown
            }
    }

    // ---- material ----

    void lay(const Vec3 &a, const Vec3 &b) { material.add_segment(a, b, section); }

    // ---- emitting moves in the current orientation ----

    std::string axes_words(const Vec3 &target_m, const Parsed &p, bool force_all)
    {
        const char   ax[3] = { 'X', 'Y', 'Z' };
        const double v[3]  = { target_m.x, target_m.y, target_m.z };
        std::string  s;
        for (int i = 0; i < 3; ++ i) {
            std::string t = fmt(v[i], XYZ_DIGITS);
            if (force_all || p.has(ax[i]) || t != last_txt[i]) {
                s += ' ';
                s += ax[i];
                s += t;
            }
            last_txt[i] = t;
        }
        return s;
    }

    std::string other_words(const Parsed &p, const char *skip) const
    {
        std::string s;
        for (const Word &w : p.words)
            if (std::strchr(skip, w.letter) == nullptr) {
                s += ' ';
                s += w.text;
            }
        return s;
    }

    std::string code_of(const Parsed &p) const { return std::string(1, p.kind) + std::to_string(p.num); }

    void emit_line(const Parsed &p, std::string body)
    {
        body = with_feed(p, std::move(body));
        if (! p.comment.empty())
            body += " " + p.comment;
        emit(body);
        note_feed(p);
    }

    // An ordinary move (or an E/F-only line) in the current orientation.
    void emit_motion(const Parsed &p)
    {
        const Vec3 before    = pw;
        const bool had_known = all_known();
        bool       kn[3]     = { known[0], known[1], known[2] };
        Vec3       after     = pw;
        apply_xyz(p, after, kn);
        const bool moved     = p.has_xyz();
        const bool lays      = p.extrudes() && moved && had_known;

        if (! moved) {
            // E / F only: nothing to transform.
            emit_raw(p);
        } else if (identity()) {
            emit_raw(p);
            if (p.num == 2 || p.num == 3) {
                if (lays) lay_arc(p, before, after);
            } else if (lays)
                lay(before, after);
            pw = after;
            for (int i = 0; i < 3; ++ i) known[i] = kn[i];
            pm = pw;
            // Remember what the firmware was told, so the first transformed move only writes what changed.
            const char ax[3] = { 'X', 'Y', 'Z' };
            for (int i = 0; i < 3; ++ i)
                if (const Word *w = p.word(ax[i]); w && p.abs_xyz) last_txt[i] = fmt(w->value, XYZ_DIGITS);
                else if (! p.abs_xyz && p.has(ax[i])) last_txt[i].clear();
            if (! fw_abs) for (int i = 0; i < 3; ++ i) last_txt[i].clear();
        } else {
            if (! (kn[0] && kn[1] && kn[2]) || ! had_known)
                fail(p.line_no, "move in a tilted section from an unknown position");
            if (p.num == 2 || p.num == 3) {
                emit_arc(p, before, after, lays);
            } else {
                const Vec3 target_m = to_machine(M, cur, after);
                if (p.abs_xyz) {
                    emit_line(p, code_of(p) + axes_words(target_m, p, false) + other_words(p, "XYZABCIJ"));
                } else {
                    // Relative: the delta turns with the table, it does not move with the pivot.
                    const Vec3 d = target_m - pm;
                    std::string s = code_of(p);
                    const char   ax[3] = { 'X', 'Y', 'Z' };
                    const double v[3]  = { d.x, d.y, d.z };
                    for (int i = 0; i < 3; ++ i) {
                        const std::string t = fmt(v[i], XYZ_DIGITS);
                        if (t != "0" || p.has(ax[i])) { s += ' '; s += ax[i]; s += t; }
                        last_txt[i].clear();
                    }
                    emit_line(p, s + other_words(p, "XYZABCIJ"));
                }
                if (lays) lay(before, after);
            }
            pw = after;
            pm = to_machine(M, cur, pw);
        }
    }

    // Points of a G2/G3 arc in G-code coordinates, start excluded, chord error <= tol.
    std::vector<Vec3> arc_points(const Parsed &p, const Vec3 &a, const Vec3 &b, double tol) const
    {
        double i_off = 0., j_off = 0.;
        if (const Word *R = p.word('R'); R != nullptr && ! std::isnan(R->value)) {
            // Centre from the radius, as Grbl / Marlin / RRF do: R < 0 selects the arc over 180 degrees.
            const double x = b.x - a.x, y = b.y - a.y, d = std::hypot(x, y), r = R->value;
            const double disc = 4. * r * r - x * x - y * y;
            if (d < 1e-9 || disc < -1e-6)
                fail(p.line_no, "R arc with an impossible radius");
            double h = -std::sqrt(std::max(0., disc)) / d;
            if (p.num == 3) h = -h;
            if (r < 0.) h = -h;
            i_off = 0.5 * (x - y * h);
            j_off = 0.5 * (y + x * h);
        } else {
            const Word *I = p.word('I'), *J = p.word('J');
            if (I == nullptr && J == nullptr)
                fail(p.line_no, "arc without I/J or R");
            i_off = I ? I->value : 0.;
            j_off = J ? J->value : 0.;
        }
        const double cx = a.x + i_off, cy = a.y + j_off;
        const double r  = std::hypot(a.x - cx, a.y - cy);
        const double a0 = std::atan2(a.y - cy, a.x - cx), a1 = std::atan2(b.y - cy, b.x - cx);
        double sweep = a1 - a0;
        const bool cw = p.num == 2;
        if (cw) { if (sweep >= -1e-12) sweep -= 2. * kPi; }
        else    { if (sweep <= 1e-12)  sweep += 2. * kPi; }
        const double step = r > tol ? 2. * std::acos(1. - tol / r) : kPi / 4.;
        const int    n    = std::max(1, int(std::ceil(std::abs(sweep) / step)));
        std::vector<Vec3> pts;
        pts.reserve(n);
        for (int i = 1; i <= n; ++ i) {
            const double t = double(i) / n;
            if (i == n) { pts.push_back(b); break; }
            const double ang = a0 + sweep * t;
            pts.push_back({ cx + r * std::cos(ang), cy + r * std::sin(ang), a.z + (b.z - a.z) * t });
        }
        return pts;
    }

    void lay_arc(const Parsed &p, const Vec3 &a, const Vec3 &b)
    {
        Vec3 prev = a;
        for (const Vec3 &q : arc_points(p, a, b, 0.05)) { lay(prev, q); prev = q; }
    }

    void emit_arc(const Parsed &p, const Vec3 &a, const Vec3 &b, bool lays)
    {
        if (std::abs(cur.a) < 1e-9 && ! p.has('R')) {
            // A = 0: the arc stays in a horizontal plane, only turned by C.
            const Word *I = p.word('I'), *J = p.word('J');
            const double i0 = I ? I->value : 0., j0 = J ? J->value : 0.;
            const double sc = std::sin(rad(cur.c)), cc = std::cos(rad(cur.c));
            if (! p.abs_xyz)
                fail(p.line_no, "relative arcs in a turned section are not supported");
            const Vec3 target_m = to_machine(M, cur, b);
            std::string s = code_of(p) + axes_words(target_m, p, false) + " I" + fmt(cc * i0 - sc * j0, XYZ_DIGITS) +
                            " J" + fmt(sc * i0 + cc * j0, XYZ_DIGITS) + other_words(p, "XYZABCIJ");
            emit_line(p, s);
            if (lays) lay_arc(p, a, b);
            return;
        }
        if (! p.abs_xyz)
            fail(p.line_no, "relative arcs in a tilted section are not supported");
        // Tilted: the arc's plane is no longer horizontal, so it becomes lines.
        const std::vector<Vec3> pts = arc_points(p, a, b, 0.005);
        const Word  *E = p.word('E');
        const double e_total = E ? (p.rel_e ? E->value : E->value - p.e_before) : 0.;
        const int    n = int(pts.size());
        double       e_written = 0.;
        Vec3         prev = a;
        for (int i = 0; i < n; ++ i) {
            Parsed seg = p;
            seg.words.clear();
            std::string s = "G1" + axes_words(to_machine(M, cur, pts[i]), seg, false);
            if (E != nullptr) {
                // Cumulative rounding so the segments add up to exactly the arc's extrusion.
                const double e_cum = std::round(e_total * (i + 1) / n * 1e5) / 1e5;
                if (p.rel_e) s += " E" + fmt(e_cum - e_written, E_DIGITS);
                else         s += " E" + fmt(p.e_before + e_cum, E_DIGITS);
                e_written = e_cum;
            }
            if (i == 0) {
                if (const Word *F = p.word('F')) s += " " + F->text;
                s = with_feed(p, s);
                if (! p.comment.empty()) s += " " + p.comment;
            }
            emit(s);
            if (lays) lay(prev, pts[i]);
            prev = pts[i];
        }
        note_feed(p);
        ++ report.arcs_split;
    }

    // ---- section transitions ----

    // Highest machine Z anything on the table reaches while A/C run linearly from `from` to `to`.
    double swing_top(const Orientation &from, const Orientation &to)
    {
        if (table_ring.empty() && M.table_radius > 0.) {
            const int n = 180;
            for (int i = 0; i < n; ++ i) {
                const double t = 2. * kPi * i / n;
                table_ring.push_back({ M.origin_x + M.c_offset_x + M.table_radius * std::cos(t),
                                       M.origin_y + M.c_offset_y + M.table_radius * std::sin(t), 0. });
            }
        }
        const double span  = std::max(std::abs(to.a - from.a), std::abs(to.c - from.c));
        const int    steps = std::max(1, int(std::ceil(span / 2.)));
        double       top   = -1e300;
        double       max_sin_a = 0.;
        for (int s = 0; s <= steps; ++ s) {
            const double t = double(s) / steps;
            const Orientation o { from.a + (to.a - from.a) * t, from.c + (to.c - from.c) * t };
            const ZRow zr(o);
            max_sin_a = std::max(max_sin_a, std::abs(zr.sa));
            for (const Vec3 &p : table_ring) top = std::max(top, zr.z(M, p));
            for (const auto &kv : material.columns) {
                top = std::max(top, zr.z(M, kv.second.top));
                top = std::max(top, zr.z(M, kv.second.bottom));
            }
        }
        // A sample may sit anywhere in its 2 mm column, and the path is sampled every 2 degrees.
        const double slack = Material::COLUMN * std::sqrt(2.) * max_sin_a + 0.1;
        return top + slack;
    }

    // Whether the hotend, its tip running straight up machine Z from `tip` to `tip + rise`, meets
    // material laid so far with the table at `o`. The envelope is the pyramid of HotendEnvelope
    // clipped by a sphere of radius height about the tip, the shape the slicer's own check uses.
    // Material of section `skip` is ignored: like the slicer's check, a section is only tested against
    // the ones before it (its own contoured top may stand a hair above the tip it was printed with).
    bool envelope_hits(const Orientation &o, const Vec3 &tip, double rise, int64_t skip, Vec3 &hit) const
    {
        if (M.env_height <= 0. || material.empty())
            return false;
        const double H = M.env_height, mg = M.env_margin, R = H + mg;
        const Vec3   a = to_gcode(M, o, tip), b = to_gcode(M, o, tip + Vec3(0., 0., rise + H));
        const double lo[3] = { std::min(a.x, b.x) - R, std::min(a.y, b.y) - R, std::min(a.z, b.z) - R };
        const double hi[3] = { std::max(a.x, b.x) + R, std::max(a.y, b.y) + R, std::max(a.z, b.z) + R };
        const double B = Material::BLOCK;
        const auto  &sp = M.env_spread;
        for (int i = cell(lo[0], B); i <= cell(hi[0], B); ++ i)
            for (int j = cell(lo[1], B); j <= cell(hi[1], B); ++ j)
                for (int k = cell(lo[2], B); k <= cell(hi[2], B); ++ k) {
                    auto it = material.blocks.find(key3(i, j, k));
                    if (it == material.blocks.end()) continue;
                    for (const Material::Sample &smp : it->second) {
                        if (int64_t(smp.section) == skip) continue;
                        const Vec3  &p  = smp.p;
                        const Vec3   q  = to_machine(M, o, p) - tip;
                        const double dz = q.z;
                        if (dz <= M.env_eps) continue;
                        // Height of the point above the tip, over all tip positions of the run.
                        double h_lo = std::max(M.env_eps, dz - rise);
                        double h_hi = std::min(dz, H);
                        if (h_lo > h_hi) continue;
                        bool ok = true;
                        auto face = [&](double lat, double s) {
                            if (lat <= mg) return;
                            if (s <= 1e-12) { ok = false; return; }
                            h_lo = std::max(h_lo, (lat - mg) / s);
                        };
                        face(q.x > 0. ? q.x : 0., sp[0]);
                        face(q.x < 0. ? -q.x : 0., sp[1]);
                        face(q.y > 0. ? q.y : 0., sp[2]);
                        face(q.y < 0. ? -q.y : 0., sp[3]);
                        if (! ok) continue;
                        const double lat2 = q.x * q.x + q.y * q.y;
                        if (lat2 > R * R) continue;
                        h_hi = std::min(h_hi, std::sqrt(R * R - lat2));
                        if (h_lo <= h_hi) { hit = p; return true; }
                    }
                }
        return false;
    }

    void buffer_line(const Parsed &p) { buffer.push_back(p); }

    // Execute a pending section change. With `descend`, pw (after the buffered travels) is the first
    // extrusion point of the new section and the head is brought down onto it.
    void commit(bool descend, size_t line_no)
    {
        const Vec3 n = *pending;
        pending.reset();
        std::vector<Parsed> buf = std::move(buffer);
        buffer.clear();

        Orientation target;
        if (! solve(M, n, cur, target)) {
            std::ostringstream s;
            s << "tool axis " << fmt(n.x, 4) << "," << fmt(n.y, 4) << "," << fmt(n.z, 4);
            if (M.kind == Kind::AB) {
                const Vec3   u = n * (1. / std::max(norm(n), 1e-12));
                s << " needs A = " << fmt(deg(std::asin(std::clamp(u.y, -1., 1.))), 2) << " B = "
                  << fmt(deg(std::atan2(-u.x, u.z)), 2) << " deg, outside the tilt limits A " << fmt(M.a_min, 1)
                  << " .. " << fmt(M.a_max, 1) << ", B " << fmt(M.c_min, 1) << " .. " << fmt(M.c_max, 1);
            } else {
                s << " needs A = +/-" << fmt(deg(std::acos(std::clamp(n.z / std::max(norm(n), 1e-12), -1., 1.))), 2)
                  << " deg, outside the A limits " << fmt(M.a_min, 1) << " .. " << fmt(M.a_max, 1)
                  << (M.c_continuous ? "" : " (or no C inside the C limits)");
            }
            fail(line_no, s.str());
        }
        // Back to an upright tool ahead of custom G-code (the end of the print, a G28): that G-code is
        // written for the machine frame, which the workpiece frame only matches at C = 0 as well, so
        // turn C home rather than leave it wherever the last section had it.
        if (! descend && std::abs(target.a) < 1e-9)
            target.c = 360. * std::round(cur.c / 360.);
        if (std::abs(target.a - cur.a) < 1e-7 && std::abs(target.c - cur.c) < 1e-7) {
            // Same pose: the slicer's own travels are valid as written.
            for (const Parsed &p : buf)
                handle(p, true);
            return;
        }

        // Run the buffered lines for their effect on the position; keep what is not a move.
        std::vector<std::string> residual;
        for (const Parsed &p : buf) {
            if (p.is_motion()) {
                bool kn[3] = { known[0], known[1], known[2] };
                apply_xyz(p, pw, kn);
                for (int i = 0; i < 3; ++ i) known[i] = kn[i];
                if (const Word *e = p.word('E')) {
                    std::string s = "G1 " + e->text;
                    if (const Word *f = p.word('F')) s += " " + f->text;
                    else if (p.modal_f > 0.) s += " F" + fmt(p.modal_f, 3);
                    if (! p.comment.empty()) s += " " + p.comment;
                    residual.push_back(s);
                }
            } else if (p.kind == 'G' && p.num == 92 && p.has_xyz()) {
                fail(p.line_no, "G92 on X/Y/Z during a section change is not supported");
            } else {
                if (p.kind == 'G' && p.num == 90) fw_abs = true;
                if (p.kind == 'G' && p.num == 91) fw_abs = false;
                residual.push_back(p.raw);
            }
        }
        if (! all_known())
            fail(line_no, "section change from an unknown position (no X/Y/Z move before it)");

        // The machine Z that clears everything the swing can raise, the lift itself included.
        double z_safe = std::max(pm.z, swing_top(cur, target) + M.clearance);
        Vec3   start_m;
        if (descend) {
            start_m = to_machine(M, target, pw);
            z_safe  = std::max(z_safe, start_m.z);
        }
        if (M.max_z > 0. && z_safe > M.max_z + 1e-6) {
            std::ostringstream s;
            s << "the swing to A" << fmt(target.a, 3) << (M.kind == Kind::AB ? " B" : " C") << fmt(target.c, 3) << " needs the nozzle at Z" << fmt(z_safe, 2)
              << ", above the printable height " << fmt(M.max_z, 2) << " (part plus table swing too tall)";
            fail(line_no, s.str());
        }
        Vec3 hit;
        if (envelope_hits(cur, pm, z_safe - pm.z, int64_t(section), hit)) {
            std::ostringstream s;
            s << "lifting off the section before A" << fmt(target.a, 3) << (M.kind == Kind::AB ? " B" : " C") << fmt(target.c, 3)
              << ", the hotend would hit material printed earlier near X" << fmt(hit.x, 1) << " Y" << fmt(hit.y, 1) << " Z" << fmt(hit.z, 1)
              << " (part coordinates)";
            fail(line_no, s.str());
        }
        if (descend && envelope_hits(target, start_m, z_safe - start_m.z, -1, hit)) {
            std::ostringstream s;
            s << "descending onto the section at A" << fmt(target.a, 3) << (M.kind == Kind::AB ? " B" : " C") << fmt(target.c, 3)
              << ", the hotend would hit material printed earlier near X" << fmt(hit.x, 1) << " Y" << fmt(hit.y, 1) << " Z" << fmt(hit.z, 1)
              << " (part coordinates)";
            fail(line_no, s.str());
        }

        if (! fw_abs) emit("G90 ; A/C section change: absolute moves");
        if (z_safe > pm.z + 1e-6)
            emit("G1 Z" + fmt(z_safe, XYZ_DIGITS) + " F" + fmt(M.z_feed, 0) + " ; A/C: lift clear of the swing");
        emit("G1 A" + fmt(target.a, AC_DIGITS) + (M.kind == Kind::AB ? " B" : " C") + fmt(target.c, AC_DIGITS) + " F" + fmt(M.rotary_feed, 0) +
             " ; A/C: rotate to the next section");
        cur  = target;
        pm.z = z_safe;
        if (descend) {
            emit("G1 X" + fmt(start_m.x, XYZ_DIGITS) + " Y" + fmt(start_m.y, XYZ_DIGITS) + " F" + fmt(M.travel_feed, 0) +
                 " ; A/C: travel over the next section");
            emit("G1 Z" + fmt(start_m.z, XYZ_DIGITS) + " F" + fmt(M.z_feed, 0) + " ; A/C: descend onto it");
            pm = start_m;
        } else {
            pw = to_gcode(M, cur, pm);
        }
        last_txt[0] = fmt(pm.x, XYZ_DIGITS);
        last_txt[1] = fmt(pm.y, XYZ_DIGITS);
        last_txt[2] = fmt(pm.z, XYZ_DIGITS);
        if (! fw_abs) emit("G91");
        feed_dirty = true;
        ++ report.transitions;
        ++ section;
        report.highest_lift = std::max(report.highest_lift, z_safe);
        for (const std::string &s : residual)
            emit(s);
    }

    void handle(const Parsed &p, bool replaying)
    {
        // Comments and everything this pass does not interpret.
        if (p.kind == 0 || (! p.is_motion() && ! (p.kind == 'G' && (p.num == 28 || p.num == 90 || p.num == 91 || p.num == 92)) &&
                            ! (p.kind == 'M' && (p.num == 82 || p.num == 83)))) {
            Vec3 n;
            if (p.kind == 0 && ! p.comment.empty() && parse_tool_axis(p.comment, n)) {
                in_body = true;
                if (pending || norm(n * (1. / std::max(norm(n), 1e-12)) - axis_of(M, cur)) > 1e-4) {
                    pending = n;
                    buffer_line(p);
                    return;
                }
                emit_raw(p);
                return;
            }
            const bool role_tag = p.raw.rfind("; FEATURE: ", 0) == 0 || p.raw.rfind(";TYPE:", 0) == 0;
            if (pending && is_custom_role_tag(p.raw))
                commit(false, p.line_no);
            if (role_tag)
                in_custom = is_custom_role_tag(p.raw);
            if (pending) { buffer_line(p); return; }
            emit_raw(p);
            return;
        }

        if (p.kind == 'G' && p.num == 28) {
            if (pending) commit(false, p.line_no);
            emit_raw(p);
            // Homed axes are known again only after the next absolute move names them.
            const bool all = ! p.has('X') && ! p.has('Y') && ! p.has('Z');
            if (all || p.has('X')) known[0] = false;
            if (all || p.has('Y')) known[1] = false;
            if (all || p.has('Z')) known[2] = false;
            for (int i = 0; i < 3; ++ i) if (! known[i]) last_txt[i].clear();
            return;
        }
        if (pending && ! p.is_motion()) { buffer_line(p); return; }
        if (p.kind == 'G' && p.num == 92) {
            if (p.has_xyz()) {
                if (! identity())
                    fail(p.line_no, "G92 on X/Y/Z in a tilted or turned section is not supported");
                bool kn[3] = { known[0], known[1], known[2] };
                Parsed q = p;
                q.abs_xyz = true;
                apply_xyz(q, pw, kn);
                for (int i = 0; i < 3; ++ i) known[i] = kn[i];
                pm = pw;
                for (int i = 0; i < 3; ++ i) last_txt[i].clear();
            }
            emit_raw(p);
            return;
        }
        if (! p.is_motion()) { emit_raw(p); return; }

        // ---- G0..G3 ----
        if (p.has('A')) {
            // Machine-native custom G-code: verbatim, and its A/C are the new orientation.
            if (pending) commit(false, p.line_no);
            emit_raw(p);
            if (const Word *a = p.word('A'); a && ! std::isnan(a->value)) cur.a = p.abs_xyz ? a->value : cur.a + a->value;
            if (const Word *c = p.word(M.kind == Kind::AB ? 'B' : 'C'); c && ! std::isnan(c->value)) cur.c = p.abs_xyz ? c->value : cur.c + c->value;
            bool kn[3] = { known[0], known[1], known[2] };
            Vec3 m = pm;
            apply_xyz(p, m, kn);
            for (int i = 0; i < 3; ++ i) known[i] = kn[i];
            pm = m;
            if (all_known()) pw = to_gcode(M, cur, pm);
            for (int i = 0; i < 3; ++ i) last_txt[i].clear();
            return;
        }
        if (in_custom && ! pending && (p.has('B') || p.has('C'))) {
            // Custom G-code is the user's, written for this machine. A B word has no meaning here (a
            // B/C profile's "G1 X133 Y-97 Z140 B-120 C0" park): keep the move, drop B and C. A C word
            // alone is the machine's own C.
            if (! p.has('B')) {
                emit_raw(p);
                if (const Word *c = p.word('C'); c && ! std::isnan(c->value)) cur.c = p.abs_xyz ? c->value : cur.c + c->value;
                if (all_known()) pw = to_gcode(M, cur, pm);
                return;
            }
            Parsed q = p;
            q.words.erase(std::remove_if(q.words.begin(), q.words.end(), [](const Word &w) { return w.letter == 'B' || w.letter == 'C'; }),
                          q.words.end());
            q.comment = "; A/C output: B/C words dropped from \"" + (p.comment.empty() ? p.raw : p.raw.substr(0, p.raw.find(';'))) + "\"";
            q.raw = code_of(q) + other_words(q, "") + " " + q.comment;
            if (q.has_xyz() || q.has('E') || q.has('F'))
                emit_motion(q);
            else
                emit(q.comment);
            return;
        }
        if (p.has('B') || p.has('C')) {
            // The slicer's B/C convention: B is the tool axis elevation - 90, C its azimuth.
            if (const Word *b = p.word('B'); b && ! std::isnan(b->value)) bc_b = b->value;
            if (const Word *c = p.word('C'); c && ! std::isnan(c->value)) bc_c = c->value;
            const double cb = std::cos(rad(bc_b));
            const Vec3   n { cb * std::cos(rad(bc_c)), cb * std::sin(rad(bc_c)), -std::sin(rad(bc_b)) };
            // In the print body every B/C line is part of a section change (a C swing at a vertical tool
            // axis changes nothing by itself, but the B tilt follows), so it opens one; if the pose turns
            // out unchanged, commit() replays the slicer's own travels. Before the body (start G-code) a
            // B/C line at the current pose is just a move.
            if (replaying || (! in_body && ! pending && norm(n - axis_of(M, cur)) <= 1e-4)) {
                // No change of pose: an ordinary travel once the B/C words are gone.
                Parsed q = p;
                q.words.erase(std::remove_if(q.words.begin(), q.words.end(), [](const Word &w) { return w.letter == 'B' || w.letter == 'C'; }),
                              q.words.end());
                q.raw = code_of(q) + other_words(q, "") + (q.comment.empty() ? "" : " " + q.comment);
                if (q.has_xyz() || q.has('E') || q.has('F'))
                    emit_motion(q);
                return;
            }
            pending = n;
            buffer_line(p);
            return;
        }
        if (pending) {
            if (p.extrudes() && p.has_xyz())
                commit(true, p.line_no);
            else {
                buffer_line(p);
                return;
            }
        }
        emit_motion(p);
    }
};

} // namespace

Report process(std::istream &in, std::ostream &out, const Machine &m)
{
    Converter   conv(m, out);
    std::string line;
    while (std::getline(in, line))
        conv.feed(std::move(line));
    conv.finish();
    return conv.report;
}

} // namespace TrunnionAC
} // namespace Slic3r

#ifndef TRUNNION_AC_STANDALONE
// ------------------------------------------------------------------------------------------------
// Config adapter.

#include "../PrintConfig.hpp"
#include "../BoundingBox.hpp"
#include "../Exception.hpp"
#include "../Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r {

bool trunnion_ac_enabled(const DynamicPrintConfig &config)
{
    const auto *np  = config.option<ConfigOptionBool>("enable_nonplanar");
    const auto *sel = config.option<ConfigOptionEnum<AxisSelection>>("axis_selection");
    return np != nullptr && np->value && sel != nullptr && (sel->value == AxisSelection::AC || sel->value == AxisSelection::AB);
}

TrunnionAC::Machine trunnion_ac_machine(const DynamicPrintConfig &config)
{
    TrunnionAC::Machine m;
    auto f = [&config](const char *key, double fallback) {
        const auto *o = config.option<ConfigOptionFloat>(key);
        return o == nullptr ? fallback : o->value;
    };
    auto b = [&config](const char *key, bool fallback) {
        const auto *o = config.option<ConfigOptionBool>(key);
        return o == nullptr ? fallback : o->value;
    };
    auto first = [&config](const char *key, double fallback) {
        const auto *o = config.option<ConfigOptionFloatsNullable>(key);
        return (o == nullptr || o->values.empty() || o->is_nil(0)) ? fallback : o->get_at(0);
    };

    m.pivot_height = f("ac_pivot_height", 0.);
    m.c_offset_x   = f("ac_c_axis_offset_x", 0.);
    m.c_offset_y   = f("ac_c_axis_offset_y", 0.);
    if (b("axis_a_continuous", false)) {
        m.a_min = -180.;
        m.a_max = 180.;
    } else {
        m.a_min = f("axis_a_rotation_min", -90.);
        m.a_max = f("axis_a_rotation_max", 90.);
    }
    const auto *sel = config.option<ConfigOptionEnum<AxisSelection>>("axis_selection");
    if (sel != nullptr && sel->value == AxisSelection::AB) {
        // The rough A/B guess (0.1.4): a bed on three independent Z leadscrews ("tri-Z"), tilted by
        // driving them apart. A tilts it about X, B about Y, both about a pivot in the bed plane at
        // the leadscrews' centre: ac_pivot_height (default 0 = the bed surface) and ac_c_axis_offset
        // (the leadscrew centre relative to the bed centre). The firmware turns A/B into the three
        // leadscrew heights. Leadscrew travel allows only a few degrees, so limits still at the
        // +-360 defaults mean "not set" and become +-10; set the real ones from the machine.
        m.kind         = TrunnionAC::Kind::AB;
        m.c_continuous = false;
        auto tilt_limit = [&](const char *key, double unset) {
            const double v = f(key, unset);
            return std::abs(v) >= 360. - 1e-9 ? (v < 0. ? -10. : 10.) : v;
        };
        m.a_min = tilt_limit("axis_a_rotation_min", -360.);
        m.a_max = tilt_limit("axis_a_rotation_max", 360.);
        m.c_min = tilt_limit("axis_b_rotation_min", -360.);
        m.c_max = tilt_limit("axis_b_rotation_max", 360.);
    } else {
        m.c_continuous = b("axis_c_continuous", true);
        m.c_min        = f("axis_c_rotation_min", -360.);
        m.c_max        = f("axis_c_rotation_max", 360.);
    }
    if (const auto *sol = config.option<ConfigOptionEnum<AcSolution>>("ac_solution")) {
        switch (sol->value) {
        case AcSolution::NegativeA: m.solution = TrunnionAC::Solution::NegativeA; break;
        case AcSolution::PositiveA: m.solution = TrunnionAC::Solution::PositiveA; break;
        default:                    m.solution = TrunnionAC::Solution::SmallestMove; break;
        }
    }

    // The workpiece origin is the bed centre, which the G-code addresses at the centre of the
    // printable area (plate offsets are already removed from the written coordinates).
    double bed_reach = 0.;
    if (const auto *area = config.option<ConfigOptionPoints>("printable_area"); area != nullptr && ! area->values.empty()) {
        const BoundingBoxf bb(area->values);
        m.origin_x = bb.center().x();
        m.origin_y = bb.center().y();
        for (const Vec2d &p : area->values)
            bed_reach = std::max(bed_reach, (p - bb.center()).norm());
    }
    const double table = f("ac_table_radius", 0.);
    m.table_radius = table > 0. ? table : bed_reach;
    m.clearance    = std::max(0., f("ac_transition_clearance", 5.));
    m.max_z        = std::max(0., f("printable_height", 0.));

    const double travel = first("travel_speed", 100.);
    const double travel_z = first("travel_speed_z", 0.);
    m.travel_feed = std::max(1., travel) * 60.;
    m.z_feed      = (travel_z > 0. ? travel_z : std::min(travel, 10.)) * 60.;
    m.rotary_feed = std::max(1., f("ac_rotation_speed", 30.)) * 60.;

    const HotendEnvelope env = HotendEnvelope::from_config(config);
    m.env_height = env.enabled() ? env.height : 0.;
    for (int s = 0; s < 4; ++ s)
        m.env_spread[s] = env.spread(HotendEnvelope::Side(s));
    return m;
}

bool trunnion_ac_process_file(const std::string &path, const DynamicPrintConfig &config)
{
    if (! trunnion_ac_enabled(config))
        return false;
    const TrunnionAC::Machine machine = trunnion_ac_machine(config);
    const std::string tmp = path + ".ac";
    try {
        TrunnionAC::Report report;
        {
            boost::nowide::ifstream in(path, std::ios::binary);
            if (! in)
                throw Slic3r::RuntimeError("cannot read " + path);
            boost::nowide::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (! out)
                throw Slic3r::RuntimeError("cannot write " + tmp);
            report = TrunnionAC::process(in, out, machine);
            out.close();
            if (! out)
                throw Slic3r::RuntimeError("failed writing " + tmp);
        }
        std::string error_message;
        if (copy_file(tmp, path, error_message, false) != SUCCESS)
            throw Slic3r::RuntimeError("failed replacing " + path + ": " + error_message);
        boost::filesystem::remove(tmp);
        BOOST_LOG_TRIVIAL(info) << "A/C trunnion output: " << report.transitions << " section transitions, highest lift Z"
                                << report.highest_lift << ", " << report.arcs_split << " arcs split, " << report.lines_in << " lines";
        return true;
    } catch (const std::exception &ex) {
        boost::system::error_code ec;
        boost::filesystem::remove(tmp, ec);
        throw Slic3r::RuntimeError(std::string("A/C trunnion output failed, ") + ex.what());
    }
}

} // namespace Slic3r
#endif
