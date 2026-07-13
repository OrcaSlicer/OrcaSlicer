// VolTempCompensation.cpp
// OrcaSlicer
//
// Concept & original idea: Nadir @ CN3D (n.nadir@gmail.com). License: MIT.
// C++ port of the reference post-processor vtc_postprocess.py. See the header
// for the high-level description of the pass.

#include "VolTempCompensation.hpp"
#include "PchipInterpolatorHelper.hpp"
#include "../PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace Slic3r {

namespace {

// Local PI constant: M_PI is not guaranteed by <cmath> on MSVC without _USE_MATH_DEFINES.
constexpr double VTC_PI = 3.14159265358979323846;

// Resolved VTC parameters for one extruder/filament. Mirrors the VTCConfig +
// VTCThermalModel dataclasses from the reference Python implementation.
struct Params
{
    bool   enabled = false;

    // Flow -> temperature mapping (per filament).
    double base_temp = 220.0;     // = nozzle_temperature, reused
    double max_delta = 10.0;      // safety band around base_temp (deg C)
    double min_delta_to_act = 2.0;
    double slope_k = 0.5;         // simple mode: deg C per (mm3/s) of deviation
    double ref_flow = 12.0;       // mm3/s at which base_temp is correct
    bool   has_curve = false;     // curve mode: user flow->temp curve
    PchipInterpolatorHelper curve;
    double curve_lo_flow = 0.0, curve_hi_flow = 0.0;
    double curve_lo_temp = 0.0, curve_hi_temp = 0.0;

    // Lookahead & smoothing (seconds).
    double lookahead_s = 45.0;
    double smoothing_s = 12.0;
    double min_cmd_interval_s = 12.0;
    double micro_adjust_interval_s = 8.0;

    // Asymmetric thermal model (per hotend/printer).
    double heating_rate = 2.0;            // deg C/s, active
    double cooling_no_flow_rate = 0.30;   // deg C/s, idle nozzle
    double cooling_with_flow_rate = 0.85; // deg C/s, extruding
    double pid_overshoot = 3.0;           // deg C, heating only
    double settling_margin_s = 8.0;       // s, added to every lead time

    // Geometry.
    double filament_area = 2.405; // mm^2 (pi*(1.75/2)^2), used to derive mm3/s from E
};

// Flows below this (mm3/s) are treated as "no extrusion" for cooling-rate
// selection (travels, retracts, tiny moves).
constexpr double EXTRUSION_FLOW_THRESHOLD = 0.1;
constexpr double STEP_SIZE_DEG            = 2.0; // micro-step size

struct Segment
{
    size_t line_index = 0;
    double duration_s = 0.0;
    double flow_mm3s  = 0.0;
    bool   is_extrusion = false;
    int    extruder = 0;
    double cumulative_time_s = 0.0;
};

struct TempCommand
{
    double insert_at_time_s = 0.0;
    double target_temp = 0.0;
    std::string reason;
};

template<typename OptT, typename V>
V get_at_or(const PrintConfig &config, const char *key, unsigned int idx, V fallback)
{
    if (const auto *opt = config.option<OptT>(key); opt != nullptr && !opt->empty())
        return static_cast<V>(opt->get_at(idx));
    return fallback;
}

std::string get_string_at(const PrintConfig &config, const char *key, unsigned int idx)
{
    if (const auto *opt = config.option<ConfigOptionStrings>(key); opt != nullptr && !opt->empty())
        return opt->get_at(idx);
    return {};
}

// Parse a "flow,temp" per-line curve into a monotonic PCHIP curve. Returns
// false if fewer than two valid points are found (caller falls back to simple).
bool build_curve(const std::string &points, Params &p)
{
    std::vector<double> flows, temps;
    std::istringstream ss(points);
    std::string line;
    while (std::getline(ss, line)) {
        // tolerate whitespace and trailing commas
        double f = 0.0, t = 0.0;
        char comma = 0;
        std::istringstream ls(line);
        if (ls >> f >> comma >> t) {
            flows.push_back(f);
            temps.push_back(t);
        }
    }
    if (flows.size() < 2)
        return false;

    // PchipInterpolatorHelper sorts internally; keep our own sorted copy for the
    // clamp endpoints (interpolate() is only valid within the data range).
    std::vector<size_t> order(flows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return flows[a] < flows[b]; });

    try {
        p.curve.setData(flows, temps);
    } catch (const std::exception &) {
        return false;
    }
    p.has_curve = true;
    p.curve_lo_flow = flows[order.front()];
    p.curve_hi_flow = flows[order.back()];
    p.curve_lo_temp = temps[order.front()];
    p.curve_hi_temp = temps[order.back()];
    return true;
}

Params resolve_params(const PrintConfig &config, unsigned int e)
{
    Params p;
    p.enabled = get_at_or<ConfigOptionBools, bool>(config, "vtc_enabled", e, false);

    p.base_temp = get_at_or<ConfigOptionInts, double>(config, "nozzle_temperature", e, 220.0);
    p.max_delta = get_at_or<ConfigOptionFloats, double>(config, "vtc_max_delta", e, 10.0);
    p.min_delta_to_act = get_at_or<ConfigOptionFloats, double>(config, "vtc_min_delta", e, 2.0);
    p.slope_k   = get_at_or<ConfigOptionFloats, double>(config, "vtc_slope_k", e, 0.5);
    p.ref_flow  = get_at_or<ConfigOptionFloats, double>(config, "vtc_ref_flow", e, 12.0);

    p.lookahead_s = get_at_or<ConfigOptionFloats, double>(config, "vtc_lookahead_s", e, 45.0);
    p.smoothing_s = get_at_or<ConfigOptionFloats, double>(config, "vtc_smoothing_s", e, 12.0);
    p.min_cmd_interval_s = get_at_or<ConfigOptionFloats, double>(config, "vtc_min_cmd_interval_s", e, 12.0);
    p.micro_adjust_interval_s = get_at_or<ConfigOptionFloats, double>(config, "vtc_micro_adjust_interval_s", e, 8.0);

    p.heating_rate = get_at_or<ConfigOptionFloats, double>(config, "vtc_heating_rate", e, 2.0);
    p.cooling_no_flow_rate = get_at_or<ConfigOptionFloats, double>(config, "vtc_cooling_no_flow_rate", e, 0.30);
    p.cooling_with_flow_rate = get_at_or<ConfigOptionFloats, double>(config, "vtc_cooling_flow_rate", e, 0.85);
    p.pid_overshoot = get_at_or<ConfigOptionFloats, double>(config, "vtc_pid_overshoot", e, 3.0);
    p.settling_margin_s = get_at_or<ConfigOptionFloats, double>(config, "vtc_settling_margin_s", e, 8.0);

    const double dia = get_at_or<ConfigOptionFloats, double>(config, "filament_diameter", e, 1.75);
    p.filament_area = VTC_PI * (dia * 0.5) * (dia * 0.5);
    if (p.filament_area <= 1e-6)
        p.filament_area = 2.405;

    // Curve mode is opt-in via vtc_flow_mode == "curve"; falls back to the simple
    // slope mapping if the mode is "simple" or the curve has fewer than 2 points.
    const std::string mode = get_string_at(config, "vtc_flow_mode", e);
    if (mode == "curve") {
        const std::string curve = get_string_at(config, "vtc_flow_curve", e);
        if (!curve.empty())
            build_curve(curve, p);
    }

    return p;
}

// Extract the numeric value following `axis` in a G-code word list, e.g. the
// "12.3" in "G1 X12.3 Y4 E0.5 F1800". Returns false if the axis is absent.
bool read_axis(const std::string &line, char axis, double &out)
{
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if ((c == axis || c == (axis ^ 0x20)) &&
            (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            out = std::atof(line.c_str() + i + 1);
            return true;
        }
    }
    return false;
}

bool starts_with_token(const std::string &line, const char *tok)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t j = 0;
    while (tok[j] != '\0') {
        if (i + j >= line.size() || line[i + j] != tok[j]) return false;
        ++j;
    }
    // Token boundary must be whitespace/EOL, NOT a digit — otherwise "G1" would
    // match "G10"/"G11" (firmware retract). Numeric commands are matched exactly.
    const char after = (i + j < line.size()) ? line[i + j] : ' ';
    return after == ' ' || after == '\t' || after == '\0' || after == '\n' || after == '\r';
}

double arc_length(double x, double y, double nx, double ny, double i, double j, bool clockwise)
{
    const double cx = x + i, cy = y + j;
    const double r = std::sqrt(i * i + j * j);
    if (r < 1e-9)
        return std::sqrt((nx - x) * (nx - x) + (ny - y) * (ny - y));
    double a0 = std::atan2(y - cy, x - cx);
    double a1 = std::atan2(ny - cy, nx - cx);
    double sweep = a1 - a0;
    if (clockwise) {
        if (sweep > 0) sweep -= 2.0 * VTC_PI;
    } else {
        if (sweep < 0) sweep += 2.0 * VTC_PI;
    }
    return std::fabs(sweep) * r;
}

std::vector<Segment> parse_segments(const std::vector<std::string> &lines,
                                    const PrintConfig &config,
                                    const std::vector<Params> &params)
{
    std::vector<Segment> segments;
    segments.reserve(lines.size() / 2);

    double x = 0, y = 0, z = 0, e_prev = 0, f = 1800.0;
    int    extruder = 0;
    bool   relative_e = false;
    if (const auto *opt = config.option<ConfigOptionBool>("use_relative_e_distances"))
        relative_e = opt->value;
    double cumulative = 0.0;

    auto area_for = [&](int ext) -> double {
        return (ext >= 0 && ext < int(params.size())) ? params[ext].filament_area : 2.405;
    };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const std::string &line = lines[idx];
        if (line.empty() || line[0] == ';')
            continue;

        // E mode overrides
        if (starts_with_token(line, "M82")) { relative_e = false; continue; }
        if (starts_with_token(line, "M83")) { relative_e = true;  continue; }

        // Tool change: T0, T1, ... (only when a digit follows the T).
        if ((line[0] == 'T' || line[0] == 't') && line.size() > 1 && std::isdigit((unsigned char)line[1])) {
            extruder = std::atoi(line.c_str() + 1);
            continue;
        }

        if (starts_with_token(line, "G92")) {
            double ev;
            if (read_axis(line, 'E', ev)) e_prev = ev;
            continue;
        }

        const bool is_g0g1 = starts_with_token(line, "G1") || starts_with_token(line, "G0");
        const bool is_g2   = starts_with_token(line, "G2");
        const bool is_g3   = starts_with_token(line, "G3");
        if (!is_g0g1 && !is_g2 && !is_g3)
            continue;

        double nx = x, ny = y, nz = z, ev = e_prev, nf = f;
        const bool has_x = read_axis(line, 'X', nx);
        const bool has_y = read_axis(line, 'Y', ny);
        read_axis(line, 'Z', nz);
        const bool has_e = read_axis(line, 'E', ev);
        if (read_axis(line, 'F', nf)) f = nf;

        double de;
        if (relative_e) {
            de = has_e ? ev : 0.0;
        } else {
            de = has_e ? (ev - e_prev) : 0.0;
            if (has_e) e_prev = ev;
        }

        double dist_xy;
        if (is_g2 || is_g3) {
            double i = 0, j = 0;
            read_axis(line, 'I', i);
            read_axis(line, 'J', j);
            dist_xy = arc_length(x, y, has_x ? nx : x, has_y ? ny : y, i, j, is_g2);
        } else {
            const double dx = nx - x, dy = ny - y;
            dist_xy = std::sqrt(dx * dx + dy * dy);
        }
        const double dz = nz - z;
        const double dist = std::sqrt(dist_xy * dist_xy + dz * dz);
        const double speed_mms = f / 60.0;
        const double dur = (dist > 0 && speed_mms > 0) ? dist / speed_mms : 0.0;

        Segment seg;
        seg.line_index = idx;
        seg.duration_s = dur;
        seg.extruder = extruder;
        seg.is_extrusion = de > 1e-4 && dist_xy > 1e-3;
        if (seg.is_extrusion && dur > 0) {
            // Exact volumetric flow: filament cross-section * linear filament rate.
            double flow = area_for(extruder) * de / dur;
            seg.flow_mm3s = std::clamp(flow, 0.0, 80.0);
        }
        seg.cumulative_time_s = cumulative;
        cumulative += dur;

        x = nx; y = ny; z = nz;
        segments.push_back(seg);
    }
    return segments;
}

// Forward-looking average flow over the next window_s seconds, aligned with segments.
std::vector<double> build_smoothed_flow(const std::vector<Segment> &segments, double window_s)
{
    const size_t n = segments.size();
    std::vector<double> smoothed(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double t_start = segments[i].cumulative_time_s;
        const double t_end = t_start + window_s;
        double total_flow = 0.0, total_dur = 0.0;
        for (size_t j = i; j < n; ++j) {
            const Segment &s = segments[j];
            if (s.cumulative_time_s >= t_end)
                break;
            const double seg_end = s.cumulative_time_s + s.duration_s;
            double eff = std::min(seg_end, t_end) - std::max(s.cumulative_time_s, t_start);
            eff = std::max(0.0, eff);
            total_flow += s.flow_mm3s * eff;
            total_dur  += eff;
        }
        smoothed[i] = total_dur > 0 ? total_flow / total_dur : 0.0;
    }
    return smoothed;
}

double flow_to_temp(double flow, const Params &p)
{
    double raw;
    if (p.has_curve) {
        if (flow <= p.curve_lo_flow)       raw = p.curve_lo_temp;
        else if (flow >= p.curve_hi_flow)  raw = p.curve_hi_temp;
        else                               raw = p.curve.interpolate(flow);
    } else {
        raw = p.base_temp + p.slope_k * (flow - p.ref_flow);
    }
    const double lo = p.base_temp - p.max_delta;
    const double hi = p.base_temp + p.max_delta;
    return std::clamp(raw, lo, hi);
}

double lead_time(double current_temp, double target_temp, bool extruding, const Params &p)
{
    const double delta = target_temp - current_temp;
    double effective_delta, rate;
    if (delta > 0) {
        const double effective_target = target_temp - p.pid_overshoot;
        effective_delta = std::max(0.0, effective_target - current_temp);
        rate = p.heating_rate;
    } else {
        rate = extruding ? p.cooling_with_flow_rate : p.cooling_no_flow_rate;
        effective_delta = std::fabs(delta);
    }
    if (rate <= 0)
        return p.lookahead_s;
    return effective_delta / rate + p.settling_margin_s;
}

// Intermediate setpoints between current and target, spaced micro_adjust_interval_s
// apart (excluding the final target). Mirrors _micro_adjust_steps.
std::vector<std::pair<double, double>> micro_adjust_steps(double current_temp, double target_temp,
                                                          double start_time, const Params &p)
{
    const double delta = target_temp - current_temp;
    if (std::fabs(delta) <= STEP_SIZE_DEG + p.min_delta_to_act)
        return {};
    const int n_steps = std::max(0, int(std::fabs(delta) / STEP_SIZE_DEG) - 1);
    const double dir = delta > 0 ? 1.0 : -1.0;
    std::vector<std::pair<double, double>> steps;
    for (int k = 1; k <= n_steps; ++k) {
        double t = start_time + k * p.micro_adjust_interval_s;
        double temp = current_temp + dir * STEP_SIZE_DEG * k;
        temp = dir > 0 ? std::min(temp, target_temp) : std::max(temp, target_temp);
        steps.emplace_back(t, std::round(temp * 10.0) / 10.0);
    }
    return steps;
}

std::vector<TempCommand> schedule_commands(const std::vector<Segment> &segments,
                                           const std::vector<double> &smoothed,
                                           const std::vector<Params> &params)
{
    std::vector<TempCommand> commands;
    double last_cmd_time = -1e9;
    int    cur_extruder = segments.empty() ? 0 : segments.front().extruder;
    double current_temp = (cur_extruder < int(params.size())) ? params[cur_extruder].base_temp : 220.0;

    for (size_t i = 0; i < segments.size(); ++i) {
        const Segment &seg = segments[i];
        if (seg.extruder >= int(params.size()))
            continue;
        const Params &p = params[seg.extruder];

        // Reset assumed temperature on tool change.
        if (seg.extruder != cur_extruder) {
            cur_extruder = seg.extruder;
            current_temp = p.base_temp;
            last_cmd_time = -1e9;
        }
        if (!p.enabled)
            continue;

        const double target = flow_to_temp(smoothed[i], p);
        const double delta = target - current_temp;
        if (std::fabs(delta) < p.min_delta_to_act)
            continue;

        const bool transition_has_flow = smoothed[i] > EXTRUSION_FLOW_THRESHOLD;
        double lead = std::min(lead_time(current_temp, target, transition_has_flow, p), p.lookahead_s);
        double fire_at = seg.cumulative_time_s - lead;
        if (fire_at - last_cmd_time < p.min_cmd_interval_s)
            fire_at = last_cmd_time + p.min_cmd_interval_s;
        if (fire_at < 0)
            fire_at = 0.0;

        const auto steps = micro_adjust_steps(current_temp, target, fire_at, p);
        for (const auto &[st, stemp] : steps) {
            if (st - last_cmd_time >= p.min_cmd_interval_s) {
                std::ostringstream r;
                r << "micro-adjust toward " << target << " C (flow=" << smoothed[i] << " mm3/s)";
                commands.push_back({st, stemp, r.str()});
                last_cmd_time = st;
            }
        }

        double final_fire = steps.empty() ? fire_at
                                          : fire_at + double(steps.size()) * p.micro_adjust_interval_s;
        if (final_fire - last_cmd_time >= p.min_cmd_interval_s) {
            std::ostringstream r;
            r << "target " << target << " C for flow=" << smoothed[i] << " mm3/s";
            commands.push_back({final_fire, target, r.str()});
            last_cmd_time = final_fire;
            current_temp = target;
        }
    }

    // Deduplicate: keep at most one command per min-interval window.
    std::sort(commands.begin(), commands.end(),
              [](const TempCommand &a, const TempCommand &b) { return a.insert_at_time_s < b.insert_at_time_s; });
    std::vector<TempCommand> deduped;
    double prev = -1e9;
    const double min_iv = params.empty() ? 12.0 : params.front().min_cmd_interval_s;
    for (const auto &c : commands) {
        if (c.insert_at_time_s - prev >= min_iv) {
            deduped.push_back(c);
            prev = c.insert_at_time_s;
        }
    }
    return deduped;
}

size_t find_insertion_line(const std::vector<Segment> &segments, double fire_at)
{
    size_t lo = 0, hi = segments.empty() ? 0 : segments.size() - 1;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (segments[mid].cumulative_time_s < fire_at)
            lo = mid + 1;
        else
            hi = mid;
    }
    return segments.empty() ? 0 : segments[lo].line_index;
}

} // namespace

bool VolTempCompensation::any_enabled(const PrintConfig &config,
                                      const std::vector<unsigned int> &extruders)
{
    const auto *opt = config.option<ConfigOptionBools>("vtc_enabled");
    if (opt == nullptr || opt->empty())
        return false;
    for (unsigned int e : extruders)
        if (e < opt->size() && opt->get_at(e))
            return true;
    return false;
}

std::string VolTempCompensation::process_gcode(const std::string &gcode,
                                               const PrintConfig &config,
                                               const std::vector<unsigned int> &extruders)
{
    // Resolve per-extruder params up to the highest used index.
    unsigned int max_e = 0;
    for (unsigned int e : extruders) max_e = std::max(max_e, e);
    std::vector<Params> params(max_e + 1);
    bool any = false;
    for (unsigned int e = 0; e <= max_e; ++e) {
        params[e] = resolve_params(config, e);
        any = any || params[e].enabled;
    }
    if (!any)
        return gcode;

    // Split into lines, preserving line endings on re-emit.
    std::vector<std::string> lines;
    lines.reserve(gcode.size() / 32 + 16);
    {
        size_t start = 0;
        while (start <= gcode.size()) {
            size_t nl = gcode.find('\n', start);
            if (nl == std::string::npos) {
                if (start < gcode.size())
                    lines.push_back(gcode.substr(start));
                break;
            }
            lines.push_back(gcode.substr(start, nl - start + 1)); // keep '\n'
            start = nl + 1;
        }
    }

    const std::vector<Segment> segments = parse_segments(lines, config, params);
    if (segments.empty())
        return gcode;

    // Smoothing window: use the max across enabled extruders so the look-ahead
    // average is consistent regardless of which extruder a segment belongs to.
    double smoothing = 0.0;
    for (const auto &p : params)
        if (p.enabled) smoothing = std::max(smoothing, p.smoothing_s);
    if (smoothing <= 0) smoothing = 12.0;

    const std::vector<double> smoothed = build_smoothed_flow(segments, smoothing);
    const std::vector<TempCommand> commands = schedule_commands(segments, smoothed, params);
    if (commands.empty())
        return gcode;

    const auto *gc_opt = config.option<ConfigOptionBool>("gcode_comments");
    const bool annotate = gc_opt != nullptr && gc_opt->value;

    // Build insertion map: line_index -> M104 strings to emit before that line.
    std::vector<std::pair<size_t, std::string>> insertions;
    insertions.reserve(commands.size());
    for (const auto &cmd : commands) {
        const size_t li = find_insertion_line(segments, cmd.insert_at_time_s);
        std::ostringstream g;
        g << "M104 S" << int(std::lround(cmd.target_temp));
        if (annotate)
            g << " ; VTC: " << cmd.reason;
        g << "\n";
        insertions.emplace_back(li, g.str());
    }
    std::sort(insertions.begin(), insertions.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    std::string out;
    out.reserve(gcode.size() + commands.size() * 24);
    size_t ins = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        while (ins < insertions.size() && insertions[ins].first == i) {
            out += insertions[ins].second;
            ++ins;
        }
        out += lines[i];
    }
    return out;
}

void VolTempCompensation::process_file(const std::string &path,
                                       const PrintConfig &config,
                                       const std::vector<unsigned int> &extruders)
{
    if (!any_enabled(config, extruders))
        return;

    std::string gcode;
    {
        boost::nowide::ifstream in(path, std::ios::binary);
        if (!in) {
            BOOST_LOG_TRIVIAL(error) << "VTC: cannot open " << path << " for reading";
            return;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        gcode = ss.str();
    }

    std::string processed;
    try {
        processed = process_gcode(gcode, config, extruders);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "VTC: processing failed, leaving G-code unmodified: " << ex.what();
        return;
    }
    if (processed.size() == gcode.size() && processed == gcode)
        return; // nothing injected

    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        BOOST_LOG_TRIVIAL(error) << "VTC: cannot open " << path << " for writing";
        return;
    }
    out << processed;
    BOOST_LOG_TRIVIAL(info) << "VTC: injected predictive temperature commands into " << path;
}

} // namespace Slic3r
