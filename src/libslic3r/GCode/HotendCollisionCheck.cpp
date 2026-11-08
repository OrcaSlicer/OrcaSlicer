#include "HotendCollisionCheck.hpp"

#include "../libslic3r.h"
#include "../I18N.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Slic3r {

namespace {

struct Sample
{
    Vec3d        p;        // bed frame
    unsigned int gcode_id; // originating G-code line
    int          extruder = -1; // physical extruder of the move (tips only), -1 unknown
};

struct Episode
{
    unsigned int        first_gcode_id = 0;
    int                 object_label   = -1;
    int                 segment        = -1;
    Vec3d               axis           = Vec3d::UnitZ();
    std::vector<Sample> tips;      // every move, extruding or not, sampled every 1 mm
    std::vector<Sample> material;  // extrusions, sampled every 0.5 mm
};

void sample_segment(const Vec3d &a, const Vec3d &b, double step, unsigned int gcode_id, std::vector<Sample> &out, int extruder = -1)
{
    const Vec3d  d   = b - a;
    const double len = d.norm();
    const int    n   = std::max(2, int(len / step) + 1);
    for (int i = 0; i < n; ++ i)
        out.push_back({a + d * (double(i) / double(n - 1)), gcode_id, extruder});
}

// Summed-area table of a 0/1 grid: count() is the number of set cells in an inclusive rectangle
// of cells, in O(1), with out-of-range bounds clipped to the grid.
struct CellCounts
{
    int                  nu = 0, nv = 0;
    std::vector<int32_t> s;   // (nu + 1) x (nv + 1), s[u][v] = set cells in [0, u) x [0, v)

    void build(const std::vector<char> &occ, int nu_, int nv_)
    {
        nu = nu_; nv = nv_;
        s.assign(size_t(nu + 1) * (nv + 1), 0);
        for (int u = 0; u < nu; ++ u) {
            int32_t row = 0;
            for (int v = 0; v < nv; ++ v) {
                row += occ[size_t(u) * nv + v] ? 1 : 0;
                s[size_t(u + 1) * (nv + 1) + v + 1] = s[size_t(u) * (nv + 1) + v + 1] + row;
            }
        }
    }
    int32_t at(int u, int v) const { return s[size_t(u) * (nv + 1) + v]; }
    int32_t count(int u_lo, int u_hi, int v_lo, int v_hi) const
    {
        u_lo = std::max(u_lo, 0); v_lo = std::max(v_lo, 0);
        u_hi = std::min(u_hi, nu - 1); v_hi = std::min(v_hi, nv - 1);
        if (u_lo > u_hi || v_lo > v_hi) return 0;
        return at(u_hi + 1, v_hi + 1) - at(u_lo, v_hi + 1) - at(u_hi + 1, v_lo) + at(u_lo, v_lo);
    }
};

// How far a material cell may sit from a tip cell, per face, at one height: the pyramid's
// half-widths plus the margin, in cells (1 cell = 1 mm). Tip cell (tu, tv) reaches material cell
// (u, v) iff u - tu lies in [-xneg, xpos] and v - tv in [-yneg, ypos], i.e. the tip lies in
// tip_rect(u, v) -- cell centres against a continuous pyramid, as the old distance field did.
struct Reach
{
    double xpos, xneg, ypos, yneg;
    Reach inset(double s) const { return { xpos - s, xneg - s, ypos - s, yneg - s }; }
    bool  tip_rect(int u, int v, int &u_lo, int &u_hi, int &v_lo, int &v_hi) const
    {
        u_lo = int(std::ceil(u - xpos)); u_hi = int(std::floor(u + xneg));
        v_lo = int(std::ceil(v - ypos)); v_hi = int(std::floor(v + yneg));
        return u_lo <= u_hi && v_lo <= v_hi;
    }
};

} // namespace

std::optional<ConflictResult> check_hotend_collisions(const GCodeProcessorResult &result, const HotendCollisionParams &params)
{
    // The envelope a move of physical extruder `x` is tested with (see HotendCollisionParams).
    auto envelope_of = [&params](int x) -> const HotendEnvelope & {
        return x >= 0 && size_t(x) < params.extruder_envelopes.size() ? params.extruder_envelopes[size_t(x)] : params.envelope;
    };
    bool any_enabled = params.envelope.enabled();
    for (const HotendEnvelope &e : params.extruder_envelopes)
        any_enabled = any_enabled || e.enabled();
    if (! any_enabled)
        return std::nullopt;
    if (result.tool_axes.size() <= 1 || result.moves.size() < 2)
        return std::nullopt;   // planar print: nothing above the tip can have been printed yet

    // --- split the move stream into tool-axis episodes and sample tips / material ---
    std::vector<Episode> episodes;
    auto axis_of = [&result](const GCodeProcessorResult::MoveVertex &m) -> Vec3d {
        const size_t id = m.tool_axis_id < result.tool_axes.size() ? m.tool_axis_id : 0;
        return result.tool_axes[id].cast<double>();
    };
    for (size_t i = 1; i < result.moves.size(); ++ i) {
        const auto &prev = result.moves[i - 1];
        const auto &m    = result.moves[i];
        const Vec3d axis = axis_of(m);
        if (episodes.empty() || (axis - episodes.back().axis).norm() > 1e-6) {
            Episode e;
            e.axis = axis; e.first_gcode_id = m.gcode_id; e.object_label = m.object_label_id; e.segment = m.cut_segment_id;
            episodes.push_back(std::move(e));
        }
        Episode &e = episodes.back();
        if (m.type != EMoveType::Extrude && m.type != EMoveType::Travel)
            continue;
        const Vec3d a = prev.position.cast<double>();
        const Vec3d b = m.position.cast<double>();
        if ((b - a).norm() < 1e-9)
            continue;
        sample_segment(a, b, 1.0, m.gcode_id, e.tips, int(m.physical_extruder_id));
        if (m.type == EMoveType::Extrude && m.delta_extruder > 0.f)
            sample_segment(a, b, 0.5, m.gcode_id, e.material);
    }
    if (episodes.size() < 2)
        return std::nullopt;

    // The envelope is the four-face pyramid clipped by a sphere of radius H about the tip (the
    // preview draws the same shape): at height h no part of it stands further out than the sphere's
    // half-chord sqrt(H^2 - h^2), however shallow a face is. So each face's reach is capped there,
    // and a hit the rectangle query finds is confirmed against the true distance (below).
    std::vector<Eigen::Matrix3d> ac_frames;
    if (params.ac) {
        std::vector<TrunnionAC::Vec3> axes;
        axes.reserve(episodes.size());
        for (const Episode &e : episodes)
            axes.push_back({ e.axis.x(), e.axis.y(), e.axis.z() });
        for (const TrunnionAC::Orientation &o : TrunnionAC::solve_sequence(*params.ac, axes)) {
            const std::array<double, 9> r = TrunnionAC::rotation(*params.ac, o);
            Eigen::Matrix3d R;
            R << r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8];
            ac_frames.push_back(R);
        }
    }

    const double band = 0.2;

    struct Hit { double pen = -1.; size_t episode = 0, source = 0; Sample tip; Sample mat; double height = 0.; HotendEnvelope::Side face = HotendEnvelope::XPos; HotendEnvelope env; };
    Hit worst;
    size_t total_hits = 0;

    // material of all earlier episodes, accumulated as we go
    std::vector<Sample> earlier;
    std::vector<size_t> earlier_episode;
    for (size_t ei = 0; ei < episodes.size(); ++ ei) {
        const Episode &e = episodes[ei];
        // An episode may hold moves of more than one nozzle (a tool change inside one chunk): each
        // nozzle's tips are tested on their own, with that nozzle's envelope.
        std::vector<int> episode_extruders;
        for (const Sample &t : e.tips)
            if (std::find(episode_extruders.begin(), episode_extruders.end(), t.extruder) == episode_extruders.end())
                episode_extruders.push_back(t.extruder);
        for (const int tip_extruder : episode_extruders) {
        const HotendEnvelope &env = envelope_of(tip_extruder);
        if (! env.enabled())
            continue;
        std::vector<size_t> tip_ids; // indices into e.tips of this nozzle's tips
        for (size_t i = 0; i < e.tips.size(); ++ i)
            if (e.tips[i].extruder == tip_extruder)
                tip_ids.push_back(i);
        const double H     = env.height;
        const double sp[4] = { env.spread(HotendEnvelope::XPos), env.spread(HotendEnvelope::XNeg),
                               env.spread(HotendEnvelope::YPos), env.spread(HotendEnvelope::YNeg) };
        auto chord_at = [&](double h) { return std::sqrt(std::max(0., H * H - h * h)); };
        auto reach_at = [&](double h) -> Reach {
            const double c = chord_at(h);
            return { std::min(h * sp[0], c) + params.margin, std::min(h * sp[1], c) + params.margin,
                     std::min(h * sp[2], c) + params.margin, std::min(h * sp[3], c) + params.margin };
        };
        if (ei > 0 && ! earlier.empty() && ! tip_ids.empty()) {
            // head frame: (u, v, a) = R * p, with u / v along the head's own +X / +Y faces
            const Eigen::Matrix3d R = params.upright ? Eigen::Matrix3d::Identity() :
                                      ! ac_frames.empty() ? ac_frames[ei] : Eigen::Matrix3d(tool_head_frame(e.axis));
            std::vector<Vec3d> T(tip_ids.size()), M(earlier.size());
            for (size_t i = 0; i < T.size(); ++ i) T[i] = R * e.tips[tip_ids[i]].p;
            for (size_t i = 0; i < M.size(); ++ i) M[i] = R * earlier[i].p;
            double umin = 1e300, vmin = 1e300, umax = -1e300, vmax = -1e300;
            for (const Vec3d &p : T) { umin = std::min(umin, p.x()); vmin = std::min(vmin, p.y()); umax = std::max(umax, p.x()); vmax = std::max(vmax, p.y()); }
            for (const Vec3d &p : M) { umin = std::min(umin, p.x()); vmin = std::min(vmin, p.y()); umax = std::max(umax, p.x()); vmax = std::max(vmax, p.y()); }
            const int u0 = int(std::floor(umin)) - 1, v0 = int(std::floor(vmin)) - 1;
            const int nu = int(std::ceil(umax)) - u0 + 2, nv = int(std::ceil(vmax)) - v0 + 2;
            if (nu <= 0 || nv <= 0 || size_t(nu) * size_t(nv) > 40'000'000)
                continue; // absurd extents: do not blow memory, skip silently (logged below)

            // tips binned by height band
            std::vector<int> tip_band(T.size());
            int bmin = INT32_MAX, bmax = INT32_MIN;
            for (size_t i = 0; i < T.size(); ++ i) { tip_band[i] = int(std::floor(T[i].z() / band)); bmin = std::min(bmin, tip_band[i]); bmax = std::max(bmax, tip_band[i]); }
            std::vector<std::vector<size_t>> by_band(size_t(bmax - bmin + 1));
            for (size_t i = 0; i < T.size(); ++ i) by_band[size_t(tip_band[i] - bmin)].push_back(i);

            // material voxels (1 mm x 1 mm x band), deduped
            struct MVox { int u, v; double a; size_t sample; };
            std::vector<MVox> mv;
            {
                std::unordered_set<int64_t> seen;
                mv.reserve(M.size());
                for (size_t i = 0; i < M.size(); ++ i) {
                    const int u = int(std::floor(M[i].x())) - u0, v = int(std::floor(M[i].y())) - v0, ab = int(std::floor(M[i].z() / band));
                    const int64_t key = (int64_t(u) * 100003 + v) * 1000003 + ab;
                    if (seen.insert(key).second)
                        mv.push_back({u, v, M[i].z(), i});
                }
            }
            auto tip_cell = [&](size_t ti, int &u, int &v) { u = int(std::floor(T[ti].x())) - u0; v = int(std::floor(T[ti].y())) - v0; };

            // per band: a count table of the tip cells, then every material voxel in the pyramid's
            // height range asks whether any tip sits inside its rectangle of reach
            std::vector<char> occ;
            CellCounts        counts;
            for (size_t bi = 0; bi < by_band.size(); ++ bi) {
                if (by_band[bi].empty()) continue;
                const double a_t = (double(int(bi) + bmin) + 0.5) * band;
                bool any_candidate = false;
                for (const MVox &q : mv) { const double h = q.a - a_t; if (h > params.eps && h <= H) { any_candidate = true; break; } }
                if (! any_candidate) continue;
                occ.assign(size_t(nu) * nv, 0);
                for (size_t ti : by_band[bi]) {
                    int u, v; tip_cell(ti, u, v);
                    if (u >= 0 && v >= 0 && u < nu && v < nv) occ[size_t(u) * nv + v] = 1;
                }
                counts.build(occ, nu, nv);
                for (const MVox &q : mv) {
                    const double h = q.a - a_t;
                    if (h <= params.eps || h > H) continue;
                    const Reach r = reach_at(h);
                    int ul, uh, vl, vh;
                    if (! r.tip_rect(q.u, q.v, ul, uh, vl, vh) || counts.count(ul, uh, vl, vh) == 0)
                        continue;
                    // The rectangle is the cap's bounding square; its corners lie outside the sphere.
                    {
                        const double lim = chord_at(h) + params.margin;
                        bool         inside = false;
                        for (size_t ti : by_band[bi]) {
                            int tu, tv; tip_cell(ti, tu, tv);
                            if (tu < ul || tu > uh || tv < vl || tv > vh) continue;
                            const double du = q.u - tu, dv = q.v - tv;
                            if (du * du + dv * dv <= lim * lim) { inside = true; break; }
                        }
                        if (! inside)
                            continue;
                    }
                    ++ total_hits;
                    // Depth: the largest uniform inset of all four faces that still leaves a tip
                    // inside -- how far, laterally, the material sits from the nearest face.
                    double lo = 0., hi = std::max({ r.xpos, r.xneg, r.ypos, r.yneg });
                    for (int it = 0; it < 12; ++ it) {
                        const double mid = 0.5 * (lo + hi);
                        if (r.inset(mid).tip_rect(q.u, q.v, ul, uh, vl, vh) && counts.count(ul, uh, vl, vh) > 0) lo = mid; else hi = mid;
                    }
                    const double pen = lo;
                    if (pen > worst.pen) {
                        // For the report: the tip the material lies deepest inside, and the side of the
                        // head it is on -- the face whose reach it fills the largest fraction of, i.e.
                        // the one that would have to slim the least to let it out.
                        double best_slack = -1e300;
                        size_t best = by_band[bi].front();
                        int    best_du = 0, best_dv = 0;
                        for (size_t ti : by_band[bi]) {
                            int tu, tv; tip_cell(ti, tu, tv);
                            const int    du = q.u - tu, dv = q.v - tv;
                            const double slack[4] = { r.xpos - du, r.xneg + du, r.ypos - dv, r.yneg + dv };
                            const double least = *std::min_element(slack, slack + 4);
                            if (least > best_slack) { best_slack = least; best = ti; best_du = du; best_dv = dv; }
                        }
                        const double fill[4] = { best_du / r.xpos, -best_du / r.xneg, best_dv / r.ypos, -best_dv / r.yneg };
                        const HotendEnvelope::Side best_face = HotendEnvelope::Side(std::max_element(fill, fill + 4) - fill);
                        worst.pen = pen; worst.episode = ei; worst.source = earlier_episode[q.sample];
                        worst.tip = e.tips[tip_ids[best]]; worst.mat = earlier[q.sample]; worst.height = h; worst.face = best_face;
                        worst.env = env;
                    }
                }
            }
        }
        } // per nozzle
        for (const Sample &s : e.material) { earlier.push_back(s); earlier_episode.push_back(ei); }
    }

    if (total_hits == 0)
        return std::nullopt;

    auto chunk_name = [&episodes](size_t ei) {
        const Episode &e = episodes[ei];
        std::string name = (boost::format("chunk %1%") % (ei + 1)).str();
        if (e.segment >= 0)          name += (boost::format(" (section %1%)") % (e.segment + 1)).str();
        else if (e.object_label >= 0) name += (boost::format(" (object %1%)") % e.object_label).str();
        name += (boost::format(", tool axis %.2f,%.2f,%.2f") % e.axis.x() % e.axis.y() % e.axis.z()).str();
        return name;
    };
    auto deg = [](double a) { return (boost::format("%.0f") % a).str(); };
    const HotendEnvelope &env = worst.env; // the envelope of the nozzle that hit
    const double          H   = env.height;
    ConflictResult res(chunk_name(worst.episode), chunk_name(worst.source), worst.tip.p.z(), nullptr, nullptr);
    res.message = (boost::format(_u8L("Hotend collision: modelled as a pyramid capped by a %1% mm sphere about the tip (sides +X %2%°, -X %3%°, +Y %4%°, -Y %5%° from the bed), "
                                       "the hotend enters material printed earlier while printing %6% (G-code line %7%, tip at X%8% Y%9% Z%10%): "
                                       "material of %11% (G-code line %12%) sits %13% mm up the tool axis on the hotend's %15% side, %14% mm inside the envelope; "
                                       "%16% hits in total. Re-orient the chunk, add clearance, or correct the hotend side angles."))
                   % int(std::lround(H)) % deg(env.angle[0]) % deg(env.angle[1]) % deg(env.angle[2]) % deg(env.angle[3])
                   % res._objName1 % worst.tip.gcode_id
                   % (boost::format("%.1f") % worst.tip.p.x()).str() % (boost::format("%.1f") % worst.tip.p.y()).str() % (boost::format("%.2f") % worst.tip.p.z()).str()
                   % res._objName2 % worst.mat.gcode_id % (boost::format("%.1f") % worst.height).str()
                   % (boost::format("%.1f") % worst.pen).str() % HotendEnvelope::side_name(worst.face) % total_hits).str();
    BOOST_LOG_TRIVIAL(error) << "hotend collision check: " << res.message;
    return res;
}

} // namespace Slic3r
