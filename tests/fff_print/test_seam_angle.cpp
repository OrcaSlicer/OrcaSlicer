#include <catch2/catch_all.hpp>

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// seam_test.stl is a bracket whose square corners are chamfered and then slanted in 3D, so every
// corner column runs at 45 degrees: it moves 1 mm in X for every 1 mm of Z. Sliced, those corners
// measure 54.74 and 35.26 degrees - both below the 55 degrees the seam placer snapped at before
// seam_angle_threshold existed, which is why the aligned seam drifted across the chamfer face
// instead of sitting on the crease.
constexpr double seam_window_z_min = 20.5;
constexpr double seam_window_z_max = 23.5;

// A seam that stays on a corner traces that corner, and this part's corners are straight lines in
// (z, x, y) even though they are slanted. Distance from the line fitted through the seams therefore
// separates "pinned to the crease" from "wandering over the face", without depending on where the
// object ends up on the bed.
double max_distance_from_fitted_line(const std::vector<Vec3d> &seams)
{
    REQUIRE(seams.size() >= 3);

    double sum_z = 0.;
    for (const Vec3d &seam : seams)
        sum_z += seam.z();
    const double mean_z = sum_z / double(seams.size());

    double variance_z = 0.;
    Vec2d  covariance = Vec2d::Zero();
    Vec2d  mean_xy    = Vec2d::Zero();
    for (const Vec3d &seam : seams) {
        const double dz = seam.z() - mean_z;
        variance_z += dz * dz;
        covariance += dz * seam.head<2>();
        mean_xy    += seam.head<2>();
    }
    mean_xy /= double(seams.size());
    REQUIRE(variance_z > 0.);
    const Vec2d slope = covariance / variance_z;

    double worst = 0.;
    for (const Vec3d &seam : seams)
        worst = std::max(worst, (seam.head<2>() - (mean_xy + slope * (seam.z() - mean_z))).norm());
    return worst;
}

// Start point of each outer wall loop inside the Z window - that point is the seam. The role is
// announced on its own comment line rather than on each move, so the loop is found by tracking that
// state and taking the first extruding move under it.
std::vector<Vec3d> outer_wall_seams(const std::string &gcode)
{
    std::vector<Vec3d> seams;
    bool               outer_wall = false;
    bool               seam_taken = false;
    GCodeReader        reader;
    reader.parse_buffer(gcode, [&seams, &outer_wall, &seam_taken](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos) {
            outer_wall = comment.find("Outer wall") != std::string_view::npos ||
                         comment.find("External perimeter") != std::string_view::npos;
            seam_taken = false;
        }
        if (! outer_wall || seam_taken || ! line.extruding(self))
            return;
        // The reader applies the move only after this callback, so self still holds the point the
        // loop starts from.
        seam_taken = true;
        if (self.z() >= seam_window_z_min && self.z() <= seam_window_z_max)
            seams.emplace_back(self.x(), self.y(), self.z());
    });
    return seams;
}

std::vector<Vec3d> seams_with_snapping_angle(int degrees)
{
    // load_model() from test_utils.hpp only reads OBJ, and this fixture is the STL it was reported
    // against, so it is loaded the way tests/libslic3r/test_stl.cpp loads its own fixtures.
    Model             model;
    const std::string path = std::string(TEST_DATA_DIR) + "/test_stl/seam_test.stl";
    REQUIRE(load_stl(path.c_str(), &model));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spAligned));
    config.set_key_value("seam_angle_threshold", new ConfigOptionInt(degrees));

    return outer_wall_seams(Test::slice({ model.objects.front()->volumes.front()->mesh() }, config));
}

} // namespace

TEST_CASE("A lower corner snapping angle pins the aligned seam to a chamfer slanted in 3D", "[SeamPlacer]")
{
    const std::vector<Vec3d> seams_default = seams_with_snapping_angle(55);
    const std::vector<Vec3d> seams_snapped = seams_with_snapping_angle(30);

    REQUIRE(seams_default.size() == seams_snapped.size());

    // Over the 15 layers of the window this measures 1.2e-06 mm at 30 degrees - the seam is on the
    // corner vertex itself - against 0.199 mm at the default, about half a line width of drift.
    REQUIRE_THAT(max_distance_from_fitted_line(seams_snapped), Catch::Matchers::WithinAbs(0., 0.01));
    REQUIRE(max_distance_from_fitted_line(seams_default) > 0.05);
}
