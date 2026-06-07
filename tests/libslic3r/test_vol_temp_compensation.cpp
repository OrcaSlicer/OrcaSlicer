#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/VolTempCompensation.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <cmath>
#include <regex>
#include <sstream>
#include <vector>

using namespace Slic3r;

// Build a PrintConfig from defaults with VTC enabled and a known mapping.
static PrintConfig make_vtc_config(bool enabled, double base_temp = 220, double max_delta = 10,
                                   const std::string &flow_mode = "simple",
                                   const std::string &flow_curve = std::string())
{
    DynamicPrintConfig dyn = DynamicPrintConfig::full_print_config();
    dyn.set_key_value("vtc_enabled", new ConfigOptionBools{ enabled });
    dyn.set_key_value("nozzle_temperature", new ConfigOptionInts{ int(base_temp) });
    dyn.set_key_value("filament_diameter", new ConfigOptionFloats{ 1.75 });
    dyn.set_key_value("vtc_flow_mode", new ConfigOptionStrings{ flow_mode });
    dyn.set_key_value("vtc_slope_k", new ConfigOptionFloats{ 0.5 });
    dyn.set_key_value("vtc_ref_flow", new ConfigOptionFloats{ 12.0 });
    dyn.set_key_value("vtc_max_delta", new ConfigOptionFloats{ max_delta });
    dyn.set_key_value("vtc_min_delta", new ConfigOptionFloats{ 2.0 });
    dyn.set_key_value("vtc_lookahead_s", new ConfigOptionFloats{ 45.0 });
    dyn.set_key_value("vtc_smoothing_s", new ConfigOptionFloats{ 12.0 });
    dyn.set_key_value("vtc_min_cmd_interval_s", new ConfigOptionFloats{ 12.0 });
    dyn.set_key_value("vtc_micro_adjust_interval_s", new ConfigOptionFloats{ 8.0 });
    dyn.set_key_value("vtc_heating_rate", new ConfigOptionFloats{ 2.0 });
    dyn.set_key_value("vtc_cooling_no_flow_rate", new ConfigOptionFloats{ 0.30 });
    dyn.set_key_value("vtc_cooling_flow_rate", new ConfigOptionFloats{ 0.85 });
    dyn.set_key_value("vtc_pid_overshoot", new ConfigOptionFloats{ 3.0 });
    dyn.set_key_value("vtc_settling_margin_s", new ConfigOptionFloats{ 8.0 });
    dyn.set_key_value("vtc_flow_curve", new ConfigOptionStrings{ flow_curve });
    dyn.set_key_value("use_relative_e_distances", new ConfigOptionBool(false));
    dyn.set_key_value("gcode_comments", new ConfigOptionBool(true));

    PrintConfig cfg;
    cfg.apply(dyn, true /* ignore_nonexistent */);
    return cfg;
}

// Emit `n` horizontal extrusion moves (zig-zag) at a constant volumetric flow.
// flow_mm3s = filament_area * dE / dt ; dt = seg_len / speed.
static void emit_flow_block(std::ostringstream &g, int n, double flow_mm3s,
                            double &x, double &e, double seg_len = 10.0, double feed_mm_s = 60.0)
{
    const double area = 3.14159265358979323846 * (1.75 * 0.5) * (1.75 * 0.5);
    const double dt = seg_len / feed_mm_s;
    const double de = flow_mm3s * dt / area;
    double dir = 1.0;
    for (int i = 0; i < n; ++i) {
        x += dir * seg_len;
        e += de;
        g << "G1 X" << x << " Y0 E" << e << " F" << (feed_mm_s * 60.0) << "\n";
        dir = -dir;
    }
}

TEST_CASE("VTC is a no-op when disabled", "[VTC]")
{
    PrintConfig cfg = make_vtc_config(false);
    std::ostringstream g;
    g << "M104 S220\n";
    double x = 0, e = 0;
    emit_flow_block(g, 400, 20.0, x, e);
    emit_flow_block(g, 400, 3.0, x, e);
    const std::string gcode = g.str();

    const std::string out = VolTempCompensation::process_gcode(gcode, cfg, { 0 });
    REQUIRE(out == gcode);
}

TEST_CASE("VTC injects predictive M104 across a flow transition", "[VTC]")
{
    PrintConfig cfg = make_vtc_config(true);
    std::ostringstream g;
    g << "M104 S220\n";
    double x = 0, e = 0;
    emit_flow_block(g, 400, 20.0, x, e); // high flow  -> ~224 C
    emit_flow_block(g, 400, 3.0,  x, e); // low  flow  -> ~215 C
    const std::string gcode = g.str();

    const std::string out = VolTempCompensation::process_gcode(gcode, cfg, { 0 });

    // Something was injected, and only as VTC-tagged M104 lines.
    REQUIRE(out.size() > gcode.size());
    std::regex vtc_re(R"(^M104 S(\d+)\s*;\s*VTC:)");
    std::smatch m;
    int n_vtc = 0, min_t = 1000, max_t = -1000;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        if (std::regex_search(line, m, vtc_re)) {
            ++n_vtc;
            int t = std::stoi(m[1].str());
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }
        // Safety: VTC must never emit a blocking M109.
        REQUIRE(line.find("M109") == std::string::npos);
    }
    REQUIRE(n_vtc > 0);

    // Safety: every injected setpoint stays within the +/- max_delta band.
    REQUIRE(min_t >= 210);
    REQUIRE(max_t <= 230);
}

TEST_CASE("VTC respects the safety band with an aggressive slope", "[VTC]")
{
    // slope_k high enough that the raw target would blow past the band; must clamp.
    PrintConfig cfg = make_vtc_config(true, 220, 6 /* tight band */);
    std::ostringstream g;
    g << "M104 S220\n";
    double x = 0, e = 0;
    emit_flow_block(g, 400, 30.0, x, e);
    emit_flow_block(g, 400, 1.0,  x, e);

    const std::string out = VolTempCompensation::process_gcode(g.str(), cfg, { 0 });
    std::regex vtc_re(R"(^M104 S(\d+)\s*;\s*VTC:)");
    std::smatch m;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        if (std::regex_search(line, m, vtc_re)) {
            int t = std::stoi(m[1].str());
            REQUIRE(t >= 214); // 220 - 6
            REQUIRE(t <= 226); // 220 + 6
        }
    }
}

TEST_CASE("VTC curve mode interpolates the custom flow-temperature points", "[VTC]")
{
    // Curve maps high flow (20 mm3/s) -> 226 C and low flow (3 mm3/s) -> ~213 C,
    // a steeper, non-linear response than the simple slope would give. Enabling
    // "curve" mode must use these points (and clamp to the max_delta band).
    PrintConfig cfg = make_vtc_config(true, 220, 10, "curve",
                                      "2,212\n8,217\n12,220\n20,226\n25,228");

    std::ostringstream g;
    g << "M104 S220\n";
    double x = 0, e = 0;
    emit_flow_block(g, 400, 20.0, x, e); // -> curve high end (~226 C)
    emit_flow_block(g, 400, 3.0,  x, e); // -> curve low  end (~213 C)
    const std::string gcode = g.str();

    const std::string out = VolTempCompensation::process_gcode(gcode, cfg, { 0 });
    REQUIRE(out.size() > gcode.size());

    std::regex vtc_re(R"(^M104 S(\d+)\s*;\s*VTC:)");
    std::smatch m;
    int n_vtc = 0, min_t = 1000, max_t = -1000;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        if (std::regex_search(line, m, vtc_re)) {
            ++n_vtc;
            int t = std::stoi(m[1].str());
            min_t = std::min(min_t, t);
            max_t = std::max(max_t, t);
        }
        REQUIRE(line.find("M109") == std::string::npos);
    }
    REQUIRE(n_vtc > 0);
    // Curve drives the hot end higher than its low-flow target: the spread must
    // exceed the simple-slope spread and stay inside the band.
    REQUIRE(max_t >= 224);
    REQUIRE(min_t <= 216);
    REQUIRE(min_t >= 210);
    REQUIRE(max_t <= 230);
}
