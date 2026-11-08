// The include environment libslic3r_gui TUs get from pchheader.hpp: Windows.h first (PartPlate.hpp
// pulls in wx headers that need it).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_all.hpp>

#include <cmath>
#include <memory>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/PlatePrintability.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
// Plater.hpp also forward-declares a Slic3r::PartPlateList; name the GUI one explicitly.
using Plates = Slic3r::GUI::PartPlateList;

namespace {

constexpr double BED    = 256.;
constexpr double HEIGHT = 250.;

Pointfs square_bed() { return {{0., 0.}, {BED, 0.}, {BED, BED}, {0., BED}}; }

// Round bed with the same bounding box as square_bed().
Pointfs round_bed()
{
    Pointfs pts;
    const int n = 128;
    for (int i = 0; i < n; ++i) {
        const double a = 2. * PI * double(i) / double(n);
        pts.emplace_back(BED / 2. + BED / 2. * std::cos(a), BED / 2. + BED / 2. * std::sin(a));
    }
    return pts;
}

// A 20 mm cube with its lower-left-bottom corner at (x, y, z).
ModelObject* add_cube(Model& model, double x, double y, double z = 0.)
{
    ModelObject* obj = model.add_object();
    obj->name        = "cube";
    obj->add_volume(make_cube(20., 20., 20.));
    obj->add_instance()->set_offset(Vec3d(x, y, z));
    return obj;
}

std::unique_ptr<Plates> make_plates(Model& model, const Pointfs& shape, const Pointfs& exclude = {})
{
    auto plates = std::make_unique<Plates>(nullptr, &model, ptFFF);
    plates->reset_size(int(BED), int(BED), HEIGHT, false);
    plates->set_shapes(shape, exclude, {}, {}, {}, "", 0.f, 0.f);
    plates->reload_all_objects();
    return plates;
}

} // namespace

TEST_CASE("plate printability: a corner of the square bed is outside a round bed of the same size", "[plates][printability]")
{
    Model model;
    add_cube(model, 5., 5.);    // corner
    add_cube(model, 118., 118.); // centre

    {
        auto       plates = make_plates(model, square_bed());
        const auto result = assess_plate_printability(*plates, model);
        CHECK(result.instances[0][0].placement == InstancePlacement::Printable);
        CHECK(result.instances[1][0].placement == InstancePlacement::Printable);
        CHECK(result.plates[0].printable == 2);
    }
    {
        auto       plates = make_plates(model, round_bed());
        const auto result = assess_plate_printability(*plates, model);
        CHECK(result.instances[0][0].plate == 0);
        CHECK(result.instances[0][0].placement == InstancePlacement::OutsideBed);
        CHECK(result.instances[1][0].placement == InstancePlacement::Printable);
        CHECK(result.plates[0].printable == 1);
        CHECK(result.plates[0].outside_bed == 1);
    }
}

TEST_CASE("plate printability: the round-bed shortcut agrees with the exact test", "[plates][printability]")
{
    Model model;
    for (double x = -10.; x < BED; x += 14.)
        for (double y = -10.; y < BED; y += 14.)
            add_cube(model, x, y);
    auto              plates = make_plates(model, round_bed());
    const auto        result = assess_plate_printability(*plates, model);
    const BuildVolume bed(round_bed(), HEIGHT, {}, {});
    REQUIRE(bed.type() == BuildVolume_Type::Circle);
    int compared = 0;
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const InstancePrintability& st = result.instances[i][0];
        if (st.plate < 0)
            continue;
        const bool exact_inside = model.objects[i]->instances[0]->calc_print_volume_state(bed) == ModelInstancePVS_Inside;
        INFO("object " << i);
        CHECK((st.state == ModelInstancePVS_Inside) == exact_inside);
        CHECK(st.printable() == exact_inside);
        ++compared;
    }
    CHECK(compared > 100);
}

TEST_CASE("plate printability: exclude areas, printable flags and off-plate instances", "[plates][printability]")
{
    Model model;
    add_cube(model, 10., 10.);                                // in the exclude area
    add_cube(model, 118., 118.)->instances[0]->printable = false; // set not printable
    add_cube(model, 1000., 1000.);                            // on no plate
    auto       plates = make_plates(model, square_bed(), {{0., 0.}, {50., 0.}, {50., 50.}, {0., 50.}});
    const auto result = assess_plate_printability(*plates, model);

    CHECK(result.instances[0][0].placement == InstancePlacement::ExcludeArea);
    CHECK(result.instances[1][0].placement == InstancePlacement::Disabled);
    CHECK(result.instances[2][0].placement == InstancePlacement::OffPlate);
    CHECK(result.off_plate_count == 1);
    CHECK_FALSE(result.plate_has_printable(0));

    std::string reason, message;
    REQUIRE(explain_nothing_printable(result, 0, reason, message));
    CHECK(reason == "nothing_printable");
    CHECK(message.find("excluded area") != std::string::npos);
    CHECK(message.find("not printable") != std::string::npos);
    CHECK(message.find("outside the printable area") == std::string::npos);
}

TEST_CASE("plate printability: explanations name the cause", "[plates][printability]")
{
    std::string reason, message;

    SECTION("only outside the bed") {
        Model model;
        add_cube(model, 5., 5.);
        auto plates = make_plates(model, round_bed());
        REQUIRE(explain_nothing_printable(assess_plate_printability(*plates, model), 0, reason, message));
        CHECK(reason == "outside_bed");
        CHECK(message.find("1 object is outside the printable area of the bed.") != std::string::npos);
    }
    SECTION("only in an exclude area") {
        Model model;
        add_cube(model, 10., 10.);
        auto plates = make_plates(model, square_bed(), {{0., 0.}, {50., 0.}, {50., 50.}, {0., 50.}});
        REQUIRE(explain_nothing_printable(assess_plate_printability(*plates, model), 0, reason, message));
        CHECK(reason == "exclude_area");
        CHECK(message.find("outside the printable area") == std::string::npos);
    }
    SECTION("nothing on the plate") {
        Model model;
        add_cube(model, 1000., 1000.);
        auto plates = make_plates(model, square_bed());
        REQUIRE(explain_nothing_printable(assess_plate_printability(*plates, model), 0, reason, message));
        CHECK(reason == "empty_plate");
        CHECK(message.find("outside every plate") != std::string::npos);
    }
    SECTION("something printable") {
        Model model;
        add_cube(model, 118., 118.);
        auto plates = make_plates(model, round_bed());
        CHECK_FALSE(explain_nothing_printable(assess_plate_printability(*plates, model), 0, reason, message));
        CHECK_FALSE(explain_nothing_printable(assess_plate_printability(*plates, model), -1, reason, message));
    }
}

TEST_CASE("plate printability: instances on a second plate are tested against that plate's bed", "[plates][printability]")
{
    Model model;
    auto  plates = make_plates(model, round_bed());
    REQUIRE(plates->create_plate() == 1);
    const Vec3d origin = plates->get_plate(1)->get_origin();
    add_cube(model, origin.x() + 118., origin.y() + 118.); // centre of plate 2
    add_cube(model, origin.x() + 5., origin.y() + 5.);     // its corner, outside the round bed
    add_cube(model, 118., 118.);                           // centre of plate 1
    plates->reload_all_objects();

    const auto result = assess_plate_printability(*plates, model);
    CHECK(result.instances[0][0].plate == 1);
    CHECK(result.instances[0][0].placement == InstancePlacement::Printable);
    CHECK(result.instances[1][0].plate == 1);
    CHECK(result.instances[1][0].placement == InstancePlacement::OutsideBed);
    CHECK(result.instances[2][0].plate == 0);
    CHECK(result.instances[2][0].placement == InstancePlacement::Printable);
    CHECK(result.plates[1].printable == 1);
    CHECK(result.plates[1].outside_bed == 1);
}

TEST_CASE("plate printability: refreshing the plates' outside status after a bed change", "[plates][printability]")
{
    // The plate list's own test is rectangular, so what a bed change can flip in its cached status
    // is the exclude areas (the outline itself is covered by the fresh shape test).
    const Pointfs corner_exclude{{0., 0.}, {50., 0.}, {50., 50.}, {0., 50.}};
    Model         model;
    add_cube(model, 10., 10.);
    auto plates = make_plates(model, square_bed());
    REQUIRE(plates->get_plate(0)->has_printable_instances());
    CHECK(refresh_plate_outside_status(*plates, model).empty()); // nothing changed yet

    // Same bounding size, so nothing re-files the instance by itself.
    plates->set_shapes(square_bed(), corner_exclude, {}, {}, {}, "", 0.f, 0.f);
    const auto changed = refresh_plate_outside_status(*plates, model);
    REQUIRE(changed.size() == 1);
    CHECK(changed.front() == 0);
    CHECK_FALSE(plates->get_plate(0)->has_printable_instances());
    CHECK_FALSE(assess_plate_printability(*plates, model).plate_has_printable(0));
    CHECK(refresh_plate_outside_status(*plates, model).empty()); // settled

    // And back: the plate list agrees with the fresh verdict again.
    plates->set_shapes(square_bed(), {}, {}, {}, {}, "", 0.f, 0.f);
    CHECK(refresh_plate_outside_status(*plates, model).size() == 1);
    CHECK(plates->get_plate(0)->has_printable_instances());
    CHECK(assess_plate_printability(*plates, model).plate_has_printable(0));
}

TEST_CASE("plate printability: an object set not printable is not counted by the plate list either", "[plates][printability]")
{
    Model model;
    add_cube(model, 118., 118.)->printable = false;
    auto plates = make_plates(model, square_bed());
    CHECK_FALSE(plates->get_plate(0)->has_printable_instances());
    const auto result = assess_plate_printability(*plates, model);
    CHECK(result.instances[0][0].placement == InstancePlacement::Disabled);
    std::string reason, message;
    REQUIRE(explain_nothing_printable(result, 0, reason, message));
    CHECK(reason == "not_printable");
}

TEST_CASE("plate printability: the shortcut also serves a convex custom bed", "[plates][printability]")
{
    // An octagon: convex but neither a rectangle nor a circle.
    Pointfs octagon;
    for (int i = 0; i < 8; ++i) {
        const double a = 2. * PI * (double(i) + 0.5) / 8.;
        octagon.emplace_back(BED / 2. + BED / 2. * std::cos(a), BED / 2. + BED / 2. * std::sin(a));
    }
    Model model;
    for (double x = -10.; x < BED; x += 22.)
        for (double y = -10.; y < BED; y += 22.)
            add_cube(model, x, y);
    auto              plates = make_plates(model, octagon);
    const auto        result = assess_plate_printability(*plates, model);
    const BuildVolume bed(octagon, HEIGHT, {}, {});
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const InstancePrintability& st = result.instances[i][0];
        if (st.plate < 0)
            continue;
        INFO("object " << i);
        CHECK((st.state == ModelInstancePVS_Inside) ==
              (model.objects[i]->instances[0]->calc_print_volume_state(bed) == ModelInstancePVS_Inside));
    }
}

TEST_CASE("plate printability: an instance entirely below the bed is on no plate", "[plates][printability]")
{
    // The plates' volume starts at the bed surface, so such an instance is never filed to a plate.
    Model model;
    add_cube(model, 118., 118., -50.);
    auto       plates = make_plates(model, round_bed());
    const auto result = assess_plate_printability(*plates, model);
    CHECK(result.instances[0][0].placement == InstancePlacement::OffPlate);
    std::string reason, message;
    REQUIRE(explain_nothing_printable(result, 0, reason, message));
    CHECK(reason == "empty_plate");
}

TEST_CASE("plate printability: outside the bed is named before an exclude area", "[plates][printability]")
{
    // Too tall for the bed and standing in the exclude area: the cause to fix first is the height.
    Model model;
    ModelObject* tall = model.add_object();
    tall->add_volume(make_cube(20., 20., HEIGHT + 50.));
    tall->add_instance()->set_offset(Vec3d(10., 10., 0.));
    auto       plates = make_plates(model, square_bed(), {{0., 0.}, {50., 0.}, {50., 50.}, {0., 50.}});
    const auto result = assess_plate_printability(*plates, model);
    CHECK(result.instances[0][0].placement == InstancePlacement::OutsideBed);
    std::string reason, message;
    REQUIRE(explain_nothing_printable(result, 0, reason, message));
    CHECK(reason == "outside_bed");
}

TEST_CASE("plate printability: a sinking instance over an exclude area is refused by the plate list too", "[plates][printability]")
{
    Model model;
    add_cube(model, 10., 10., -5.); // sunk 5 mm, inside the exclude area
    auto plates = make_plates(model, square_bed(), {{0., 0.}, {50., 0.}, {50., 50.}, {0., 50.}});
    CHECK_FALSE(plates->get_plate(0)->has_printable_instances());
    const auto result = assess_plate_printability(*plates, model);
    CHECK(result.instances[0][0].placement == InstancePlacement::ExcludeArea);
}

TEST_CASE("plate printability: the plate list's printable counters agree on objects set not printable", "[plates][printability]")
{
    Model model;
    ModelObject* off = add_cube(model, 118., 118.);
    off->printable   = false; // instance stays printable, as some merge / calibration paths leave it
    auto plates      = make_plates(model, square_bed());
    Slic3r::GUI::PartPlate* plate = plates->get_plate(0);
    CHECK_FALSE(plate->has_printable_instances());
    CHECK(plate->printable_instance_size() == 0);
    CHECK(plate->is_all_instances_unprintable());
}
