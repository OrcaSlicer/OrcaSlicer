#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

// Wave overhangs (port of dennisklappe/OrcaSlicer-WaveOverhangs): a 90-degree unsupported region is
// printed as wavefronts grown out from the supported edge. These tests pin the slicing side (wave
// paths, the fill carve, the floor layers above, cut-plane segments) and the G-code side (wave
// feedrates, the wave travel speed, dwells, the temperature override and the fan).

namespace {

constexpr double PILLAR_TOP   = 10.;  // underside of the shelf, mm
constexpr double SHELF_HEIGHT = 6.;   // 30 layers at 0.2 mm
constexpr double LAYER_HEIGHT = 0.2;

// A T-shaped shelf: a 10 x 10 x 10 mm pillar under a 30 x 10 x 6 mm slab. The slab's underside at
// z = 10 cantilevers 10 mm out on both sides of the pillar: two 90-degree overhangs, each anchored
// along a single 10 mm edge, which is the case the wave generator exists for (a span anchored on one
// side only cannot be bridged).
TriangleMesh t_shelf()
{
    TriangleMesh mesh  = make_cube(10., 10., PILLAR_TOP);
    TriangleMesh shelf = make_cube(30., 10., SHELF_HEIGHT);
    shelf.translate(-10.f, 0.f, float(PILLAR_TOP));
    mesh.merge(shelf);
    return mesh;
}

DynamicPrintConfig wave_config(bool wave_on)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops",                 "2" },
        { "detect_overhang_wall",       "1" },
        { "enable_support",             "0" },
        { "sparse_infill_density",      "15%" },
        { "top_shell_layers",           "3" },
        { "bottom_shell_layers",        "2" },
        { "layer_change_gcode",         "G92 E0" },
        { "wave_overhangs",             wave_on ? "1" : "0" },
    });
    return config;
}

void collect_paths(const ExtrusionEntity *entity, std::vector<const ExtrusionPath*> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_paths(child, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
        for (const ExtrusionPath &path : loop->paths)
            out.push_back(&path);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
        for (const ExtrusionPath &path : multi->paths)
            out.push_back(&path);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity)) {
        out.push_back(path);
    }
}

std::vector<const ExtrusionPath*> perimeter_paths(const Layer &layer)
{
    std::vector<const ExtrusionPath*> out;
    for (const LayerRegion *layerm : layer.regions())
        collect_paths(&layerm->perimeters, out);
    return out;
}

std::vector<const ExtrusionPath*> fill_paths(const Layer &layer)
{
    std::vector<const ExtrusionPath*> out;
    for (const LayerRegion *layerm : layer.regions())
        collect_paths(&layerm->fills, out);
    return out;
}

size_t count_wave_paths(const Layer &layer)
{
    size_t n = 0;
    for (const ExtrusionPath *path : perimeter_paths(layer))
        if (path->wave_overhang)
            ++ n;
    return n;
}

bool any_wave_flag(const ExtrusionPath &path)
{
    return path.wave_overhang || path.wave_overhang_floor || path.wave_overhang_perimeter ||
           path.wave_overhang_floor_perimeter || path.wave_overhang_floor_distance != 0;
}

// Index of the first layer above z (the shelf's first layer when z is the pillar top).
size_t first_layer_above(const PrintObject &object, double z)
{
    for (size_t i = 0; i < object.layers().size(); ++ i)
        if (object.layers()[i]->print_z > z + EPSILON)
            return i;
    return object.layers().size();
}

Polygons fill_surfaces_of(const Layer &layer, bool solid)
{
    Polygons out;
    for (const LayerRegion *layerm : layer.regions())
        for (const Surface &surface : layerm->fill_surfaces.surfaces)
            if (surface.is_solid() == solid)
                polygons_append(out, to_polygons(surface.expolygon));
    return union_(out);
}

std::string export_gcode(Print &print, const char *tag)
{
    print.process();
    const auto out_path = boost::filesystem::temp_directory_path()
                        / boost::filesystem::unique_path(std::string("wave-overhangs-") + tag + "-%%%%%%%%.gcode");
    GCodeProcessorResult result; // export_gcode dereferences it
    print.export_gcode(out_path.string(), &result, nullptr);
    std::ifstream in(out_path.string());
    std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close(); // Windows refuses to remove a file that is still open
    boost::filesystem::remove(out_path);
    return gcode;
}

std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

// G/M/T commands with their comments stripped: the part of the G-code a machine executes.
std::vector<std::string> commands_of(const std::string &gcode)
{
    std::vector<std::string> out;
    for (std::string line : lines_of(gcode)) {
        if (size_t c = line.find(';'); c != std::string::npos)
            line.erase(c);
        while (! line.empty() && line.back() == ' ')
            line.pop_back();
        if (! line.empty() && (line[0] == 'G' || line[0] == 'M' || line[0] == 'T'))
            out.push_back(line);
    }
    return out;
}

// Value of the `axis` word in a command (e.g. 'F' in "G1 X1 Y2 F120"), if present.
bool word(const std::string &cmd, char axis, double &value)
{
    for (size_t i = 1; i < cmd.size(); ++ i)
        if (cmd[i] == axis && cmd[i - 1] == ' ') {
            value = std::atof(cmd.c_str() + i + 1);
            return true;
        }
    return false;
}

// One pass over exported G-code, following the "; WAVE_OVERHANG_START/END" debug blocks.
struct WaveGCodeScan
{
    size_t           blocks               = 0;
    bool             balanced             = true;  // START/END strictly alternate
    std::set<long>   extrusion_feeds;              // F (mm/min) on extruding moves inside a block
    std::set<long>   travel_feeds;                 // F (mm/min) on XY travels inside a block
    size_t           travels_in_blocks    = 0;
    std::set<long>   fan_in_blocks;                // part fan (M106 S) on extruding moves inside a block
    std::set<long>   fan_after_blocks;             // part fan on the first extruding move after a block
    bool             has_b_word           = false; // the file drives a B rotary axis

    explicit WaveGCodeScan(const std::string &gcode)
    {
        bool   in_block      = false;
        bool   after_block   = false;
        bool   in_wipe       = false;
        double feed          = 0.;
        long   fan           = 0;
        for (const std::string &raw : lines_of(gcode)) {
            // A wipe (retract_before_wipe) can sit inside a wave block, between the retract and the
            // travel into the wave line. Its XY moves carry no E and run at the wipe speed: not a travel.
            if (raw.rfind(";WIPE_START", 0) == 0) {
                in_wipe = true;
                continue;
            }
            if (raw.rfind(";WIPE_END", 0) == 0) {
                in_wipe = false;
                continue;
            }
            if (raw.rfind("; WAVE_OVERHANG_START", 0) == 0) {
                balanced &= ! in_block;
                in_block = true;
                ++ blocks;
                continue;
            }
            if (raw.rfind("; WAVE_OVERHANG_END", 0) == 0) {
                balanced &= in_block;
                in_block    = false;
                after_block = true;
                continue;
            }
            std::string cmd = raw.substr(0, raw.find(';'));
            while (! cmd.empty() && cmd.back() == ' ')
                cmd.pop_back();
            if (cmd.rfind("M106", 0) == 0 && cmd.find(" P") == std::string::npos) {
                double s = 0.;
                fan = word(cmd, 'S', s) ? std::lround(s) : 0;
                continue;
            }
            if (cmd.rfind("M107", 0) == 0) {
                fan = 0;
                continue;
            }
            if (cmd.rfind("G0", 0) != 0 && cmd.rfind("G1", 0) != 0)
                continue;
            double v = 0.;
            if (word(cmd, 'B', v))
                has_b_word = true;
            if (word(cmd, 'F', v))
                feed = v;
            if (in_wipe)
                continue;
            double x = 0., y = 0., e = 0.;
            const bool xy      = word(cmd, 'X', x) || word(cmd, 'Y', y);
            const bool has_e   = word(cmd, 'E', e);
            const bool extrude = has_e && e > 0.;
            // XY moves with a negative E are wipes, at the wipe speed: neither a travel nor an extrusion.
            if (! xy || (has_e && ! extrude))
                continue;
            if (extrude) {
                if (in_block) {
                    extrusion_feeds.insert(std::lround(feed));
                    fan_in_blocks.insert(fan);
                } else if (after_block) {
                    fan_after_blocks.insert(fan);
                    after_block = false;
                }
            } else if (in_block) {
                ++ travels_in_blocks;
                travel_feeds.insert(std::lround(feed));
            }
        }
        balanced &= ! in_block;
    }
};

} // namespace

TEST_CASE("Wave overhangs switched off leave every toolpath and command unchanged", "[WaveOverhangs]")
{
    // Both runs keep wave_overhangs off. The second moves every other wave knob away from its default;
    // none of them may reach the toolpaths or the G-code while the feature is off.
    DynamicPrintConfig tuned = wave_config(false);
    tuned.set_deserialize_strict({
        { "wave_overhangs_instead_of_bridges", "1" },
        { "wave_overhang_outer_perimeters",    "0" },
        { "wave_overhang_line_spacing",        "0.5" },
        { "wave_overhang_flow_mm3_per_mm",     "0.3" },
        { "wave_overhang_print_speed",         "7" },
        { "wave_overhang_perimeter_speed",     "9" },
        { "wave_overhang_travel_speed",        "11" },
        { "wave_overhang_fan_speed",           "13" },
        { "wave_overhang_aux_fan_speed",       "17" },
        { "wave_overhang_nozzle_temp",         "190" },
        { "wave_overhang_min_wave_time",       "3" },
        { "wave_overhang_min_layer_time",      "9" },
        { "wave_overhang_end_retract_length",  "1" },
        { "wave_overhang_floor_layers",        "5" },
        { "wave_overhang_floor_use_hilbert",   "1" },
        { "wave_overhang_floor_print_speed",   "5" },
        { "wave_overhang_floor_perimeter_speed", "6" },
        { "wave_overhang_floor_fan_speed",     "20" },
        { "support_remaining_areas_after_wave_overhangs", "0" },
    });

    Print baseline, variant;
    Test::init_and_process_print({ t_shelf() }, baseline, wave_config(false));
    Test::init_and_process_print({ t_shelf() }, variant, tuned);

    const PrintObject &a = *baseline.objects().front();
    const PrintObject &b = *variant.objects().front();
    REQUIRE(a.layers().size() == b.layers().size());
    REQUIRE(a.layers().size() == size_t(std::lround((PILLAR_TOP + SHELF_HEIGHT) / LAYER_HEIGHT)));
    for (size_t i = 0; i < a.layers().size(); ++ i) {
        const auto pa = perimeter_paths(*a.layers()[i]), pb = perimeter_paths(*b.layers()[i]);
        const auto fa = fill_paths(*a.layers()[i]),      fb = fill_paths(*b.layers()[i]);
        INFO("layer " << i);
        CHECK(pa.size() == pb.size());
        CHECK(fa.size() == fb.size());
        for (const auto *paths : { &pa, &pb, &fa, &fb })
            for (const ExtrusionPath *path : *paths)
                CHECK_FALSE(any_wave_flag(*path));
        CHECK(a.layers()[i]->wave_overhang_floor_polygons.empty());
        CHECK(b.layers()[i]->wave_overhang_shadow_polygons.empty());
    }

    const std::vector<std::string> ca = commands_of(export_gcode(baseline, "off-a"));
    const std::vector<std::string> cb = commands_of(export_gcode(variant, "off-b"));
    REQUIRE(! ca.empty());
    CHECK(ca == cb);
}

TEST_CASE("Wave overhangs print the cantilevers of a 90 degree shelf as waves", "[WaveOverhangs]")
{
    Print print;
    Test::init_and_process_print({ t_shelf() }, print, wave_config(true));
    const PrintObject &object = *print.objects().front();
    const size_t       wave_idx = first_layer_above(object, PILLAR_TOP);
    REQUIRE(wave_idx < object.layers().size());
    const Layer &wave_layer = *object.layers()[wave_idx];

    std::vector<const ExtrusionPath*> waves;
    for (const ExtrusionPath *path : perimeter_paths(wave_layer))
        if (path->wave_overhang)
            waves.push_back(path);
    REQUIRE(! waves.empty());
    for (const ExtrusionPath *path : waves)
        CHECK(path->role() == erOverhangPerimeter);

    // Every other layer is fully supported by the one below it, so only the shelf's first layer waves.
    for (size_t i = 0; i < object.layers().size(); ++ i)
        if (i != wave_idx) {
            INFO("layer " << i << " at z " << object.layers()[i]->print_z);
            CHECK(count_wave_paths(*object.layers()[i]) == 0);
        }

    // The wave footprint is carved out of the layer's fill surfaces: nothing else is printed there.
    const Polygons covered = union_(wave_layer.wave_overhang_covered_polygons);
    REQUIRE(! covered.empty());
    Polygons fill = fill_surfaces_of(wave_layer, true);
    polygons_append(fill, fill_surfaces_of(wave_layer, false));
    const double overlap = area(intersection(fill, covered));
    INFO("fill overlap " << overlap * SCALING_FACTOR * SCALING_FACTOR << " mm2 of "
         << area(covered) * SCALING_FACTOR * SCALING_FACTOR << " mm2 covered");
    CHECK(overlap < 0.01 * area(covered));
}

TEST_CASE("Wave floor layers make the configured number of layers above the wave solid", "[WaveOverhangs]")
{
    const int floor_layers = GENERATE(1, 3);
    DynamicPrintConfig config = wave_config(true);
    config.set_deserialize_strict({ { "wave_overhang_floor_layers", std::to_string(floor_layers) } });

    Print print;
    Test::init_and_process_print({ t_shelf() }, print, config);
    const PrintObject &object   = *print.objects().front();
    const size_t       wave_idx = first_layer_above(object, PILLAR_TOP);
    REQUIRE(wave_idx + size_t(floor_layers) + 1 < object.layers().size());
    const Layer &wave_layer = *object.layers()[wave_idx];
    REQUIRE(! wave_layer.wave_overhang_floor_polygons.empty());

    // Probe the middle of each cantilever, 2 mm clear of the walls and of the pillar's edge.
    const Polygons zone = intersection(shrink(union_(wave_layer.wave_overhang_floor_polygons), scaled<float>(2.)),
                                       shrink(to_polygons(wave_layer.lslices), scaled<float>(2.)));
    REQUIRE(! zone.empty());
    const double zone_area = area(zone);

    for (int k = 1; k <= floor_layers + 1; ++ k) {
        const Layer &layer  = *object.layers()[wave_idx + size_t(k)];
        const double solid  = area(intersection(fill_surfaces_of(layer, true), zone));
        const double sparse = area(intersection(fill_surfaces_of(layer, false), zone));
        INFO("floor_layers " << floor_layers << ", layer " << k << " above the wave: solid "
             << solid / zone_area << ", sparse " << sparse / zone_area << " of the probe");
        if (k <= floor_layers) {
            CHECK(solid > 0.95 * zone_area);
            CHECK(sparse < 0.01 * zone_area);
        } else {
            CHECK(sparse > 0.95 * zone_area);
        }
    }
}

TEST_CASE("Wave overhangs do not fire along a sloped cut-plane floor", "[WaveOverhangs][cutplanes]")
{
    // A cube cut by a tilted plane whose upper segment is printed FLAT (build normal +Z). That segment's
    // floor is then a slope in its own slicing frame, so every layer reaches past the one below it --
    // onto material the root segment has already printed. That growth is supported, and waves must not
    // treat it as an overhang.
    const double slope_deg = GENERATE(8., 17.);

    Model model;
    ModelObject *object = model.add_object();
    object->name = "sloped_cut_cube";
    object->add_volume(Test::cube(20.));
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(100., 100., 0.));
    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane cp;
    cp.point            = Vec3d(10., 10., 10.);
    cp.normal           = Vec3d(std::sin(Geometry::deg2rad(slope_deg)), 0., std::cos(Geometry::deg2rad(slope_deg)));
    cp.build_normal     = Vec3d::UnitZ();
    cp.has_build_normal = true;
    stack.planes.push_back(cp);
    object->cut_planes = stack;
    object->auto_drop = true;
    object->ensure_on_bed();

    DynamicPrintConfig config = wave_config(true);
    config.set_deserialize_strict({
        { "enable_nonplanar", "1" },
        { "axis_selection",   "bc" },  // the tilted case checks the B words
        { "print_sequence",   "by group" },
        { "wave_overhangs_instead_of_bridges", "1" }, // the most eager setting: no bridge escape hatch
    });
    Print print;
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 2);

    for (const PrintObject *segment : print.objects()) {
        REQUIRE(segment->is_segment());
        // Guard against a vacuous pass: the flat-printed upper segment really does grow layer by layer.
        if (! segment->is_root_segment()) {
            const auto &layers = segment->layers();
            REQUIRE(layers.size() > 4);
            CHECK(area(layers[layers.size() / 2]->lslices) > area(layers[1]->lslices));
        }
        size_t waves = 0;
        for (const Layer *layer : segment->layers())
            waves += count_wave_paths(*layer);
        INFO("slope " << slope_deg << " deg, segment " << segment->segment_index()
             << (segment->is_root_segment() ? " (root)" : ""));
        CHECK(waves == 0);
    }
}

TEST_CASE("Wave overhang G-code uses the wave speeds, dwells, temperature and fan", "[WaveOverhangs]")
{
    // planar: an ordinary object. tilted: the shelf rides on the upper segment of a 10-degree cut through
    // the pillar, which is printed tilted -- every move of it goes through the 5-axis transform and the
    // file carries B words.
    const bool tilted = GENERATE(false, true);

    // Wave speed 20 mm/s (not the 2 mm/s default): the shelf's two cantilevers take roughly 560 mm of wave
    // line, so at 2 mm/s the wave time alone (~280 s) would already exceed any min_layer_time and no
    // layer-time dwell would be due. At 20 mm/s it is ~28 s, under the 60 s asked for below.
    // reduce_fan_stop_start_freq: the wave layer is long, and a long layer's normal part fan is 0 unless
    // the fan is held at fan_min_speed -- set it so "the fan drops back after a wave" has a non-zero
    // level (30 %) to drop back to.
    DynamicPrintConfig config = wave_config(true);
    config.set_deserialize_strict({
        { "gcode_flavor",                     "reprapfirmware" },
        { "travel_speed",                     "200" },
        { "wave_overhang_print_speed",        "20" },
        { "wave_overhang_travel_speed",       "40" },
        { "wave_overhang_fan_speed",          "100" },
        { "wave_overhang_nozzle_temp",        "190" },
        { "wave_overhang_min_wave_time",      "5" },
        { "wave_overhang_min_layer_time",     "60" },
        { "wave_overhang_debug_gcode",        "1" },
        { "nozzle_temperature",               "215" },
        { "nozzle_temperature_initial_layer", "220" },
        { "fan_min_speed",                    "30" },
        { "fan_max_speed",                    "30" },
        { "reduce_fan_stop_start_freq",       "1" },
        { "enable_overhang_bridge_fan",       "0" },
        { "slow_down_for_layer_cooling",      "0" },
    });

    Print print;
    std::string gcode;
    if (tilted) {
        config.set_deserialize_strict({ { "enable_nonplanar", "1" }, { "axis_selection", "bc" }, { "print_sequence", "by group" } });
        Model model;
        ModelObject *object = model.add_object();
        object->name = "tilted_shelf";
        object->add_volume(t_shelf());
        ModelInstance *instance = object->add_instance();
        instance->set_offset(Vec3d(100., 100., 0.));
        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        cp.point  = Vec3d(5., 5., 5.); // through the pillar, well below the shelf
        cp.normal = Vec3d(std::sin(Geometry::deg2rad(10.)), 0., std::cos(Geometry::deg2rad(10.)));
        stack.planes.push_back(cp);
        object->cut_planes = stack;
        object->auto_drop = true;
        object->ensure_on_bed();
        print.apply(model, config);
        REQUIRE(print.objects().size() == 2);
        gcode = export_gcode(print, "tilted");
    } else {
        Model model;
        Test::init_print({ t_shelf() }, print, model, config);
        gcode = export_gcode(print, "planar");
    }
    REQUIRE(! gcode.empty());

    const WaveGCodeScan scan(gcode);
    INFO((tilted ? "tilted" : "planar") << ": " << scan.blocks << " wave paths");
    REQUIRE(scan.blocks > 0);
    CHECK(scan.balanced);
    if (tilted)
        CHECK(scan.has_b_word);

    // Wave lines print at the wave speed (20 mm/s = F1200) and nothing else.
    CHECK(scan.extrusion_feeds == std::set<long>{ 1200 });
    // The travel into each wave line uses the wave travel speed (40 mm/s = F2400), not travel_speed.
    REQUIRE(scan.travels_in_blocks > 0);
    CHECK(scan.travel_feeds == std::set<long>{ 2400 });

    // The part fan runs at the wave fan speed (100 % = S255) on wave lines and drops back to the layer's
    // own fan after them (fan_min_speed 30 % = S76, see reduce_fan_stop_start_freq above).
    CHECK(scan.fan_in_blocks == std::set<long>{ 255 });
    CHECK(scan.fan_after_blocks == std::set<long>{ 76 });

    // Dwells are integer milliseconds (RepRapFirmware rejects a fractional G4 P).
    const std::regex g4_int(R"(^G4 P\d+( |;|$))");
    size_t wave_time_dwells = 0, layer_time_dwells = 0;
    for (const std::string &line : lines_of(gcode)) {
        if (line.find("wave-overhang min_wave_time dwell") != std::string::npos) {
            ++ wave_time_dwells;
            CHECK(std::regex_search(line, g4_int));
        } else if (line.find("wave-overhang min_layer_time dwell") != std::string::npos) {
            ++ layer_time_dwells;
            CHECK(std::regex_search(line, g4_int));
        }
    }
    CHECK(wave_time_dwells > 0);
    CHECK(layer_time_dwells > 0);

    // Every temperature override is restored, to the active filament's temperature for that layer.
    size_t overrides = 0, restores = 0;
    for (const std::string &line : lines_of(gcode)) {
        if (line.find("wave-overhang temp override") != std::string::npos) {
            ++ overrides;
            CHECK(line.rfind("M104 S190", 0) == 0);
        } else if (line.find("wave-overhang temp restore") != std::string::npos) {
            ++ restores;
            CHECK(line.rfind("M104 S215", 0) == 0);
        }
    }
    CHECK(overrides == scan.blocks);
    CHECK(restores == overrides);

    // The debug block sits after the header block, never inside it: firmware parsers read the header.
    const size_t header_end = gcode.find("; HEADER_BLOCK_END");
    const size_t build_line = gcode.find("; WAVE_OVERHANG_BUILD");
    REQUIRE(header_end != std::string::npos);
    REQUIRE(build_line != std::string::npos);
    CHECK(build_line > header_end);
    CHECK(gcode.find("; WAVE_OVERHANG_CONFIG region=") != std::string::npos);
    // CoolingBuffer consumes its markers.
    CHECK(gcode.find(";_WAVE_OVERHANG_FAN") == std::string::npos);
}
