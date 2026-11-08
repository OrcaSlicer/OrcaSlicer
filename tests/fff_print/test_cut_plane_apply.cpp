#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

// Build a model with one cube object (object-local coords [0,20]^3) carrying the given cut-plane
// stack, with `n_instances` instances placed apart on the bed.
static void make_cut_cube_model(Model &model, const CutPlaneStack &stack, size_t n_instances = 1)
{
    ModelObject *object = model.add_object();
    object->name = "cut_cube";
    object->add_volume(Test::cube(20.));
    for (size_t i = 0; i < n_instances; ++ i) {
        ModelInstance *instance = object->add_instance();
        instance->set_offset(Vec3d(100. * double(i + 1), 100., 0.));
    }
    object->cut_planes = stack;
    object->ensure_on_bed();
}

static DynamicPrintConfig cut_plane_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_nonplanar",   "1" },
        { "axis_selection",     "bc" },  // these checks read the rotary (B/C) output
        { "print_sequence",     "by group" },
        // Unrelated validate() requirement of the stock test config (relative E addressing).
        { "layer_change_gcode", "G92 E0" },
    });
    return config;
}

static CutPlaneStack mid_plane_stack(double z = 10.)
{
    CutPlaneStack stack;
    stack.enabled = true;
    CutPlane cp;
    cp.point  = Vec3d(10., 10., z);
    cp.normal = Vec3d::UnitZ();
    stack.planes.push_back(cp);
    return stack;
}

SCENARIO("Print::apply emits one PrintObject per cut-plane segment", "[cutplanes]") {
    GIVEN("a cube with one mid cut plane, enable_nonplanar on") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, mid_plane_stack());
        const DynamicPrintConfig config = cut_plane_config();
        print.apply(model, config);

        THEN("two PrintObjects exist, root first") {
            REQUIRE(print.objects().size() == 2);
            const PrintObject *root  = print.objects()[0];
            const PrintObject *upper = print.objects()[1];
            CHECK(root->is_segment());
            CHECK(root->is_root_segment());
            CHECK(root->segment_index() == 0);
            CHECK(upper->is_segment());
            CHECK(! upper->is_root_segment());
            CHECK(upper->segment_index() == 1);
            const auto validation = print.validate();
            INFO("validate: " << validation.string);
            CHECK(validation.string.empty());
        }

        WHEN("the same model is re-applied") {
            std::vector<const PrintObject*> before(print.objects().begin(), print.objects().end());
            const auto status = print.apply(model, config);
            THEN("nothing is invalidated and the PrintObjects are reused") {
                CHECK(status == PrintBase::APPLY_STATUS_UNCHANGED);
                REQUIRE(print.objects().size() == before.size());
                for (size_t i = 0; i < before.size(); ++ i)
                    CHECK(print.objects()[i] == before[i]);
            }
        }

        WHEN("a cut plane is moved") {
            model.objects.front()->cut_planes.planes[0].point.z() = 14.;
            const auto status = print.apply(model, config);
            THEN("the object re-segments, still two PrintObjects") {
                CHECK(status != PrintBase::APPLY_STATUS_UNCHANGED);
                CHECK(print.objects().size() == 2);
            }
        }

        WHEN("the stack is disabled") {
            model.objects.front()->cut_planes.enabled = false;
            print.apply(model, config);
            THEN("the object collapses back to a single ordinary PrintObject") {
                REQUIRE(print.objects().size() == 1);
                CHECK(! print.objects().front()->is_segment());
            }
        }
    }

    GIVEN("the same cube but enable_nonplanar off") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, mid_plane_stack());
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        print.apply(model, config);
        THEN("cut planes are inert: one ordinary PrintObject") {
            REQUIRE(print.objects().size() == 1);
            CHECK(! print.objects().front()->is_segment());
        }
    }

    GIVEN("two instances of a cube with one mid cut plane") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, mid_plane_stack(), 2);
        print.apply(model, cut_plane_config());
        THEN("each copy gets its own segment PrintObjects: 2 copies x 2 segments, one instance each") {
            // The two instances differ only by XY shift, but a segment's G-code is un-rotated about
            // its instance placement, so copies of a cut-plane object cannot share a PrintObject
            // (they used to, and the second copy landed off its footprint).
            REQUIRE(print.objects().size() == 4);
            size_t segments = 0;
            for (const PrintObject *object : print.objects()) {
                CHECK(object->instances().size() == 1);
                segments += object->is_segment() ? 1 : 0;
            }
            CHECK(segments == 4);
        }
    }
}

SCENARIO("Cut-plane segments slice as complementary geometry", "[cutplanes][slice]") {
    GIVEN("a processed cube with one mid cut plane") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, mid_plane_stack());
        print.apply(model, cut_plane_config());
        REQUIRE(print.objects().size() == 2);
        print.process();

        const PrintObject *root  = print.objects()[0];
        const PrintObject *upper = print.objects()[1];

        THEN("the root's top layer ends at the cut and the upper starts there") {
            REQUIRE(! root->layers().empty());
            REQUIRE(! upper->layers().empty());
            const double cut_z = 10.;

            // Each segment slices in its OWN frame, with its lowest material at slice z = 0
            // (PrintObject::update_slice_frame). The root rests on the bed, so its frame coincides
            // with the world; the upper segment's does not, and layer print_z is segment-local.
            REQUIRE(root->is_segment());
            REQUIRE(upper->is_segment());
            CHECK_THAT(root->segment()->build_z_min,  Catch::Matchers::WithinAbs(0.,    1e-9));
            CHECK_THAT(upper->segment()->build_z_min, Catch::Matchers::WithinAbs(cut_z, 1e-6));

            // Root: bed to the cut, in a frame where those are the same numbers. Its MATERIAL ends
            // at the cut; the schedule may run a few empty layers past it, kept so support carrying
            // the upper segment can end flush under the cut face (segment_slice_top()).
            CHECK(root->layers().front()->print_z <= 0.3);
            double root_material_top = 0.;
            for (const Layer *layer : root->layers())
                if (! layer->lslices.empty())
                    root_material_top = std::max(root_material_top, layer->print_z);
            CHECK(root_material_top <= cut_z + 0.3);
            CHECK(root_material_top >= cut_z - 0.3);
            CHECK(root->layers().back()->print_z <= cut_z + 1.);

            // Commit 0's acceptance condition: a non-root segment's first object layer sits at its
            // configured first-layer height in its own frame -- NOT at the world cut height. The
            // pre-rebase expectation (front() >= cut_z) is what this replaces.
            CHECK_THAT(upper->layers().front()->print_z,
                       Catch::Matchers::WithinAbs(upper->slicing_parameters().first_object_layer_height, 1e-6));

            // ...and mapping the segment-local schedule back through its own build offset must land
            // on the physical cut and the physical cube top. This is the half that actually pins the
            // rebase to reality: local-frame checks alone would pass for any offset whatsoever.
            const double upper_base = upper->segment()->build_z_min;
            CHECK(upper->layers().front()->print_z + upper_base >= cut_z);
            CHECK_THAT(upper->layers().back()->print_z + upper_base, Catch::Matchers::WithinAbs(20., 0.3));
        }

        THEN("layer areas of the segments sum to the full cube's cross-section") {
            // Away from the cut both segments are 20x20 squares in disjoint z-ranges; spot-check
            // one mid layer of each.
            auto layer_area = [](const Layer *layer) {
                double a = 0.;
                for (const ExPolygon &expoly : layer->lslices)
                    a += expoly.area();
                return unscale<double>(unscale<double>(a));
            };
            const Layer *root_mid  = root->layers()[root->layers().size() / 2];
            const Layer *upper_mid = upper->layers()[upper->layers().size() / 2];
            CHECK(layer_area(root_mid)  == Catch::Approx(400.).margin(10.));
            CHECK(layer_area(upper_mid) == Catch::Approx(400.).margin(10.));
        }

        THEN("only the root gets a brim/skirt footprint") {
            CHECK(root->is_root_segment());
            CHECK(! upper->is_root_segment());
            // has_brim() is the per-object CONTRACT, not a gate brim generation runs through:
            // Print::has_brim() only decides whether make_brim() is called at all, and
            // outer_inner_brim_area() used to consult neither, which is how an untilted non-root
            // segment came to carve the root's collar. The geometric assertion that pins that
            // lives in test_skirt_brim.cpp ("A cut-plane segment off the build plate must not
            // carve the root's brim") -- this check only pins the predicate itself.
            CHECK(! upper->has_brim());
        }

        THEN("the exported G-code contains BOTH segments' layers") {
            // Export to an absolute temp path (Test::gcode uses a cwd-relative unique_path, which
            // export_gcode trips over in this fork).
            const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cutplanes-%%%%%%%%.gcode");
            GCodeProcessorResult result; // export_gcode dereferences it unconditionally
            print.export_gcode(out_path.string(), &result, nullptr);
            std::ifstream in(out_path.string());
            const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close(); // Windows refuses to remove a file that is still open
            boost::filesystem::remove(out_path);
            REQUIRE(! gcode.empty());
            // Highest printed Z must reach the cube top (~20mm): if the upper segment were
            // dropped from the export, the G-code would stop at the cut (10mm).
            double max_z = 0.;
            size_t pos = 0;
            while ((pos = gcode.find("G1 Z", pos)) != std::string::npos) {
                max_z = std::max(max_z, atof(gcode.c_str() + pos + 4));
                ++ pos;
            }
            // Also parse the layer-change comment form ";Z:" used by Orca.
            pos = 0;
            while ((pos = gcode.find(";Z:", pos)) != std::string::npos) {
                max_z = std::max(max_z, atof(gcode.c_str() + pos + 3));
                ++ pos;
            }
            INFO("max printed Z in gcode: " << max_z);
            CHECK(max_z >= 19.0);
        }
    }

    GIVEN("a processed cube with TWO stacked planes (three segments)") {
        Slic3r::Print print;
        Model model;
        CutPlaneStack stack;
        stack.enabled = true;
        for (double z : { 6., 14. }) {
            CutPlane cp;
            cp.point  = Vec3d(10., 10., z);
            cp.normal = Vec3d::UnitZ();
            stack.planes.push_back(cp);
        }
        make_cut_cube_model(model, stack);
        print.apply(model, cut_plane_config());
        REQUIRE(print.objects().size() == 3);
        print.process();

        THEN("every segment slices and the G-code reaches the cube top") {
            for (const PrintObject *object : print.objects()) {
                INFO("segment " << object->segment_index());
                CHECK(! object->layers().empty());
            }
            const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cutplanes3-%%%%%%%%.gcode");
            GCodeProcessorResult result;
            print.export_gcode(out_path.string(), &result, nullptr);
            std::ifstream in(out_path.string());
            const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close(); // Windows refuses to remove a file that is still open
            boost::filesystem::remove(out_path);
            REQUIRE(! gcode.empty());
            // Machine space is what matters here: each segment slices in its own rebased frame, so
            // the layer-change COMMENTS carry segment-local z (the tallest segment is only 8mm of
            // this 20mm cube). Only the emitted moves are un-rotated back to the machine frame, so
            // the reach-the-top assertion has to read the moves, not the comments.
            auto max_tagged = [&gcode](std::initializer_list<const char *> tags) {
                double m = 0.;
                for (const char *tag : tags) {
                    size_t pos = 0;
                    while ((pos = gcode.find(tag, pos)) != std::string::npos) {
                        m = std::max(m, atof(gcode.c_str() + pos + strlen(tag)));
                        ++ pos;
                    }
                }
                return m;
            };
            const double max_move_z    = max_tagged({ "G1 Z" });
            const double max_comment_z = max_tagged({ ";Z:", "; Z_HEIGHT: " });
            INFO("max move Z (machine frame): " << max_move_z
                 << ", max layer-comment Z (segment-local): " << max_comment_z);
            CHECK(max_move_z >= 19.0);
        }
    }

    GIVEN("a tilted segment carrying all the curved geometry, with arc fitting enabled") {
        // Landing step 1: fitted arcs must never be emitted as XY-plane G2/G3 under a non-identity
        // segment transform. GCodeWriter::extrude_arc_to_xy() takes no transform, so an unguarded
        // arc would print in the slice frame while the linear parts of the same path are un-rotated.
        //
        // Fixture design: the object is a CUBE base with a SPHERE on top, and the cut plane sits
        // below the sphere, so the root segment is purely planar (a cut cube cross-section yields
        // straight perimeters) and ONLY the tilted upper segment can produce arcs. A cube-only
        // fixture would make this test pass vacuously -- no curvature means no arcs to suppress.
        auto make_cube_plus_sphere = [](Model &model, bool planes_enabled) {
            ModelObject *object = model.add_object();
            object->name = "cube_plus_sphere";
            // modify_to_center_geometry = false: keep raw mesh coordinates so the offsets computed
            // from the bounding boxes below mean what they say (the default re-centres each volume
            // and compensates with its own offset, which would fight set_offset here).
            ModelVolume *cube   = object->add_volume(Test::cube(20.),
                                                     ModelVolumeType::MODEL_PART, false);
            ModelVolume *sphere = object->add_volume(Test::mesh(Test::TestMesh::sphere_50mm),
                                                     ModelVolumeType::MODEL_PART, false);
            const BoundingBoxf3 cb = cube->mesh().bounding_box();
            const BoundingBoxf3 sb = sphere->mesh().bounding_box();
            // Rest the sphere on top of the cube, centred over it.
            sphere->set_offset(Vec3d(cb.center().x() - sb.center().x(),
                                     cb.center().y() - sb.center().y(),
                                     cb.max.z()      - sb.min.z()));
            ModelInstance *instance = object->add_instance();
            instance->set_offset(Vec3d(100., 100., 0.));

            CutPlaneStack stack;
            stack.enabled = planes_enabled;
            CutPlane cp;
            // Tilted, and low enough that the tilt never reaches the sphere. The plane pivots about
            // the cube centre, so over the cube's half-span (10mm) it deviates by 0.3*10 = 3mm; sited
            // 5mm below the cube top it peaks 2mm short of the sphere base. All curvature therefore
            // lands in the upper (tilted) segment, and the root stays purely planar.
            cp.point  = Vec3d(cb.center().x(), cb.center().y(), cb.max.z() - 5.);
            cp.normal = Vec3d(0.3, 0., 1.).normalized();
            stack.planes.push_back(cp);
            object->cut_planes = stack;
            object->ensure_on_bed();
        };
        auto export_gcode_string = [](Slic3r::Print &print, const char *tag) {
            print.process();
            const auto out_path = boost::filesystem::temp_directory_path()
                                / boost::filesystem::unique_path(std::string("cutplanes-") + tag + "-%%%%%%%%.gcode");
            GCodeProcessorResult result;
            print.export_gcode(out_path.string(), &result, nullptr);
            std::ifstream in(out_path.string());
            const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close(); // Windows refuses to remove a file that is still open
            boost::filesystem::remove(out_path);
            return gcode;
        };
        // NOTE the trailing space in the needles. Arc moves are emitted as "G2 X.. Y.. I.. J..",
        // but "\nG2" alone also matches G28 (auto-home) in the machine start G-code -- which made
        // this count report phantom arcs in the segmented run AND let the "control emits arcs"
        // guard below pass without a single real arc.
        auto count_arcs = [](const std::string &gcode) {
            size_t n = 0;
            for (const char *needle : { "\nG2 ", "\nG3 " }) {
                size_t pos = 0;
                while ((pos = gcode.find(needle, pos)) != std::string::npos) { ++ n; ++ pos; }
            }
            return n;
        };
        // Arcs emitted while a NON-ROOT segment's toolpaths are open (";SEGMENT_ID:<n>" with n >= 1,
        // closed by ";SEGMENT_ID:-1"). The root segment's frame is a pure Z translation, so its arcs
        // stay valid XY-plane arcs and are deliberately kept (GCode.cpp keys the fallback on the
        // ROTATION part of the trafo); only a rotated segment must never carry a fitted arc.
        auto count_arcs_in_rotated_segments = [](const std::string &gcode) {
            size_t n   = 0;
            int    cur = -1;
            for (size_t pos = 0; pos < gcode.size();) {
                size_t end = gcode.find('\n', pos);
                if (end == std::string::npos) end = gcode.size();
                const std::string line = gcode.substr(pos, end - pos);
                if (line.rfind(";SEGMENT_ID:", 0) == 0)
                    cur = std::atoi(line.c_str() + 12);
                else if (cur >= 1 && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                    ++ n;
                pos = end + 1;
            }
            return n;
        };
        // Attribute each arc to the extrusion type in force when it was emitted. Skirt and brim are
        // authored in BED coordinates and are extruded with an identity transform by design, so an
        // arc there is legitimate; an arc under a segment's own object paths is the defect.
        auto arc_types = [](const std::string &gcode) {
            std::vector<std::string> types;
            for (size_t pos = 0; (pos = gcode.find_first_of('\n', pos)) != std::string::npos; ) {
                ++ pos;
                if (gcode.compare(pos, 3, "G2 ") != 0 && gcode.compare(pos, 3, "G3 ") != 0)
                    continue;
                const size_t t = gcode.rfind(";TYPE:", pos);
                types.push_back(t == std::string::npos ? std::string("<none>")
                                                       : gcode.substr(t + 6, gcode.find('\n', t) - t - 6));
            }
            return types;
        };

        DynamicPrintConfig config = cut_plane_config();
        config.set_deserialize_strict({ { "enable_arc_fitting", "1" } });

        THEN("the control run without cut planes does emit arcs") {
            // Guards the negative assertion below: if this fixture produced no arcs even when
            // unsegmented, the "no arcs" check would prove nothing. A planar print: with
            // enable_nonplanar on, normalize_fdm turns arc fitting off for the whole print.
            DynamicPrintConfig planar = config;
            planar.set_deserialize_strict({ { "enable_nonplanar", "0" } });
            Slic3r::Print print;
            Model model;
            make_cube_plus_sphere(model, /*planes_enabled*/ false);
            print.apply(model, planar);
            REQUIRE(print.objects().size() == 1);
            REQUIRE(! print.objects().front()->is_segment());
            const std::string gcode = export_gcode_string(print, "arc-control");
            REQUIRE(! gcode.empty());
            INFO("arc moves in unsegmented control: " << count_arcs(gcode));
            CHECK(count_arcs(gcode) > 0);
        }

        THEN("the segmented run emits no arcs inside the rotated segment") {
            Slic3r::Print print;
            Model model;
            make_cube_plus_sphere(model, /*planes_enabled*/ true);
            print.apply(model, config);
            REQUIRE(print.objects().size() == 2);
            // The curved volume must really be in the non-root segment for this to mean anything.
            REQUIRE(print.objects()[1]->is_segment());
            REQUIRE(! print.objects()[1]->is_root_segment());
            const std::string gcode = export_gcode_string(print, "arc-segmented");
            REQUIRE(! gcode.empty());
            std::string where;
            for (const std::string &t : arc_types(gcode))
                where += "[" + t + "]";
            INFO("arc moves in segmented export: " << count_arcs(gcode) << " (root-segment arcs are allowed) types: " << where);
            CHECK(count_arcs_in_rotated_segments(gcode) == 0);
        }
    }

    GIVEN("a tilted segment with supports enabled") {
        // Support toolpaths must be emitted in the same frame as the object they hold up.
        // GCode::extrude_support() took no transform, so a rotated segment's support printed in the
        // SLICE frame while the segment's own perimeters were un-rotated -- support and part in two
        // different frames within one object.
        //
        // Support generation is not segment-aware yet (it still assumes the bed at the segment
        // frame's z=0, hence the warning in PrintObjectSlice.cpp); this asserts only WHERE the
        // resulting toolpaths are emitted, which is a separate concern from whether the cascade has
        // decided the right places to put them.
        Slic3r::Print print;
        Model model;
        ModelObject *object = model.add_object();
        object->name = "supported_cut";
        ModelVolume *cube   = object->add_volume(Test::cube(20.),
                                                 ModelVolumeType::MODEL_PART, false);
        ModelVolume *sphere = object->add_volume(Test::mesh(Test::TestMesh::sphere_50mm),
                                                 ModelVolumeType::MODEL_PART, false);
        const BoundingBoxf3 cb = cube->mesh().bounding_box();
        const BoundingBoxf3 sb = sphere->mesh().bounding_box();
        // Sphere resting on the cube: its underside is a large overhang, so support is guaranteed.
        sphere->set_offset(Vec3d(cb.center().x() - sb.center().x(),
                                 cb.center().y() - sb.center().y(),
                                 cb.max.z()      - sb.min.z()));
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));

        const double cut_z = cb.max.z() - 5.;
        CutPlaneStack stack;
        stack.enabled = true;
        CutPlane cp;
        cp.point  = Vec3d(cb.center().x(), cb.center().y(), cut_z);
        cp.normal = Vec3d(0.3, 0., 1.).normalized();
        stack.planes.push_back(cp);
        object->cut_planes = stack;
        object->ensure_on_bed();

        DynamicPrintConfig config = cut_plane_config();
        config.set_deserialize_strict({ { "enable_support", "1" }, { "support_type", "normal(auto)" } });
        print.apply(model, config);
        REQUIRE(print.objects().size() == 2);
        print.process();

        THEN("support toolpaths are emitted in machine space, not the segment's slice frame") {
            const auto out_path = boost::filesystem::temp_directory_path()
                                / boost::filesystem::unique_path("cutplanes-support-%%%%%%%%.gcode");
            GCodeProcessorResult result;
            print.export_gcode(out_path.string(), &result, nullptr);
            std::ifstream in(out_path.string());
            const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close(); // Windows refuses to remove a file that is still open
            boost::filesystem::remove(out_path);
            REQUIRE(! gcode.empty());

            // Collect the machine Z of every support extrusion move. Track the Z in force across ALL
            // G0/G1 lines: as upstream, a Z word is written only when Z changes, so an XY-only
            // support move inherits the last Z (a planar root segment's support carries no Z words at
            // all). Sampling only lines that spell out a Z would miss exactly those moves.
            //
            // NOTE what is deliberately NOT asserted here: that a support move's Z matches the
            // object move next to it. Under a tilted segment the un-rotation makes machine Z vary
            // across a single slice layer with XY, so per-move correspondence is not an invariant
            // -- it disagrees by tens of millimetres for entirely correct output. Phase B's frame
            // fix is pinned by its own neutralisation experiment, not by this fixture.
            std::vector<double> support_z;
            bool                in_support = false;
            bool                have_z     = false;
            double              current_z  = 0.;
            for (size_t pos = 0; pos < gcode.size(); ) {
                const size_t eol  = gcode.find('\n', pos);
                const std::string line = gcode.substr(pos, (eol == std::string::npos ? gcode.size() : eol) - pos);
                pos = (eol == std::string::npos) ? gcode.size() : eol + 1;

                if (line.rfind(";TYPE:", 0) == 0) {
                    in_support = line.find("Support") != std::string::npos;
                    continue;
                }
                const bool motion = line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0;
                if (! motion)
                    continue;
                if (const size_t z_at = line.find(" Z"); z_at != std::string::npos) {
                    current_z = atof(line.c_str() + z_at + 2);
                    have_z    = true;
                }
                if (in_support && have_z && line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos)
                    support_z.push_back(current_z);
            }
            REQUIRE(support_z.size() > 20);

            // Two separate behaviours, measured separately because they have separate causes.
            //
            // (1) Support descends past the cut onto the segment's own TILTED floor -- that is
            //     SegmentSupportBounds plus the below-floor extension, and it holds with or
            //     without the cascade (measured: it reaches ~8mm on this fixture either way).
            // (2) Support reaches the BUILD PLATE. That is the cascade and only the cascade: the
            //     sphere overhangs the cube on every side, so the child cannot land all of its
            //     load on its own floor, and what it cannot carry is handed to the root. The
            //     root's own geometry is a plain cube with no overhang of its own. Verified by
            //     disabling the cascade and re-running: the lowest support move rises from 0.4mm
            //     to 8.1mm, and the support move count halves.
            const double lowest = *std::min_element(support_z.begin(), support_z.end());
            const double cut_lowest = cut_z - 3.;   // the plane tilts ~3mm across the cube
            size_t below_cut = 0;
            for (double z : support_z)
                if (z < cut_lowest)
                    ++ below_cut;
            INFO("support moves: " << support_z.size() << ", lowest Z: " << lowest
                 << ", moves below the cut (" << cut_lowest << "): " << below_cut);
            CHECK(below_cut > 20);      // (1)
            CHECK(lowest < 1.0);        // (2)
        }
    }

    GIVEN("a cut plane that leaves a segment thinner than the first printable layer") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, mid_plane_stack(0.01));
        print.apply(model, cut_plane_config());

        THEN("the unprintable partition is treated as inert and export remains safe") {
            REQUIRE(print.objects().size() == 1);
            CHECK(! print.objects().front()->is_segment());
            print.process();
            const auto out_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cutplanes-thin-%%%%%%%%.gcode");
            GCodeProcessorResult result;
            CHECK_NOTHROW(print.export_gcode(out_path.string(), &result, nullptr));
            boost::filesystem::remove(out_path);
        }
    }
}


// Declarative cut planes: the ";SEGMENT_ID:" annotation exists purely so the G-code preview can
// colour a toolpath by the section it belongs to. It is LATCHED by the processor, so an opening tag
// with no closing one leaves every later move in the file wearing the last section's id -- an
// ordinary object printed afterwards, the wipe tower, custom G-code, the next layer's skirt.
SCENARIO("Section tags scope the toolpaths they belong to", "[cutplanes]") {
    GIVEN("a segmented print") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, mid_plane_stack());
        print.apply(model, cut_plane_config());
        print.process();

        const auto out_path = boost::filesystem::temp_directory_path() /
                              boost::filesystem::unique_path("cutplanes-tags-%%%%%%%%.gcode");
        GCodeProcessorResult result;
        print.export_gcode(out_path.string(), &result, nullptr);
        std::ifstream in(out_path.string());
        const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        in.close(); // Windows refuses to remove a file that is still open
        boost::filesystem::remove(out_path);
        REQUIRE(! gcode.empty());

        auto count_of = [&gcode](const std::string &needle) {
            size_t n = 0, pos = 0;
            while ((pos = gcode.find(needle, pos)) != std::string::npos) { ++ n; pos += needle.size(); }
            return n;
        };

        THEN("every opening tag is closed") {
            const size_t opens  = count_of(";SEGMENT_ID:") - count_of(";SEGMENT_ID:-1");
            const size_t closes = count_of(";SEGMENT_ID:-1");
            REQUIRE(opens > 0);
            CHECK(opens == closes);
        }

        THEN("the parsed moves carry the section they belong to, and nothing else does") {
            REQUIRE(! result.moves.empty());
            bool saw_section_0 = false, saw_section_1 = false;
            for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
                CHECK(move.cut_segment_id >= -1);
                CHECK(move.cut_segment_id <= 1);          // only two segments exist
                saw_section_0 = saw_section_0 || move.cut_segment_id == 0;
                saw_section_1 = saw_section_1 || move.cut_segment_id == 1;
            }
            CHECK(saw_section_0);
            CHECK(saw_section_1);
            // The scope is closed at the end of the print rather than latched on the last section.
            CHECK(result.moves.back().cut_segment_id == -1);
        }
    }

    GIVEN("an ordinary print with no cut planes") {
        Slic3r::Print print;
        Model model;
        make_cut_cube_model(model, CutPlaneStack{});   // stack absent
        print.apply(model, cut_plane_config());
        print.process();

        const auto out_path = boost::filesystem::temp_directory_path() /
                              boost::filesystem::unique_path("cutplanes-notags-%%%%%%%%.gcode");
        GCodeProcessorResult result;
        print.export_gcode(out_path.string(), &result, nullptr);
        std::ifstream in(out_path.string());
        const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        in.close(); // Windows refuses to remove a file that is still open
        boost::filesystem::remove(out_path);
        REQUIRE(! gcode.empty());

        THEN("neither tag is emitted and no move claims a section") {
            // Both tags are gated on is_segment(), so an ordinary print's output is untouched.
            CHECK(gcode.find("SEGMENT_ID") == std::string::npos);
            REQUIRE(! result.moves.empty());
            for (const GCodeProcessorResult::MoveVertex &move : result.moves)
                CHECK(move.cut_segment_id == -1);
        }
    }
}

// A cut-plane segment can reach the G-code export with no layers at all: slicing keeps an empty
// segment so the rest of the partition finishes (PrintObjectSlice.cpp, "keep it as an empty
// segment"), and the trailing empty layers are then trimmed off, every one of them if the segment
// holds no material. The export took the largest print_z over every object's layers, and reading
// the largest of an empty list took the app down on 0.1.9 (Windows access violation in
// GCode::_do_export, cut planes with nonplanar printing). The field route needs a particular
// geometry, so this test empties the upper segment's layers after slicing and exports: no crash,
// and the plate's height comes from the segment that prints.
TEST_CASE("A cut-plane segment without layers still exports", "[cutplanes][Regression]")
{
    {
        Slic3r::Print print;
        Model         model;
        make_cut_cube_model(model, mid_plane_stack(10.));
        DynamicPrintConfig config = cut_plane_config();
        // Cut planes print by group (validate() refuses any other sequence)
        config.set_deserialize_strict({ { "skirt_loops", 0 },
                                        { "brim_type", "no_brim" },
                                        { "enable_prime_tower", 0 } });
        print.apply(model, config);
        const auto validation = print.validate();
        INFO("validate: " << validation.string);
        REQUIRE(validation.string.empty());
        print.set_status_silent();
        print.process();
        REQUIRE(print.objects().size() == 2);
        PrintObject *upper = print.get_object(1);
        upper->clear_support_layers();
        upper->clear_layers();
        REQUIRE(upper->layers().empty());

        const std::string gcode = Test::gcode(print);
        CHECK_FALSE(gcode.empty());
        // The header's height is the lower segment's: the cube is cut at Z 10, and a segment slices a
        // little past its own material (segment_slice_top()), so somewhat above 10 but nowhere near the
        // 20 mm cube, and not -1
        const size_t at = gcode.find("; max_z_height: ");
        REQUIRE(at != std::string::npos);
        const double max_z = std::stod(gcode.substr(at + 16));
        CHECK(max_z > 9.);
        CHECK(max_z < 15.);
    }
}
