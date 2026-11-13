#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/Point.hpp"
#include <iterator>
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <limits>
#include "libslic3r/Model.hpp"
#include <map>
#include <memory>
#include <math.h>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Fill/FillAdaptive.hpp"
#include "libslic3r/Fill/FillGyroid.hpp"
#include "libslic3r/Fill/AdaptiveInfillField.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/IntersectionPoints.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle = 0, double density = 1.0);

#if 0
TEST_CASE("Adjusted solid distance", "[Fill]") {
    int surface_width = 250;
    int distance = Slic3r::Flow::solid_spacing(surface_width, 47);
    REQUIRE(distance == Catch::Approx(50));
    REQUIRE(surface_width % distance == 0);
}
#endif

TEST_CASE("Pattern path length", "[Fill]") {
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
    filler->angle = float(-(PI)/2.0);
	FillParams fill_params;
	filler->spacing = 5;
	fill_params.dont_adjust = true;
	//fill_params.endpoints_overlap = false;
	fill_params.density = float(filler->spacing / 50.0);

    auto test = [&filler, &fill_params] (const ExPolygon& poly) -> Slic3r::Polylines {
        Slic3r::Surface surface(stTop, poly);
        return filler->fill_surface(&surface, fill_params);
    };

    SECTION("Square") {
        Slic3r::Points test_set;
        test_set.reserve(4);
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        for (size_t i = 0; i < 4; ++i) {
            std::transform(points.cbegin()+i, points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } ); 
            std::transform(points.cbegin(), points.cbegin()+i, std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
            Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
            REQUIRE(paths.size() == 1); // one continuous path

            // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
            // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
            // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
            REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length

            test_set.clear();
        }
    }
    SECTION("Diamond with endpoints on grid") {
        std::vector<Vec2d> points {Vec2d(0,0), Vec2d(100,0), Vec2d(150,50), Vec2d(100,100), Vec2d(0,100), Vec2d(-50,50)};
        Slic3r::Points test_set;
        test_set.reserve(6);
        std::transform(points.cbegin(), points.cend(),   std::back_inserter(test_set), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(test_set));
        REQUIRE(paths.size() == 1); // one continuous path
    }

    SECTION("Square with hole") {
        std::vector<Vec2d> square {Vec2d(0,0), Vec2d(100,0), Vec2d(100,100), Vec2d(0,100)};
        std::vector<Vec2d> hole {Vec2d(25,25), Vec2d(75,25), Vec2d(75,75), Vec2d(25,75) };
        std::reverse(hole.begin(), hole.end());

        Slic3r::Points test_hole;
        Slic3r::Points test_square;

        std::transform(square.cbegin(), square.cend(), std::back_inserter(test_square), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );
        std::transform(hole.cbegin(), hole.cend(), std::back_inserter(test_hole), [] (const Vec2d& a) -> Point { return Point::new_scale(a.x(), a.y()); } );

        for (double angle : {-(PI/2.0), -(PI/4.0), -(PI), PI/2.0, PI}) {
            for (double spacing : {25.0, 5.0, 7.5, 8.5}) {
				fill_params.density = float(filler->spacing / spacing);
                filler->angle = float(angle);
                ExPolygon e(test_square, test_hole);
                Slic3r::Polylines paths = test(e);
#if 0
				{
					BoundingBox bbox = get_extents(e);
					SVG svg("c:\\data\\temp\\square_with_holes.svg", bbox);
					svg.draw(e);
					svg.draw(paths);
					svg.Close();
				}
#endif
                REQUIRE((paths.size() >= 1 && paths.size() <= 3));
                // paths don't cross hole
                REQUIRE(diff_pl(paths, offset(e, float(SCALED_EPSILON*10))).size() == 0);
            }
        }
    }
    SECTION("Regression: Missing infill segments in some rare circumstances") {
        filler->angle = float(PI/4.0);
		fill_params.dont_adjust = false;
        filler->spacing = 0.654498;
        //filler->endpoints_overlap = unscale(359974);
		fill_params.density = 1;
        filler->layer_id = 66;
        filler->z = 20.15;

        Slic3r::Points points {Point(25771516,14142125),Point(14142138,25771515),Point(2512749,14142131),Point(14142125,2512749)};
        Slic3r::Polylines paths = test(Slic3r::ExPolygon(points));
        REQUIRE(paths.size() == 1); // one continuous path

        // TODO: determine what the "Expected length" should be for rectilinear fill of a 100x100 polygon. 
        // This check only checks that it's above scale(3*100 + 2*50) + scaled_epsilon.
        // ok abs($paths->[0]->length - scale(3*100 + 2*50)) - scaled_epsilon, 'path has expected length';
        REQUIRE(std::abs(paths[0].length() - static_cast<double>(scale_(3*100 + 2*50))) - SCALED_EPSILON > 0); // path has expected length
    }

    SECTION("Rotated Square") {
        Slic3r::Points square { Point::new_scale(0,0), Point::new_scale(50,0), Point::new_scale(50,50), Point::new_scale(0,50)};
        Slic3r::ExPolygon expolygon(square);
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
		filler->bounding_box = get_extents(expolygon.contour);
        filler->angle = 0;
        
        Surface surface(stTop, expolygon);
        auto flow = Slic3r::Flow(0.69f, 0.4f, 0.50f);

		FillParams fill_params;
		fill_params.density = 1.0;
		filler->spacing = flow.spacing();

        for (auto angle : { 0.0, 45.0}) {
            surface.expolygon.rotate(angle, Point(0,0));
            Polylines paths = filler->fill_surface(&surface, fill_params);
            REQUIRE(paths.size() == 1);
        }
    }

    #if 0   // Disabled temporarily due to precision issues on the Mac VM
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(6883102, 9598327.01296997),
            Point::new_scale(6883102, 20327272.01297),
            Point::new_scale(3116896, 20327272.01297),
            Point::new_scale(3116896, 9598327.01296997) 
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        for (size_t i = 0; i <= 20; ++i)
        {
            expolygon.scale(1.05);
            REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        }
    }
    #endif

    SECTION("Solid surface fill") {
        Slic3r::Points points {
                Slic3r::Point(59515297,5422499),Slic3r::Point(59531249,5578697),Slic3r::Point(59695801,6123186),
                Slic3r::Point(59965713,6630228),Slic3r::Point(60328214,7070685),Slic3r::Point(60773285,7434379),
                Slic3r::Point(61274561,7702115),Slic3r::Point(61819378,7866770),Slic3r::Point(62390306,7924789),
                Slic3r::Point(62958700,7866744),Slic3r::Point(63503012,7702244),Slic3r::Point(64007365,7434357),
                Slic3r::Point(64449960,7070398),Slic3r::Point(64809327,6634999),Slic3r::Point(65082143,6123325),
                Slic3r::Point(65245005,5584454),Slic3r::Point(65266967,5422499),Slic3r::Point(66267307,5422499),
                Slic3r::Point(66269190,8310081),Slic3r::Point(66275379,17810072),Slic3r::Point(66277259,20697500),
                Slic3r::Point(65267237,20697500),Slic3r::Point(65245004,20533538),Slic3r::Point(65082082,19994444),
                Slic3r::Point(64811462,19488579),Slic3r::Point(64450624,19048208),Slic3r::Point(64012101,18686514),
                Slic3r::Point(63503122,18415781),Slic3r::Point(62959151,18251378),Slic3r::Point(62453416,18198442),
                Slic3r::Point(62390147,18197355),Slic3r::Point(62200087,18200576),Slic3r::Point(61813519,18252990),
                Slic3r::Point(61274433,18415918),Slic3r::Point(60768598,18686517),Slic3r::Point(60327567,19047892),
                Slic3r::Point(59963609,19493297),Slic3r::Point(59695865,19994587),Slic3r::Point(59531222,20539379),
                Slic3r::Point(59515153,20697500),Slic3r::Point(58502480,20697500),Slic3r::Point(58502480,5422499)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55) == true);
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.55, PI/2.0) == true);
    }
    SECTION("Solid surface fill") {
        Slic3r::Points points {
            Point::new_scale(0,0),Point::new_scale(98,0),Point::new_scale(98,10), Point::new_scale(0,10)
        };
        Slic3r::ExPolygon expolygon(points);
         
        REQUIRE(test_if_solid_surface_filled(expolygon, 0.5, 45.0, 0.99) == true);
    }
}

/*
{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::Polyline::Collection->new(
            Slic3r::Polyline->new([4,0], [10,0], [15,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @$_, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([0,15], [0,18], [0,20]),
            Slic3r::Polyline->new([0,10], [0,8], [0,5]),
            );
    is_deeply
        [ map $_->[Y], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(0,30), 0)} ],
        [20, 18, 15, 10, 8, 5],
        'chained path';
}

{
    my $collection = Slic3r::ExtrusionPath::Collection->new(
            map Slic3r::ExtrusionPath->new(polyline => $_, role => 0, mm3_per_mm => 1),
            Slic3r::Polyline->new([15,0], [10,0], [4,0]),
            Slic3r::Polyline->new([10,5], [15,5], [20,5]),
            );
    is_deeply
        [ map $_->[X], map @{$_->polyline}, @{$collection->chained_path_from(Slic3r::Point->new(30,0), 0)} ],
        [reverse 4, 10, 15, 10, 15, 20],
        'chained path';
}

for my $pattern (qw(rectilinear honeycomb hilbertcurve concentric)) {
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('fill_pattern', $pattern);
    $config->set('external_fill_pattern', $pattern);
    $config->set('perimeters', 1);
    $config->set('skirts', 0);
    $config->set('fill_density', 20);
    $config->set('layer_height', 0.05);
    $config->set('perimeter_extruder', 1);
    $config->set('infill_extruder', 2);
    my $print = Slic3r::Test::init_print('20mm_cube', config => $config, scale => 2);
    ok my $gcode = Slic3r::Test::gcode($print), "successful $pattern infill generation";
    my $tool = undef;
    my @perimeter_points = my @infill_points = ();
    Slic3r::GCode::Reader->new->parse($gcode, sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->perimeter_extruder-1) {
            push @perimeter_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            } elsif ($tool == $config->infill_extruder-1) {
            push @infill_points, Slic3r::Point->new_scale($args->{X}, $args->{Y});
            }
            }
            });
    my $convex_hull = convex_hull(\@perimeter_points);
    ok !(defined first { !$convex_hull->contains_point($_) } @infill_points), "infill does not exceed perimeters ($pattern)";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('infill_only_where_needed', 1);
    $config->set('bottom_solid_layers', 0);
    $config->set('infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.5);
    $config->set('fill_density', 40);
    $config->set('cooling', 0);                 # for preventing speeds from being altered
        $config->set('first_layer_speed', '100%');  # for preventing speeds from being altered

        my $test = sub {
            my $print = Slic3r::Test::init_print('pyramid', config => $config);

            my $tool = undef;
            my @infill_extrusions = ();  # array of polylines
                Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
                        my ($self, $cmd, $args, $info) = @_;

                        if ($cmd =~ /^T(\d+)/) {
                        $tool = $1;
                        } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
                        if ($tool == $config->infill_extruder-1) {
                        push @infill_extrusions, Slic3r::Line->new_scale(
                                [ $self->X, $self->Y ],
                                [ $info->{new_X}, $info->{new_Y} ],
                                );
                        }
                        }
                        });
            return 0 if !@infill_extrusions;  # prevent calling convex_hull() with no points

                my $convex_hull = convex_hull([ map $_->pp, map @$_, @infill_extrusions ]);
            return unscale unscale sum(map $_->area, @{offset([$convex_hull], scale(+$config->infill_extrusion_width/2))});
        };

    my $tolerance = 5;  # mm^2

        $config->set('solid_infill_below_area', 0);
    ok $test->() < $tolerance,
       'no infill is generated when using infill_only_where_needed on a pyramid';

    $config->set('solid_infill_below_area', 70);
    ok abs($test->() - $config->solid_infill_below_area) < $tolerance,
       'infill is only generated under the forced solid shells';
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 1);
    $config->set('fill_density', 0);
    $config->set('top_solid_layers', 0);
    $config->set('bottom_solid_layers', 0);
    $config->set('solid_infill_below_area', 20000000);
    $config->set('solid_infill_every_layers', 2);
    $config->set('perimeter_speed', 99);
    $config->set('external_perimeter_speed', 99);
    $config->set('cooling', 0);
    $config->set('first_layer_speed', '100%');

    my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %layers_with_extrusion = ();
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd eq 'G1' && $info->{dist_XY} > 0 && $info->{extruding}) {
            if (($args->{F} // $self->F) != $config->perimeter_speed*60) {
            $layers_with_extrusion{$self->Z} = ($args->{F} // $self->F);
            }
            }
            });

    ok !%layers_with_extrusion,
       "solid_infill_below_area and solid_infill_every_layers are ignored when fill_density is 0";
}

{
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('skirts', 0);
    $config->set('perimeters', 3);
    $config->set('fill_density', 0);
    $config->set('layer_height', 0.2);
    $config->set('first_layer_height', 0.2);
    $config->set('nozzle_diameter', [0.35]);
    $config->set('infill_extruder', 2);
    $config->set('solid_infill_extruder', 2);
    $config->set('infill_extrusion_width', 0.52);
    $config->set('solid_infill_extrusion_width', 0.52);
    $config->set('first_layer_extrusion_width', 0);

    my $print = Slic3r::Test::init_print('A', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            if ($tool == $config->infill_extruder-1) {
            my $z = 1 * $self->Z;
            $infill{$z} ||= [];
            push @{$infill{$z}}, Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            }
            }
            });
    my $grow_d = scale($config->infill_extrusion_width)/2;
    my $layer0_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.2} } ]);
    my $layer1_infill = union([ map @{$_->grow($grow_d)}, @{ $infill{0.4} } ]);
    my $diff = diff($layer0_infill, $layer1_infill);
    $diff = offset2_ex($diff, -$grow_d, +$grow_d);
    $diff = [ grep { $_->area > 2*(($grow_d*2)**2) } @$diff ];
    is scalar(@$diff), 0, 'no missing parts in solid shell when fill_density is 0';
}

{
    # GH: #2697
    my $config = Slic3r::Config->new_from_defaults;
    $config->set('perimeter_extrusion_width', 0.72);
    $config->set('top_infill_extrusion_width', 0.1);
    $config->set('infill_extruder', 2);         # in order to distinguish infill
        $config->set('solid_infill_extruder', 2);   # in order to distinguish infill

        my $print = Slic3r::Test::init_print('20mm_cube', config => $config);
    my %infill = ();  # Z => [ Line, Line ... ]
        my %other  = ();  # Z => [ Line, Line ... ]
        my $tool = undef;
    Slic3r::GCode::Reader->new->parse(Slic3r::Test::gcode($print), sub {
            my ($self, $cmd, $args, $info) = @_;

            if ($cmd =~ /^T(\d+)/) {
            $tool = $1;
            } elsif ($cmd eq 'G1' && $info->{extruding} && $info->{dist_XY} > 0) {
            my $z = 1 * $self->Z;
            my $line = Slic3r::Line->new_scale(
                    [ $self->X, $self->Y ],
                    [ $info->{new_X}, $info->{new_Y} ],
                    );
            if ($tool == $config->infill_extruder-1) {
            $infill{$z} //= [];
            push @{$infill{$z}}, $line;
            } else {
            $other{$z} //= [];
            push @{$other{$z}}, $line;
            }
            }
            });
    my $top_z = max(keys %infill);
    my $top_infill_grow_d = scale($config->top_infill_extrusion_width)/2;
    my $top_infill = union([ map @{$_->grow($top_infill_grow_d)}, @{ $infill{$top_z} } ]);
    my $perimeters_grow_d = scale($config->perimeter_extrusion_width)/2;
    my $perimeters = union([ map @{$_->grow($perimeters_grow_d)}, @{ $other{$top_z} } ]);
    my $covered = union_ex([ @$top_infill, @$perimeters ]);
    my @holes = map @{$_->holes}, @$covered;
    ok sum(map unscale unscale $_->area*-1, @holes) < 1, 'no gaps between top solid infill and perimeters';
}
*/

bool test_if_solid_surface_filled(const ExPolygon& expolygon, double flow_spacing, double angle, double density)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("rectilinear"));
	filler->bounding_box = get_extents(expolygon.contour);
    filler->angle = float(angle);

	Flow flow(float(flow_spacing), 0.4f, float(flow_spacing));
	filler->spacing = flow.spacing();

	FillParams fill_params;
	fill_params.density = float(density);
	fill_params.dont_adjust = false;

	Surface surface(stBottom, expolygon);
	Slic3r::Polylines paths = filler->fill_surface(&surface, fill_params);

    // check whether any part was left uncovered
    Polygons grown_paths;
    grown_paths.reserve(paths.size());

    // figure out what is actually going on here re: data types
    float line_offset = float(scale_(filler->spacing / 2.0 + EPSILON));
    std::for_each(paths.begin(), paths.end(), [line_offset, &grown_paths] (const Slic3r::Polyline& p) {
        polygons_append(grown_paths, offset(p, line_offset));
    });

	// Shrink the initial expolygon a bit, this simulates the infill / perimeter overlap that we usually apply.
    ExPolygons uncovered = diff_ex(offset(expolygon, - float(0.2 * scale_(flow_spacing))), grown_paths, ApplySafetyOffset::Yes);

    // ignore very small dots
    const double scaled_flow_spacing = std::pow(scale_(flow_spacing), 2);
    uncovered.erase(std::remove_if(uncovered.begin(), uncovered.end(), [scaled_flow_spacing](const ExPolygon& poly) { return poly.area() < scaled_flow_spacing; }), uncovered.end());

#if 0
	if (! uncovered.empty()) {
		BoundingBox bbox = get_extents(expolygon.contour);
		bbox.merge(get_extents(uncovered));
		bbox.merge(get_extents(grown_paths));
		SVG svg("c:\\data\\temp\\test_if_solid_surface_filled.svg", bbox);
		svg.draw(expolygon);
		svg.draw(uncovered, "red");
		svg.Close();
	}
#endif

    return uncovered.empty(); // solid surface is fully filled
}

// Length-weighted dominant direction of the layer's role_wanted extrusions, whole degrees
// [0, 180), or -1 if it has none. Needs a line pattern such as monotonic or rectilinear.
template<typename RolePred> static int dominant_fill_angle(const Layer &layer, RolePred role_wanted)
{
    std::map<int, double> weight_per_degree;

    auto account = [&weight_per_degree, &role_wanted](const ExtrusionPath &path) {
        if (!role_wanted(path.role()))
            return;
        const Points3 &pts = path.polyline.points;
        for (size_t i = 1; i < pts.size(); ++i) {
            const double dx = double(pts[i].x() - pts[i - 1].x());
            const double dy = double(pts[i].y() - pts[i - 1].y());
            const double len = std::hypot(dx, dy);
            if (len <= 0.)
                continue;
            int deg = int(std::lround(Geometry::rad2deg(std::atan2(dy, dx)))) % 180;
            if (deg < 0)
                deg += 180;
            weight_per_degree[deg] += len;
        }
    };

    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
            if (auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                account(*path);
            else if (auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                for (const ExtrusionPath &p : multi->paths)
                    account(p);
            else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                for (const ExtrusionPath &p : loop->paths)
                    account(p);
        }

    if (weight_per_degree.empty())
        return -1;
    return std::max_element(weight_per_degree.begin(), weight_per_degree.end(),
                            [](const auto &a, const auto &b) { return a.second < b.second; })->first;
}

template<typename RolePred> static std::vector<int> angles_per_layer(const Print &print, RolePred role_wanted)
{
    std::vector<int> angles;
    for (const Layer *layer : print.objects().front()->layers())
        angles.push_back(dominant_fill_angle(*layer, role_wanted));
    return angles;
}

static bool solid_role(ExtrusionRole role) { return is_solid_infill(role) && role != erIroning; }
static bool sparse_role(ExtrusionRole role) { return role == erInternalInfill; }
static bool ironing_role(ExtrusionRole role) { return role == erIroning; }

// The angle from line direction `from` to `to`, in whole degrees [0, 180).
static int line_angle_delta(int from, int to) { return ((to - from) % 180 + 180) % 180; }

static Polylines layer_paths(const Layer &layer, ExtrusionRole role)
{
    Polylines polylines;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->fills.flatten().entities)
            if (entity->role() == role)
                entity->collect_polylines(polylines);
    return polylines;
}

TEST_CASE("Infill rotation template is unaffected by a raft", "[Fill][Regression]")
{
    // More angles than raft layers, so a raft cannot alias back to the same angle.
    const std::string template_string = GENERATE("+45", "0,25,50,75,100,125,150");
    const int raft_layers = GENERATE(1, 3);
    CAPTURE(template_string, raft_layers);

    auto angles_for = [&template_string](int rafts) {
        Print print;
        // 100% density makes every layer solid, so the template shows on all 100, not just shells.
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"solid_infill_rotate_template", template_string},
                                             {"sparse_infill_density", "100%"},
                                             {"internal_solid_infill_pattern", "monotonic"},
                                             {"layer_height", 0.2},
                                             {"raft_layers", rafts}});
        return angles_per_layer(print, solid_role);
    };

    const std::vector<int> without_raft = angles_for(0);
    const std::vector<int> with_raft    = angles_for(raft_layers);

    REQUIRE(without_raft.size() == 100);
    REQUIRE(with_raft.size() == without_raft.size());
    REQUIRE(std::count(without_raft.begin(), without_raft.end(), -1) == 0);
    CHECK(with_raft == without_raft);
}

TEST_CASE("Sparse infill rotation template turns the infill layer by layer", "[Fill]")
{
    const std::vector<int> expected_cycle = {0, 25, 50, 75, 100, 125, 150};

    Print print;
    // No shells, so every layer is sparse infill rather than solid.
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(10)}, print,
                                        {{"sparse_infill_rotate_template", "0,25,50,75,100,125,150"},
                                         {"sparse_infill_density", "40%"},
                                         {"sparse_infill_pattern", "rectilinear"},
                                         {"top_shell_layers", 0},
                                         {"bottom_shell_layers", 0},
                                         {"layer_height", 0.2}});

    const std::vector<int> angles = angles_per_layer(print, sparse_role);
    REQUIRE(angles.size() == 50);
    REQUIRE(std::count(angles.begin(), angles.end(), -1) == 0);

    std::vector<int> expected;
    for (size_t i = 0; i < angles.size(); ++i)
        expected.push_back(expected_cycle[i % expected_cycle.size()]);
    CHECK(angles == expected);
}

TEST_CASE("Infill rotation template layer count modifier holds each angle for N layers", "[Fill]")
{
    Print print;
    // "+45#2" turns 45 degrees every 2 layers, so equal angles come in pairs.
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(10)}, print,
                                        {{"solid_infill_rotate_template", "+45#2"},
                                         {"sparse_infill_density", "100%"},
                                         {"internal_solid_infill_pattern", "monotonic"},
                                         {"layer_height", 0.2}});

    const std::vector<int> angles = angles_per_layer(print, solid_role);
    REQUIRE(angles.size() == 50);
    REQUIRE(std::count(angles.begin(), angles.end(), -1) == 0);

    std::vector<int> run_lengths;
    for (size_t i = 0; i < angles.size();) {
        size_t j = i;
        while (j < angles.size() && angles[j] == angles[i])
            ++j;
        run_lengths.push_back(int(j - i));
        i = j;
    }
    // The first and last runs can be clipped by the start and end of the object.
    REQUIRE(run_lengths.size() > 3);
    const std::vector<int> interior(run_lengths.begin() + 1, run_lengths.end() - 1);
    CHECK(std::count(interior.begin(), interior.end(), 2) == int(interior.size()));
}

TEST_CASE("Z anti-aliasing keeps the infill rotation template's step", "[Fill]")
{
    Print print;
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(10)}, print,
                                        {{"solid_infill_rotate_template", "+45"},
                                         {"sparse_infill_density", "100%"},
                                         {"internal_solid_infill_pattern", "monotonic"},
                                         {"zaa_enabled", 1},
                                         {"zaa_min_z", 0.05},
                                         {"layer_height", 0.2}});

    // Z contouring varies the layer heights, so the layer count is not 10mm / 0.2mm here.
    const std::vector<int> angles = angles_per_layer(print, solid_role);
    REQUIRE(angles.size() > 10);
    REQUIRE(std::count(angles.begin(), angles.end(), -1) == 0);

    // Z contouring may change when the template advances, but each step must still be 45 degrees.
    int steps = 0;
    for (size_t i = 1; i < angles.size(); ++i) {
        const int delta = line_angle_delta(angles[i - 1], angles[i]);
        CAPTURE(i, angles[i - 1], angles[i]);
        // Split rather than "delta == 0 || delta == 45" so Catch2 can show the operands.
        REQUIRE(delta % 45 == 0);
        REQUIRE(delta <= 45);
        steps += delta == 45;
    }
    CHECK(steps > 0);
}

static constexpr double rotate_template_first_layer_height = 0.2;

// Slices a 20-layer box whose first layer is first_layer_height thick and the rest 0.2mm.
static void slice_for_rotate_templates(Print &print, double first_layer_height)
{
    Slic3r::Test::init_and_process_print({make_cube(20, 20, 3.8 + first_layer_height)}, print,
                                        {{"layer_height", 0.2},
                                         {"initial_layer_print_height", first_layer_height},
                                         // Read by the B and T counts.
                                         {"bottom_shell_layers", 3},
                                         {"top_shell_layers", 4}});
}

// The expected angles are the parser's current output, so a deliberate change to a template's output must update them.
static void check_rotate_template(const std::string &rotate_template, const std::vector<double> &expected,
                                  double first_layer_height = rotate_template_first_layer_height)
{
    CAPTURE(rotate_template);
    Print print;
    slice_for_rotate_templates(print, first_layer_height);
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        // The infill direction does not apply when a template is set.
        const double angle = Geometry::rad2deg(calculate_infill_rotation_angle(&object, object.get_layer(i)->id(), 0., rotate_template));
        CAPTURE(i, angle, expected[i]);
        // Compare around the circle, so 360 matches 0.
        CHECK_THAT(std::remainder(angle - expected[i], 360.), Catch::Matchers::WithinAbs(0., 0.01));
    }
}

// Angles that several templates give the 20 layers of slice_for_rotate_templates().
static const std::vector<double> turn_9_per_layer  = {9, 18, 27, 36, 45, 54, 63, 72, 81, 90, 99, 108, 117, 126, 135, 144, 153, 162, 171, 180};
static const std::vector<double> turn_18_per_layer = {18, 36, 54, 72, 90, 108, 126, 144, 162, 180, 198, 216, 234, 252, 270, 288, 306, 324, 342, 360};
static const std::vector<double> step_45_every_5_layers = {45, 45, 45, 45, 45, 90, 90, 90, 90, 90, 135, 135, 135, 135, 135, 180, 180, 180, 180, 180};
static const std::vector<double> turn_10_per_layer_from_0 = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190};

TEST_CASE("Infill rotation template without operators cycles through its angles", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        {"0,90", {0, 90, 0, 90, 0, 90, 0, 90, 0, 90, 0, 90, 0, 90, 0, 90, 0, 90, 0, 90}},
        {"0,45,90", {0, 45, 90, 0, 45, 90, 0, 45, 90, 0, 45, 90, 0, 45, 90, 0, 45, 90, 0, 45}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Relative infill rotation template turns by its angle on every layer", "[Fill][RotateTemplate]")
{
    check_rotate_template("+5", {5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100});
}

TEST_CASE("Infill rotation template angle can be a percent of a full turn", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        {"25%", {90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90}},
        {"+15%", {54, 108, 162, 216, 270, 324, 18, 72, 126, 180, 234, 288, 342, 36, 90, 144, 198, 252, 306, 0}},
        {"+25%/10", turn_9_per_layer},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Relative infill rotation template range spreads the turn over its layers", "[Fill][RotateTemplate]")
{
    check_rotate_template("+90/10", turn_9_per_layer);
}

TEST_CASE("Absolute infill rotation template range holds its angle for the range", "[Fill][RotateTemplate]")
{
    check_rotate_template("0, 90/10", {0, 90, 90, 90, 90, 90, 90, 90, 90, 90, 90, 0, 90, 90, 90, 90, 90, 90, 90, 90});
}

TEST_CASE("Infill rotation template step joint holds the end angle of each range", "[Fill][RotateTemplate]")
{
    check_rotate_template("+45#5", step_45_every_5_layers);
}

TEST_CASE("Infill rotation template middle joint holds half the turn for the range", "[Fill][RotateTemplate]")
{
    check_rotate_template("+90|5",
                          {45, 45, 45, 45, 45, 135, 135, 135, 135, 135, 225, 225, 225, 225, 225, 315, 315, 315, 315, 315});
}

TEST_CASE("Infill rotation template curve joints shape the turn over the range", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        {"+90N10", {0.58, 4.38, 13.38, 27.58, 45, 62.42, 76.62, 85.62, 89.42, 90,
                    90.58, 94.38, 103.38, 117.58, 135, 152.42, 166.62, 175.62, 179.42, 180}},
        {"+90n10", {4.79, 11.19, 20.19, 31.79, 45, 58.21, 69.81, 78.81, 85.21, 90,
                    94.79, 101.19, 110.19, 121.79, 135, 148.21, 159.81, 168.81, 175.21, 180}},
        {"+90Z10", {17.42, 31.62, 40.62, 44.42, 45, 45.58, 49.38, 58.38, 72.58, 90,
                    107.42, 121.62, 130.62, 134.42, 135, 135.58, 139.38, 148.38, 162.58, 180}},
        {"+90z10", {13.21, 24.81, 33.81, 40.21, 45, 49.79, 56.19, 65.19, 76.79, 90,
                    103.21, 114.81, 123.81, 130.21, 135, 139.79, 146.19, 155.19, 166.79, 180}},
        {"+90$10", {18.43, 26.57, 33.21, 39.23, 45, 50.77, 56.79, 63.43, 71.57, 90,
                    108.43, 116.57, 123.21, 129.23, 135, 140.77, 146.79, 153.43, 161.57, 180}},
        {"+90L10", {14.08, 27.81, 40.86, 52.9, 63.64, 72.81, 80.19, 85.6, 88.89, 90,
                    104.08, 117.81, 130.86, 142.9, 153.64, 162.81, 170.19, 175.6, 178.89, 180}},
        {"+90l10", {1.11, 4.4, 9.81, 17.19, 26.36, 37.1, 49.14, 62.19, 75.92, 90,
                    91.11, 94.4, 99.81, 107.19, 116.36, 127.1, 139.14, 152.19, 165.92, 180}},
        {"+90U10", {17.1, 32.4, 45.9, 57.6, 67.5, 75.6, 81.9, 86.4, 89.1, 90,
                    107.1, 122.4, 135.9, 147.6, 157.5, 165.6, 171.9, 176.4, 179.1, 180}},
        {"+90u10", {72.9, 57.6, 44.1, 32.4, 22.5, 14.4, 8.1, 3.6, 0.9, 0,
                    162.9, 147.6, 134.1, 122.4, 112.5, 104.4, 98.1, 93.6, 90.9, 90}},
        {"+90Q10", {24.39, 43.92, 59.13, 70.56, 78.75, 84.24, 87.57, 89.28, 89.91, 90,
                    114.39, 133.92, 149.13, 160.56, 168.75, 174.24, 177.57, 179.28, 179.91, 180}},
        {"+90q10", {65.61, 46.08, 30.87, 19.44, 11.25, 5.76, 2.43, 0.72, 0.09, 0,
                    155.61, 136.08, 120.87, 109.44, 101.25, 95.76, 92.43, 90.72, 90.09, 90}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Infill rotation template random joints vary the angle within their bounds", "[Fill][RotateTemplate]")
{
    Print print;
    slice_for_rotate_templates(print, rotate_template_first_layer_height);
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() == turn_9_per_layer.size());
    // Like "+90/10", both turn 90 degrees over every 10 layers, but they draw each angle from rand().
    size_t anywhere_off_the_line = 0;
    size_t near_line_off_the_line = 0;
    for (size_t i = 0; i < turn_9_per_layer.size(); ++i) {
        const double range_start = 90. * double(i / 10);
        const double linear      = turn_9_per_layer[i];
        const double anywhere    = Geometry::rad2deg(calculate_infill_rotation_angle(&object, object.get_layer(i)->id(), 0., "+90~10"));
        const double near_line   = Geometry::rad2deg(calculate_infill_rotation_angle(&object, object.get_layer(i)->id(), 0., "+90^10"));
        CAPTURE(i, range_start, linear, anywhere, near_line);
        // ~ picks any angle of the range.
        CHECK(anywhere >= range_start - 0.01);
        CHECK(anywhere <= range_start + 90.01);
        // ^ stays within half the range of the linear turn.
        CHECK(std::abs(near_line - linear) <= 45.01);
        anywhere_off_the_line += std::abs(anywhere - linear) > 0.01;
        near_line_off_the_line += std::abs(near_line - linear) > 0.01;
    }
    CHECK(anywhere_off_the_line > 0);
    CHECK(near_line_off_the_line > 0);
}

TEST_CASE("Infill rotation template minus before the count flips the joint", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        {"+90/-10", {81, 72, 63, 54, 45, 36, 27, 18, 9, 0, 171, 162, 153, 144, 135, 126, 117, 108, 99, 90}},
        {"+90L-10", {88.89, 85.6, 80.19, 72.81, 63.64, 52.9, 40.86, 27.81, 14.08, 0,
                     178.89, 175.6, 170.19, 162.81, 153.64, 142.9, 130.86, 117.81, 104.08, 90}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Infill rotation template range can be a height in mm, cm, m, feet, inches or percent", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        // 1mm is 5 layers.
        {"+90/1mm", turn_18_per_layer},
        // 0.2cm, 0.002m and 50% of the 4mm object are each 10 layers.
        {"+90/0.2cm", turn_9_per_layer},
        {"+90/0.002m", turn_9_per_layer},
        {"+90/50%", turn_9_per_layer},
        // 0.01' is 3.048mm and 0.1" is 2.54mm, which do not end on a layer.
        {"+90/0.01'", {5.91, 11.81, 17.72, 23.62, 29.53, 35.43, 41.34, 47.24, 53.15, 59.06,
                       64.96, 70.87, 76.77, 82.68, 88.58, 95.91, 101.81, 107.72, 113.62, 119.53}},
        {"+90/0.1\"", {7.09, 14.17, 21.26, 28.35, 35.43, 42.52, 49.61, 56.69, 63.78, 70.87,
                       77.95, 85.04, 92.13, 97.09, 104.17, 111.26, 118.35, 125.43, 132.52, 139.61}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Infill rotation template layer height unit counts layer_height under a thicker first layer", "[Fill][RotateTemplate]")
{
    const auto [first_layer_height, expected] = GENERATE(table<double, std::vector<double>>({
        // 5 layers of 0.2mm.
        {0.2, turn_18_per_layer},
        // With a 0.4mm first layer, the first range is still 5 x 0.2mm = 1mm, so the first layer turns twice as far.
        {0.4, {36, 54, 72, 90, 108, 126, 144, 162, 180, 198, 216, 234, 252, 270, 288, 306, 324, 342, 360, 378}},
    }));
    CAPTURE(first_layer_height);
    check_rotate_template("+90/5#", expected, first_layer_height);
}

TEST_CASE("Infill rotation template B and T counts take the bottom and top shell layers", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        // The absolute 0 after each range marks its end, after 3 layers for B and 4 for T.
        {"+10B, 0", {10, 20, 30, 0, 10, 20, 30, 0, 10, 20, 30, 0, 10, 20, 30, 0, 10, 20, 30, 0}},
        {"+10T, 0", {10, 20, 30, 40, 0, 10, 20, 30, 40, 0, 10, 20, 30, 40, 0, 10, 20, 30, 40, 0}},
        // On their own, B and T hold the angle for that many layers, on every pass through the template.
        {"B, +30", {0, 0, 0, 30, 30, 30, 30, 60, 60, 60, 60, 90, 90, 90, 90, 120, 120, 120, 120, 150}},
        {"T, +30", {0, 0, 0, 0, 30, 30, 30, 30, 30, 60, 60, 60, 60, 60, 90, 90, 90, 90, 90, 120}},
        // With ! they hold it only on the first pass.
        {"B!, +30", {0, 0, 0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330, 360, 390, 420, 450, 480, 510}},
        {"T!, +30", {0, 0, 0, 0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330, 360, 390, 420, 450, 480}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Infill rotation template range that would end above the object is squeezed into the layers left", "[Fill][RotateTemplate]")
{
    // Turns 30 degrees over every 3 layers, and the full 30 over the 2 layers left at the top.
    check_rotate_template("+10B", {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 195, 210});
}

TEST_CASE("Infill rotation template star count repeats an instruction", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        {"+10*3, 0", {10, 10, 10, 0, 10, 10, 10, 0, 10, 10, 10, 0, 10, 10, 10, 0, 10, 10, 10, 0}},
        {"+30*2, -60", {30, 30, 330, 0, 0, 300, 330, 330, 270, 300, 300, 240, 270, 270, 210, 240, 240, 180, 210, 210}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Infill rotation template bare star sets the start angle without using a layer", "[Fill][RotateTemplate]")
{
    // Starts at 5 degrees, then turns 20 degrees over the whole object.
    check_rotate_template("5*, +20/100%", {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25});
}

TEST_CASE("Infill rotation template exclamation mark runs an instruction once", "[Fill][RotateTemplate]")
{
    const auto [rotate_template, expected] = GENERATE(table<std::string, std::vector<double>>({
        {"0!,+10", turn_10_per_layer_from_0},
        {"0!, +10, +20", {0, 10, 30, 40, 60, 70, 90, 100, 120, 130, 150, 160, 180, 190, 210, 220, 240, 250, 270, 280}},
    }));
    check_rotate_template(rotate_template, expected);
}

TEST_CASE("Printed infill follows the rotation template and ignores the infill direction", "[Fill][RotateTemplate]")
{
    // One template from each family that gives whole degrees, with the angles the cases above pin for it.
    const auto [rotate_template, pinned] = GENERATE(table<std::string, std::vector<double>>({
        {"+90/10", turn_9_per_layer},
        {"+45#5", step_45_every_5_layers},
        {"+90/5#", turn_18_per_layer},
        {"0!,+10", turn_10_per_layer_from_0},
    }));
    const bool sparse = GENERATE(true, false);
    CAPTURE(rotate_template, sparse);

    Print print;
    Slic3r::Test::init_and_process_print({make_cube(20, 20, 4)}, print,
                                        {{"sparse_infill_rotate_template", sparse ? rotate_template : ""},
                                         {"solid_infill_rotate_template", sparse ? "" : rotate_template},
                                         // Neither applies while a template is set.
                                         {"infill_direction", 30},
                                         {"solid_infill_direction", 30},
                                         // Without shells every layer is sparse, or solid at 100% density.
                                         {"sparse_infill_density", sparse ? "40%" : "100%"},
                                         {"sparse_infill_pattern", "rectilinear"},
                                         {"internal_solid_infill_pattern", "monotonic"},
                                         {"top_shell_layers", 0},
                                         {"bottom_shell_layers", 0},
                                         {"top_shell_thickness", 0},
                                         {"bottom_shell_thickness", 0},
                                         {"layer_height", 0.2},
                                         {"initial_layer_print_height", 0.2}});

    // As line directions, in the whole degrees angles_per_layer() gives.
    std::vector<int> expected;
    for (double angle : pinned)
        expected.push_back(int(std::lround(std::fmod(angle, 180.))));
    CHECK(angles_per_layer(print, sparse ? sparse_role : solid_role) == expected);
}

TEST_CASE("Sparse infill at 100% density turns solid and ignores the sparse rotation template", "[Fill][RotateTemplate]")
{
    struct Angles { std::vector<int> solid, sparse; };
    auto angles_for = [](const std::string &sparse_template) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(10)}, print,
                                            {{"sparse_infill_rotate_template", sparse_template},
                                             {"sparse_infill_density", "100%"},
                                             {"internal_solid_infill_pattern", "monotonic"},
                                             {"layer_height", 0.2},
                                             {"initial_layer_print_height", 0.2}});
        return Angles{angles_per_layer(print, solid_role), angles_per_layer(print, sparse_role)};
    };

    const Angles with_template    = angles_for("+45");
    const Angles without_template = angles_for("");
    REQUIRE(with_template.solid.size() == 50);
    REQUIRE(std::count(with_template.solid.begin(), with_template.solid.end(), -1) == 0);
    CHECK(std::count(with_template.sparse.begin(), with_template.sparse.end(), -1) == 50);
    CHECK(with_template.solid == without_template.solid);
}

TEST_CASE("Infill rotation template turns with the model when aligned to it", "[Fill][RotateTemplate]")
{
    auto angles_for = [](double z_rotation) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"solid_infill_rotate_template", "0,90"},
                                       {"align_infill_direction_to_model", 1},
                                       {"sparse_infill_density", "100%"},
                                       {"internal_solid_infill_pattern", "monotonic"},
                                       {"layer_height", 0.2},
                                       {"initial_layer_print_height", 0.2}});
        Print print;
        Model model;
        Slic3r::Test::init_print({Slic3r::Test::cube(10)}, print, model, config, nullptr, false);
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(z_rotation)));
        print.apply(model, config);
        print.process();
        return angles_per_layer(print, solid_role);
    };

    const std::vector<int> unrotated = angles_for(0.);
    const std::vector<int> rotated   = angles_for(30.);
    REQUIRE(unrotated.size() == 50);
    REQUIRE(rotated.size() == unrotated.size());
    REQUIRE(std::count(unrotated.begin(), unrotated.end(), -1) == 0);
    REQUIRE(std::count(rotated.begin(), rotated.end(), -1) == 0);
    for (size_t i = 0; i < unrotated.size(); ++i) {
        CAPTURE(i, unrotated[i], rotated[i]);
        CHECK(unrotated[i] == (i % 2 == 0 ? 0 : 90));
        CHECK(line_angle_delta(unrotated[i], rotated[i]) == 30);
    }
}

TEST_CASE("Ironing follows the solid infill rotation template", "[Fill]")
{
    Print print;
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(10)}, print,
                                        {{"solid_infill_rotate_template", "+45"},
                                         {"internal_solid_infill_pattern", "monotonic"},
                                         {"top_surface_pattern", "monotonic"},
                                         // Every solid surface, so the comparison covers every layer.
                                         {"ironing_type", "solid"},
                                         {"sparse_infill_density", "100%"},
                                         {"ironing_angle", 0},
                                         {"ironing_angle_fixed", 0},
                                         {"layer_height", 0.2}});

    const std::vector<int> ironing = angles_per_layer(print, ironing_role);
    const std::vector<int> solid   = angles_per_layer(print, solid_role);
    REQUIRE(ironing.size() == solid.size());

    // With no fixed angle and no offset, ironing runs along the template's angle for that layer.
    int compared = 0;
    for (size_t i = 0; i < ironing.size(); ++i)
        if (ironing[i] != -1 && solid[i] != -1) {
            CAPTURE(i, ironing[i], solid[i]);
            CHECK(ironing[i] == solid[i]);
            ++compared;
        }
    // Most of the object, not one lucky layer.
    REQUIRE(compared > int(ironing.size()) / 2);
}


namespace {

PrintRegionConfig ironing_config(IroningType type,
                                 int top_surface_filament_id = 1,
                                 int top_shell_layers        = 3,
                                 int bottom_shell_layers     = 1)
{
    PrintRegionConfig cfg;
    cfg.ironing_type.value            = type;
    cfg.top_surface_filament_id.value = top_surface_filament_id;
    cfg.top_shell_layers.value        = top_shell_layers;
    cfg.bottom_shell_layers.value     = bottom_shell_layers;
    cfg.outer_wall_filament_id.value  = 1;
    cfg.wall_loops.value              = 2;
    return cfg;
}

} // namespace

TEST_CASE("Ironing an all-solid region uses the top surface filament on every layer", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::AllSolid, /*top_surface_filament_id=*/2);
    const bool is_topmost_layer = GENERATE(false, true);
    CAPTURE(is_topmost_layer);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, is_topmost_layer) == 2);
}

TEST_CASE("Ironing top surfaces uses the top surface filament when the region has top shells", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::TopSurfaces,
                                                 /*top_surface_filament_id=*/3,
                                                 /*top_shell_layers=*/2);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, /*is_topmost_layer=*/false) == 3);
}

TEST_CASE("Ironing top surfaces without top shells needs spiral mode and more than one bottom shell", "[Fill]")
{
    const PrintRegionConfig one_bottom_shell = ironing_config(IroningType::TopSurfaces,
                                                              /*top_surface_filament_id=*/1,
                                                              /*top_shell_layers=*/0,
                                                              /*bottom_shell_layers=*/1);
    const PrintRegionConfig two_bottom_shells = ironing_config(IroningType::TopSurfaces,
                                                               /*top_surface_filament_id=*/1,
                                                               /*top_shell_layers=*/0,
                                                               /*bottom_shell_layers=*/2);

    REQUIRE(Layer::choose_ironing_extruder(two_bottom_shells, /*spiral_mode=*/true, /*is_topmost_layer=*/false) == 1);
    REQUIRE(Layer::choose_ironing_extruder(one_bottom_shell, /*spiral_mode=*/true, /*is_topmost_layer=*/false) == -1);
    REQUIRE(Layer::choose_ironing_extruder(two_bottom_shells, /*spiral_mode=*/false, /*is_topmost_layer=*/false) == -1);
}

TEST_CASE("Ironing the topmost surface only applies to the topmost layer", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::TopmostOnly, /*top_surface_filament_id=*/4);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, /*is_topmost_layer=*/true) == 4);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, /*is_topmost_layer=*/false) == -1);
}

TEST_CASE("A region with ironing turned off is never ironed", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::NoIroning);
    const bool spiral_mode      = GENERATE(false, true);
    CAPTURE(spiral_mode);
    REQUIRE(Layer::choose_ironing_extruder(cfg, spiral_mode, /*is_topmost_layer=*/true) == -1);
}

// Ironing path count and total length in mm, over the whole object.
static std::pair<size_t, double> ironing_extent(const Print &print)
{
    size_t paths  = 0;
    double length = 0.;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                if (ironing_role(entity->role())) {
                    ++paths;
                    length += unscale<double>(entity->length());
                }
    return {paths, length};
}

TEST_CASE("Ironing spacing below the minimum irons at the minimum spacing", "[Fill]")
{
    const std::string pattern      = GENERATE("rectilinear", "concentric");
    const bool        via_filament = GENERATE(false, true);
    const double      spacing      = GENERATE(0., 0.001);
    CAPTURE(pattern, via_filament, spacing);

    auto ironing_for = [&pattern, via_filament](double spacing) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"ironing_type", "top"},
                                       {"ironing_pattern", pattern},
                                       {"layer_height", 0.2}});
        // The filament override replaces the process spacing, which stays at a usable value.
        if (via_filament)
            config.set_deserialize_strict({{"ironing_spacing", 0.1}, {"filament_ironing_spacing", spacing}});
        else
            config.set_deserialize_strict({{"ironing_spacing", spacing}});
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print, config);
        return ironing_extent(print);
    };

    const std::pair<size_t, double> clamped = ironing_for(spacing);
    const std::pair<size_t, double> minimum = ironing_for(IRONING_SPACING_MIN);
    REQUIRE(minimum.first > 0);
    CHECK(clamped.first == minimum.first);
    CHECK_THAT(clamped.second, Catch::Matchers::WithinRel(minimum.second, 1e-9));
}

TEST_CASE("Concentric fill at zero spacing returns without paths", "[Fill]")
{
    std::unique_ptr<Fill> filler(Fill::new_from_type(ipConcentric));
    filler->spacing      = 0.;
    filler->bounding_box = BoundingBox(Point(0, 0), Point::new_scale(10, 10));
    FillParams params;
    params.density = 1.f;
    Surface surface(stTop, ExPolygon({Point(0, 0), Point::new_scale(10, 0), Point::new_scale(10, 10), Point::new_scale(0, 10)}));
    CHECK(filler->fill_surface(&surface, params).empty());
}

TEST_CASE("Solid infill direction offsets every layer when no template is set", "[Fill]")
{
    auto angles_for = [](int direction) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(10)}, print,
                                            {{"solid_infill_direction", direction},
                                             {"sparse_infill_density", "100%"},
                                             {"internal_solid_infill_pattern", "monotonic"},
                                             {"layer_height", 0.2}});
        return angles_per_layer(print, solid_role);
    };

    const std::vector<int> at_0  = angles_for(0);
    const std::vector<int> at_30 = angles_for(30);
    REQUIRE(at_0.size() == at_30.size());
    REQUIRE(std::count(at_0.begin(), at_0.end(), -1) == 0);
    REQUIRE(std::count(at_30.begin(), at_30.end(), -1) == 0);

    for (size_t i = 0; i < at_0.size(); ++i) {
        const int delta = line_angle_delta(at_0[i], at_30[i]);
        CAPTURE(i, at_0[i], at_30[i]);
        CHECK(delta == 30);
    }
}

// Orca: the spiral inset pattern chains the concentric loops into a single continuous path per
// island, so it has to cope with the degenerate loops offsetting leaves behind and it must not join
// loops that only look adjacent.
namespace {

Slic3r::Polylines spiral_inset_fill(const Slic3r::ExPolygon &surface_shape, double spacing)
{
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("spiralinset"));
    filler->spacing = spacing;
    // Cancel the half-spacing contraction fill_surface() applies, so the filler sees the shape as given.
    filler->overlap = 0.5 * spacing;

    Slic3r::FillParams fill_params;
    fill_params.density     = 1.f;
    fill_params.dont_adjust = true;

    Slic3r::Surface surface(Slic3r::stBottom, surface_shape);
    return filler->fill_surface(&surface, fill_params);
}

Slic3r::ExPolygon rectangle(double x, double y, double w, double h)
{
    return Slic3r::ExPolygon({Slic3r::Point::new_scale(x, y), Slic3r::Point::new_scale(x + w, y),
                              Slic3r::Point::new_scale(x + w, y + h), Slic3r::Point::new_scale(x, y + h)});
}

// Area of the surface the toolpaths fail to cover, and the largest single patch of it, in mm2. Each
// bead is measured at its own width so the variable width walls are not sold short.
std::pair<double, double> uncovered_area(const Slic3r::ExPolygon &surface_shape, const Slic3r::Polygons &covered)
{
    double total = 0, biggest = 0;
    for (const Slic3r::ExPolygon &gap : Slic3r::diff_ex(Slic3r::ExPolygons{surface_shape}, Slic3r::union_(covered))) {
        const double area = unscale<double>(unscale<double>(gap.area()));
        total += area;
        biggest = std::max(biggest, area);
    }
    return {total, biggest};
}

Slic3r::Polygons beads_of(const Slic3r::Polylines &paths, double width)
{
    return Slic3r::offset(paths, float(scale_(0.5 * width)));
}

Slic3r::Polygons beads_of(const Slic3r::ThickPolylines &paths)
{
    Slic3r::Polygons covered;
    for (const Slic3r::ThickPolyline &path : paths)
        for (size_t i = 0; i + 1 < path.points.size(); ++i) {
            Slic3r::Polyline segment;
            segment.points = {path.points[i], path.points[i + 1]};
            Slic3r::append(covered, Slic3r::offset(Slic3r::Polylines{segment},
                                                  float(0.5 * std::max(path.width[2 * i], path.width[2 * i + 1]))));
        }
    return covered;
}

} // namespace

TEST_CASE("Spiral inset fill drops loops shorter than the end clipping", "[Fill][Regression]")
{
    // A sliver whose whole perimeter is shorter than the length clipped off the end of a loop, so the
    // clipping consumes the path entirely. Such a loop carries no extrusion and must be dropped
    // rather than kept as an empty path and read back from.
    const double spacing = 0.45;

    Slic3r::Polylines paths;
    REQUIRE_NOTHROW(paths = spiral_inset_fill(rectangle(0, 0, 0.05, 0.05), spacing));
    for (const Slic3r::Polyline &path : paths)
        CHECK(path.size() >= 2);

    // The same surface at a size the clipping cannot swallow still gets filled.
    REQUIRE_NOTHROW(paths = spiral_inset_fill(rectangle(0, 0, 5, 5), spacing));
    REQUIRE(paths.size() == 1);
    CHECK(paths.front().size() >= 2);
}

TEST_CASE("Spiral inset fill keeps separate islands on separate paths", "[Fill]")
{
    // Two lobes joined by a neck narrower than the loop spacing: the inward offsets break the surface
    // into two islands, which cannot share one spiral, and no path may leave the surface.
    const double spacing = 0.45;
    Slic3r::ExPolygon dumbbell = rectangle(0, 0, 6, 6);
    dumbbell = Slic3r::union_ex(Slic3r::ExPolygons{dumbbell, rectangle(6, 2.9, 4, 0.2), rectangle(10, 0, 6, 6)}).front();

    const Slic3r::Polylines paths = spiral_inset_fill(dumbbell, spacing);
    REQUIRE(paths.size() >= 2);

    // Inflate by a hair so that loops sitting exactly on the outline still count as contained.
    const Slic3r::ExPolygons within = Slic3r::offset_ex(dumbbell, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);
    for (const Slic3r::Polyline &path : paths) {
        CHECK(path.size() >= 2);
        CHECK(within.front().contains(path));
    }
}


TEST_CASE("Spiral inset fill stays connected across sharp corners", "[Fill][Regression]")
{
    // At a corner of half-angle a, the next ring inward retreats along the bisector by spacing/sin(a),
    // which leaves it several spacings from the end of the ring it continues. Judging the break by
    // distance broke the spiral into loose rings at every spike; nesting is what decides the island.
    const double spacing = 0.45;
    const Slic3r::ExPolygon spike({Slic3r::Point::new_scale(0, 0), Slic3r::Point::new_scale(30, 0),
                                   Slic3r::Point::new_scale(15, 4)});

    const Slic3r::Polylines paths = spiral_inset_fill(spike, spacing);
    CHECK(paths.size() == 1);

    const Slic3r::ExPolygons within = Slic3r::offset_ex(spike, float(SCALED_EPSILON));
    REQUIRE(within.size() == 1);
    for (const Slic3r::Polyline &path : paths)
        CHECK(within.front().contains(path));
}

TEST_CASE("Spiral inset fill starts on a convex corner", "[Fill][Regression]")
{
    // The only right angle on this outline is the reflex one: the two edges meeting at the origin
    // span 90 degrees exactly as a square corner would, but the material lies outside them. The next
    // ring in steps away from a reflex corner along the bisector instead of hugging it, so starting
    // the spiral there sent it across a long diagonal on every single ring.
    const double spacing = 0.45;
    const Slic3r::ExPolygon notched({Slic3r::Point::new_scale(0, 0), Slic3r::Point::new_scale(0, 10),
                                     Slic3r::Point::new_scale(-16, 18), Slic3r::Point::new_scale(-16, -2),
                                     Slic3r::Point::new_scale(-8, -16), Slic3r::Point::new_scale(18, -16),
                                     Slic3r::Point::new_scale(10, 0)});

    const Slic3r::Polylines paths = spiral_inset_fill(notched, spacing);
    REQUIRE(paths.size() >= 1);

    // Every edge of the outline is at least 45 degrees off the bisector of that reflex corner, and
    // so is every ring offset from it. A long segment running along the bisector can therefore only
    // be the spiral striking out across the rings to reach the next one.
    for (const Slic3r::Polyline &path : paths)
        for (const Slic3r::Line &segment : path.lines()) {
            const Vec2d  v         = (segment.b - segment.a).cast<double>();
            const double direction = std::fmod(std::atan2(v.y(), v.x()) * 180.0 / M_PI + 180.0, 180.0);
            if (std::abs(direction - 45.0) > 25.0)
                continue;
            CAPTURE(direction, unscale<double>(segment.length()));
            CHECK(segment.length() <= scale_(1.5 * spacing));
        }
}

TEST_CASE("Spiral inset fill closes the gaps with variable width walls", "[Fill]")
{
    // Fixed width loops cannot fill a region that is not a whole number of lines across and leave the
    // remainder open, which on a ring shows up as a wedge several lines wide. Plain concentric avoids
    // that by building solid surfaces out of Arachne's variable width walls, and so must this pattern.
    const double spacing = 0.45;
    Slic3r::ExPolygon ring = rectangle(0, 0, 24, 24);
    Slic3r::Polygon   hole;
    for (int i = 0; i < 64; ++i) {
        const double angle = -2.0 * PI * i / 64.0; // clockwise, so it reads as a hole
        hole.points.emplace_back(Slic3r::Point::new_scale(12 + 7.3 * std::cos(angle), 12 + 7.3 * std::sin(angle)));
    }
    ring.holes.emplace_back(hole);

    Slic3r::PrintConfig       print_config;
    Slic3r::PrintObjectConfig object_config;
    auto make_filler = [&]() {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("spiralinset"));
        filler->spacing             = spacing;
        filler->overlap             = 0.5 * spacing; // cancel the contraction, so both see the same surface
        filler->print_config        = &print_config;
        filler->print_object_config = &object_config;
        return filler;
    };

    Slic3r::FillParams params;
    params.density      = 1.f;
    params.dont_adjust  = false;
    params.layer_height = 0.2;

    const Slic3r::Surface surface(Slic3r::stTop, ring);

    std::unique_ptr<Slic3r::Fill> fixed = make_filler();
    const Slic3r::Polylines fixed_width = fixed->fill_surface(&surface, params);
    REQUIRE(!fixed_width.empty());
    const auto fixed_gaps = uncovered_area(ring, beads_of(fixed_width, fixed->spacing));

    params.use_arachne = true;
    std::unique_ptr<Slic3r::Fill> variable = make_filler();
    const Slic3r::ThickPolylines variable_width = variable->fill_surface_arachne(&surface, params);
    REQUIRE(!variable_width.empty());
    const auto variable_gaps = uncovered_area(ring, beads_of(variable_width));

    CAPTURE(fixed_gaps.first, fixed_gaps.second, variable_gaps.first, variable_gaps.second);
    // The wedges the fixed width loops leave behind are what the variable width walls take up.
    CHECK(variable_gaps.second < 0.5 * fixed_gaps.second);
    CHECK(variable_gaps.first < fixed_gaps.first);

    // And it is still a spiral: far fewer paths than the ring has loops.
    // And the walls are still chained into spirals rather than printed one path per wall. The ring is
    // at its narrowest (12 - 7.3) mm across and is filled from both sides, so it is at least this many
    // walls thick there and thicker elsewhere. Arachne's short thin feature walls cannot join a spiral,
    // so only the substantial paths count towards this.
    const size_t walls_across = size_t(2.0 * (12.0 - 7.3) / spacing);
    size_t       spirals      = 0;
    for (const Slic3r::ThickPolyline &path : variable_width)
        if (path.length() > scale_(10.0 * spacing))
            ++spirals;
    CAPTURE(spirals, walls_across, variable_width.size(), fixed_width.size());
    CHECK(2 * spirals < walls_across);
}

TEST_CASE("Spiral inset fill prints the open walls at the centre end of the spiral", "[Fill][Regression]")
{
    // Arachne leaves the middle of this ellipse as an open wall. Printed after an outward spiral, which
    // ends at the edge, it sent the nozzle back across the surface to the centre.
    const double    spacing = 0.45;
    Slic3r::Polygon ellipse;
    for (int i = 0; i < 90; ++i) {
        const double angle = 2.0 * PI * i / 90.0;
        ellipse.points.emplace_back(Slic3r::Point::new_scale(10 * std::cos(angle), 5 * std::sin(angle)));
    }
    const Slic3r::SurfaceFillOrder order = GENERATE(Slic3r::SurfaceFillOrder::Default, Slic3r::SurfaceFillOrder::Inward);
    CAPTURE(int(order));

    Slic3r::PrintConfig       print_config;
    Slic3r::PrintObjectConfig object_config;
    const Slic3r::Surface     surface(Slic3r::stTop, Slic3r::ExPolygon(ellipse));
    auto fill = [&](Slic3r::ExtrusionRole role) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("spiralinset"));
        filler->spacing             = spacing;
        filler->overlap             = 0.5 * spacing;
        filler->print_config        = &print_config;
        filler->print_object_config = &object_config;

        Slic3r::FillParams params;
        params.density        = 1.f;
        params.dont_adjust    = false;
        params.layer_height   = 0.2;
        params.use_arachne    = true;
        params.extrusion_role = role;
        params.fill_order     = order;
        return filler->fill_surface_arachne(&surface, params);
    };

    const Slic3r::ThickPolylines paths = fill(Slic3r::erTopSolidInfill);
    REQUIRE(paths.size() > 1);
    // The spiral goes last when it runs outwards and first when it runs inwards.
    const auto spiral = std::max_element(paths.begin(), paths.end(), [](const Slic3r::ThickPolyline &a, const Slic3r::ThickPolyline &b) {
        return a.length() < b.length();
    });
    CHECK(spiral == (order == Slic3r::SurfaceFillOrder::Inward ? paths.begin() : paths.end() - 1));

    // Internal solid infill gets the same walls rather than fixed width loops, which leave gaps.
    const Slic3r::ThickPolylines solid = fill(Slic3r::erSolidInfill);
    REQUIRE(solid.size() == paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        CHECK(solid[i].points == paths[i].points);
        CHECK(solid[i].width == paths[i].width);
    }
}

TEST_CASE("Honeycomb infill rounds its cell corners with the smooth factor", "[Fill]")
{
    // A cell whose sides are several times the line width, so that the corners have room to be rounded.
    const double spacing = 0.45;
    const double density = 0.1;
    auto         fill    = [spacing, density](double smooth_factor) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("honeycomb"));
        filler->spacing = spacing;

        FillParams params;
        params.density = float(density);
        params.dont_adjust = true;
        // Keep the fragments apart, so that only the turns of the pattern itself are measured.
        params.anchor_length_max = 0.f;
        params.smooth_factor     = smooth_factor;

        Slic3r::ExPolygon square{ Slic3r::Points{
            Point::new_scale(0., 0.), Point::new_scale(50., 0.), Point::new_scale(50., 50.), Point::new_scale(0., 50.) } };
        Slic3r::Surface surface(stInternal, square);
        return filler->fill_surface(&surface, params);
    };

    // Cosine of the sharpest turn of any of the paths, 1 meaning none of them turns at all.
    auto sharpest_turn_cosine = [](const Slic3r::Polylines &polylines) {
        double sharpest = 1.;
        for (const Polyline &polyline : polylines)
            for (size_t i = 1; i + 1 < polyline.size(); ++i) {
                const Vec2d incoming = (polyline[i] - polyline[i - 1]).cast<double>().normalized();
                const Vec2d outgoing = (polyline[i + 1] - polyline[i]).cast<double>().normalized();
                sharpest = std::min(sharpest, incoming.dot(outgoing));
            }
        return sharpest;
    };
    auto point_count = [](const Slic3r::Polylines &polylines) {
        return std::accumulate(polylines.begin(), polylines.end(), size_t(0),
                               [](size_t count, const Polyline &polyline) { return count + polyline.size(); });
    };

    const Slic3r::Polylines sharp  = fill(0.);
    const Slic3r::Polylines smooth = fill(1.);

    REQUIRE(!sharp.empty());
    REQUIRE(smooth.size() == sharp.size());
    REQUIRE(point_count(smooth) > point_count(sharp));
    // The cell corners turn by 60 degrees; smoothing replaces them by gentle curves.
    REQUIRE(sharpest_turn_cosine(sharp) < 0.6);
    REQUIRE(sharpest_turn_cosine(smooth) > 0.9);
}

// Point count, number of turns sharper than 25 degrees and length of the sparse infill of a print.
// A rounded corner is a run of much gentler turns, so smoothing shows up as fewer sharp ones.
struct SparseInfillShape {
    size_t point_count { 0 };
    size_t sharp_turns { 0 };
    size_t path_count { 0 };
    double length { 0. };
    // Digest of every point in the order it is printed. The counts above all survive the same
    // extrusions being joined into different polylines, so only this tells two such fills apart.
    uint64_t sequence { 14695981039346656037ull };
};

static SparseInfillShape sparse_infill_shape(const Print &print)
{
    SparseInfillShape shape;

    auto account = [&shape](const ExtrusionPath &path) {
        if (!sparse_role(path.role()))
            return;
        const Points3 &pts = path.polyline.points;
        ++shape.path_count;
        shape.point_count += pts.size();
        for (const auto &pt : pts)
            for (const coord_t coordinate : {pt.x(), pt.y(), pt.z()})
                shape.sequence = (shape.sequence ^ uint64_t(coordinate)) * 1099511628211ull;
        for (size_t i = 1; i < pts.size(); ++i)
            shape.length += (pts[i] - pts[i - 1]).head<2>().cast<double>().norm();
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const Vec2d incoming = (pts[i] - pts[i - 1]).head<2>().cast<double>();
            const Vec2d outgoing = (pts[i + 1] - pts[i]).head<2>().cast<double>();
            if (incoming.squaredNorm() > 0. && outgoing.squaredNorm() > 0. &&
                incoming.normalized().dot(outgoing.normalized()) < 0.9)
                ++shape.sharp_turns;
        }
    };

    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
                if (auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                    account(*path);
                else if (auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
                    for (const ExtrusionPath &p : multi->paths)
                        account(p);
                else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                    for (const ExtrusionPath &p : loop->paths)
                        account(p);
            }
    return shape;
}

TEST_CASE("Lightning infill slices the same model the same way twice", "[Fill][Regression]")
{
    // Slicing twice in one process catches a generator that carries state from one slice to the
    // next, or whose result depends on how the parallel layer fill interleaves.
    auto shape = [] {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "lightning"},
                                             {"sparse_infill_density", "50%"},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape first  = shape();
    const SparseInfillShape second = shape();

    REQUIRE(first.path_count > 0);
    REQUIRE(second.path_count == first.path_count);
    REQUIRE(second.point_count == first.point_count);
    REQUIRE(second.sharp_turns == first.sharp_turns);
    // No tolerance: the same extrusions in the same order add up to the very same number.
    REQUIRE_THAT(second.length, Catch::Matchers::WithinAbs(first.length, 0.));
    // All of the above agree when the same branches are joined into different polylines, so the
    // point sequence is what actually decides whether the two slices produced the same infill.
    REQUIRE(second.sequence == first.sequence);
}

TEST_CASE("Lightning infill rounds the turns of its branches with the smooth factor", "[Fill]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "lightning"},
                                             {"sparse_infill_density", "15%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    // The branch turns are replaced by curves, which cut the corners off and take more points to
    // describe. The turns where two branches are joined into one path stay sharp.
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Concentric infill rounds its loops with the smooth factor", "[Fill]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "concentric"},
                                             {"sparse_infill_density", "20%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Cross hatch infill rounds its transition layers with the smooth factor", "[Fill]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "crosshatch"},
                                             {"sparse_infill_density", "20%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Trapezoidal infill rounds its corners only with more than one line", "[Fill]")
{
    const std::string pattern = GENERATE("grid", "quartercubic");
    CAPTURE(pattern);
    auto shape_for = [&pattern](int multiline, const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", pattern},
                                             {"sparse_infill_density", "20%"},
                                             {"fill_multiline", multiline},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for(2, "0%");
    const SparseInfillShape smooth = shape_for(2, "100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);

    // A single line per infill wall is the plain crossing lines, which have no corner of their own.
    const SparseInfillShape single_sharp  = shape_for(1, "0%");
    const SparseInfillShape single_smooth = shape_for(1, "100%");
    REQUIRE(single_sharp.point_count > 0);
    REQUIRE(single_smooth.point_count == single_sharp.point_count);
    REQUIRE(single_smooth.length == single_sharp.length);
}

TEST_CASE("Multiline infill of an object matches the infill of a larger object with the same center", "[Fill]")
{
    const InfillPattern pattern   = GENERATE(ipGrid, ipTriangles, ipStars, ipCubic, ipQuarterCubic);
    const int           multiline = GENERATE(2, 3);
    // A square with cells as large as itself, whose corners are as far out as the object bounding box
    // reaches, and a strip with small cells, whose extents change with every layer orientation.
    const auto [half, density] = GENERATE(table<Vec2d, float>({ { Vec2d(20., 20.), 0.15f }, { Vec2d(60., 4.), 0.35f } }));
    CAPTURE(pattern, multiline, half.x(), half.y(), density);

    // Off the origin; the same center gives both objects the same pattern.
    auto rectangle = [](const Vec2d &half) {
        const Vec2d center(100., 60.);
        return ExPolygon{ Points{ Point::new_scale(center.x() - half.x(), center.y() - half.y()),
                                  Point::new_scale(center.x() + half.x(), center.y() - half.y()),
                                  Point::new_scale(center.x() + half.x(), center.y() + half.y()),
                                  Point::new_scale(center.x() - half.x(), center.y() + half.y()) } };
    };
    const ExPolygon object = rectangle(half);
    const ExPolygon larger = rectangle(half + Vec2d(10., 10.));
    auto fill = [pattern, multiline, density = density](const ExPolygon &region, size_t layer_id) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
        filler->spacing     = 0.45;
        filler->angle       = float(M_PI / 7.);
        filler->fixed_angle = true;
        filler->layer_id    = layer_id;
        filler->z           = 0.2 * double(layer_id + 1);
        filler->set_bounding_box(get_extents(region.contour));

        FillParams params;
        params.density           = density;
        params.multiline         = multiline;
        params.dont_adjust       = true;
        Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary of the object, where both are clipped and connected the same way.
    const Polygons inner = shrink(to_polygons(object), scale_(1.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };

    // Both layer orientations of Grid, all three of the triangular family.
    for (size_t layer_id = 0; layer_id < 3; ++layer_id) {
        CAPTURE(layer_id);
        const Polylines walls = fill(object, layer_id);
        REQUIRE_FALSE(walls.empty());
        CHECK(get_intersections(to_lines(walls)).empty());
        const Polylines reference = fill(larger, layer_id);
        CHECK(farthest(reference, walls) < 0.01);
        CHECK(farthest(walls, reference) < 0.01);
    }
}

TEST_CASE("Gyroid infill of an object matches the infill of a larger object with the same center", "[Fill]")
{
    const bool   optimized = GENERATE(false, true);
    const int    multiline = GENERATE(1, 2);
    const float  density   = GENERATE(0.05f, 0.2f);
    const double spacing   = 0.45;
    CAPTURE(optimized, multiline, density);

    auto circle = [](double radius) {
        Polygon contour = make_circle_num_segments(scale_(radius), 120);
        contour.translate(Point::new_scale(100., 60.));
        return ExPolygon(std::move(contour));
    };
    const ExPolygon object = circle(20.);
    const ExPolygon larger = circle(30.);
    auto fill = [optimized, multiline, density, spacing](const ExPolygon &region, double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipGyroid));
        filler->spacing = spacing;
        filler->angle   = float(M_PI / 7.);
        filler->z       = z;

        FillParams params;
        params.density          = density;
        params.multiline        = multiline;
        params.gyroid_optimized = optimized;
        params.dont_adjust      = true;
        Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary of the object, where both are clipped and connected the same way.
    const Polygons inner = shrink(to_polygons(object), scale_(1.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };

    // Marching squares simplifies rings that start elsewhere in each object.
    const double tolerance = optimized ? SPARSE_INFILL_RESOLUTION + 0.01 : 0.01;
    // Half a z period of the waves, through both switches between horizontal and vertical waves.
    const double wave_distance = spacing * multiline / (density * FillGyroid::DensityAdjust);
    for (int step = 0; step <= 8; ++step) {
        const double z = wave_distance * M_PI * step / 8.;
        CAPTURE(z);
        const Polylines paths = fill(object, z);
        REQUIRE_FALSE(paths.empty());
        const Polylines reference = fill(larger, z);
        CHECK(farthest(reference, paths) < tolerance);
        CHECK(farthest(paths, reference) < tolerance);
    }
}

TEST_CASE("Hilbert curve sparse infill lines up across layers of different sizes", "[Fill]")
{
    // A 40mm square up to 5mm, with a 12mm square off its center on top of it up to 10mm, so the
    // two islands neither share a corner nor need the same size of curve.
    TriangleMesh step = make_cube(40, 40, 5);
    TriangleMesh top  = make_cube(12, 12, 5);
    top.translate(4, 4, 5);
    step.merge(top);
    Print print;
    Slic3r::Test::init_and_process_print({std::move(step)}, print,
                                        {{"sparse_infill_pattern", "hilbertcurve"},
                                         {"sparse_infill_density", "25%"},
                                         {"top_shell_layers", 0},
                                         {"bottom_shell_layers", 0},
                                         {"top_shell_thickness", 0},
                                         {"bottom_shell_thickness", 0},
                                         {"layer_height", 0.2},
                                         {"initial_layer_print_height", 0.2},
                                         {"resolution", 0.012}});

    const PrintObject &object = *print.objects().front();
    // 10mm at 0.2mm.
    REQUIRE(object.layers().size() == 50);
    const Layer    &lower = *object.get_layer(12); // 2.4mm to 2.6mm
    const Layer    &upper = *object.get_layer(37); // 7.4mm to 7.6mm
    const Polylines below = layer_paths(lower, erInternalInfill);
    REQUIRE_FALSE(below.empty());
    const AABBTreeLines::LinesDistancer<Line> below_tree(to_lines(below));

    // Away from the perimeters, where the curve is cut and joined differently on each layer.
    const Polylines above = intersection_pl(layer_paths(upper, erInternalInfill), shrink(to_polygons(upper.lslices), scale_(2.)));
    REQUIRE_FALSE(above.empty());
    double max_distance = 0.;
    for (const Polyline &path : above)
        for (const Point &point : path.equally_spaced_points(scale_(0.25)))
            max_distance = std::max(max_distance, below_tree.distance_from_lines<false>(point));
    CHECK(unscale<double>(max_distance) <= 0.012);
}

TEST_CASE("Multiline infill keeps its lines parallel around a corner", "[Fill]")
{
    // A left turn by 135 degrees, drawn as two lines half of the 0.4 mm spacing to each side of it.
    const double turn = 0.75 * PI;
    Polylines paths{ Polyline{ Point::new_scale(0., 0.), Point::new_scale(10., 0.),
                               Point::new_scale(10. + 10. * std::cos(turn), 10. * std::sin(turn)) } };
    FillParams params;
    params.multiline = 2;
    multiline_fill(paths, params, 0.4f);
    REQUIRE(paths.size() == 1);

    // The outer line turns at the miter point, where the lines 0.2 mm out from both legs meet.
    const Point miter = Point::new_scale(10. + 0.2 * std::tan(0.5 * turn), -0.2);
    double nearest = std::numeric_limits<double>::max();
    for (const Point &point : paths.front().points)
        nearest = std::min(nearest, (point - miter).cast<double>().norm());
    REQUIRE(nearest < scaled<double>(0.001));
    // The round ends are flattened to a fraction of the offset rather than to the integer grid.
    REQUIRE(paths.front().size() < 100);
}

TEST_CASE("Multiline cubic infill follows the cubic lines without crossing itself", "[Fill]")
{
    const std::string pattern = GENERATE("cubic", "quartercubic");
    const int    multiline = GENERATE(2, 3);
    const double spacing   = 0.45;
    const double density   = GENERATE(0.1, 0.3, 0.6);
    const double wall      = multiline * spacing;
    CAPTURE(pattern, multiline, density);

    const ExPolygon region{ Slic3r::Points{ Point::new_scale(0., 0.), Point::new_scale(40., 0.),
                                            Point::new_scale(40., 40.), Point::new_scale(0., 40.) } };
    auto fill = [&region, &pattern, spacing](int lines, double density, size_t layer_id, double z) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type(pattern));
        filler->spacing  = spacing;
        filler->angle    = float(M_PI / 7.);
        filler->layer_id = layer_id;
        filler->z        = z;

        FillParams params;
        params.density           = float(density);
        params.multiline         = lines;
        params.dont_adjust       = true;
        params.anchor_length_max = 0.f; // The bare pattern, without connections along the boundary.
        Slic3r::Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary, where a line is clipped earlier than the side of its wall.
    const Polygons inner = shrink(to_polygons(region), scale_(3.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };

    // The closest two different paths come, up to a line spacing.
    auto closest_paths = [&inner, spacing](const Polylines &paths) {
        Lines               lines;
        std::vector<size_t> owner;
        for (size_t i = 0; i < paths.size(); ++i)
            for (const Line &line : paths[i].lines()) {
                lines.emplace_back(line);
                owner.emplace_back(i);
            }
        const AABBTreeLines::LinesDistancer<Line> tree(lines);
        double closest = spacing;
        for (size_t i = 0; i < paths.size(); ++i)
            for (const Polyline &piece : intersection_pl(paths[i], inner))
                for (const Point &point : piece.equally_spaced_points(scale_(0.1)))
                    for (size_t k : tree.all_lines_in_radius(point, scale_(spacing)))
                        if (owner[k] != i)
                            closest = std::min(closest, unscale<double>(lines[k].distance_to(point)));
        return closest;
    };

    // One z period of the pattern: sqrt(2) / 3 of the 3 * wall / density line spacing of Cubic, sqrt(2) of the
    // 4 * wall / density line spacing of Quarter Cubic.
    const double z_period = std::sqrt(2.) * wall / density * (pattern == "cubic" ? 1. : 4.);
    const size_t layers   = 30;
    for (size_t layer_id = 0; layer_id < layers; ++layer_id) {
        const double z = z_period * (layer_id + 0.5) / layers;
        CAPTURE(layer_id, z);
        const Polylines walls = fill(multiline, density, layer_id, z);
        REQUIRE_FALSE(walls.empty());
        CHECK(get_intersections(to_lines(walls)).empty());
        // Long paths running out to the boundary, not loops around the cells.
        CHECK(std::none_of(walls.begin(), walls.end(), [](const Polyline &path) { return path.first_point() == path.last_point(); }));
        // Neighbouring lines stay a line spacing apart, also where two walls touch. Cubic comes a little closer when dense.
        CHECK(closest_paths(walls) > (pattern == "cubic" ? 0.9 : 0.99) * spacing);

        // Single lines at the same spacing: the walls are drawn along them.
        const Polylines lines = fill(1, density / multiline, layer_id, z);
        REQUIRE_FALSE(lines.empty());
        CHECK(farthest(lines, walls) < 0.5 * wall);
        CHECK(farthest(walls, lines) < 1.5 * wall);
    }
}

TEST_CASE("Multiline quarter cubic infill keeps parallel lines together like the single line pattern", "[Fill]")
{
    const int    multiline = GENERATE(2, 3);
    const size_t layer_id  = GENERATE(0, 1);
    const double spacing   = 0.45;
    const double density   = 0.2;
    const double wall      = multiline * spacing;
    const double period    = 4. * wall / density;
    // The z where a pair of parallel lines is at its closest, with the middle of the pair and of the lines running along it.
    // The second pair of lines is 0.7071 of a period behind the first in z / sqrt(2).
    const auto [phase, along_x, across, along] = GENERATE_COPY(table<double, bool, double, double>({ { 0., true, 0., 0.5 * period },
                                                                                                     { 0.5, true, 0.5 * period, 0.5 * period },
                                                                                                     { 1. - std::sqrt(0.5), false, 0., 0.5 * period },
                                                                                                     { 1.5 - std::sqrt(0.5), false, 0.5 * period, 0.5 * period } }));
    CAPTURE(multiline, layer_id, phase);

    const ExPolygon region{ Slic3r::Points{ Point::new_scale(-60., -60.), Point::new_scale(60., -60.),
                                            Point::new_scale(60., 60.), Point::new_scale(-60., 60.) } };
    std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type(ipQuarterCubic));
    filler->spacing  = spacing;
    filler->angle    = float(-M_PI / 2.); // The lines run along x and y.
    filler->layer_id = layer_id;
    filler->z        = std::sqrt(2.) * (phase + 0.001) * period;

    FillParams params;
    params.pattern           = ipQuarterCubic;
    params.density           = float(density);
    params.multiline         = multiline;
    params.dont_adjust       = true;
    params.anchor_length_max = 0.f; // The bare pattern, without connections along the boundary.
    Slic3r::Surface surface(stInternal, region);
    const Polylines paths = filler->fill_surface(&surface, params);
    REQUIRE_FALSE(paths.empty());

    // Across the pair, away from any crossing: the lines of both walls, a line spacing apart with no gap in the middle.
    auto probe_point = [along_x = along_x, along = along](double position) {
        return along_x ? Point::new_scale(position, along) : Point::new_scale(along, position);
    };
    const Line probe(probe_point(across - 2. * wall), probe_point(across + 2. * wall));
    std::vector<double> positions;
    for (const Polyline &path : paths)
        for (const Line &line : path.lines())
            if (Point hit; probe.intersection(line, &hit))
                positions.emplace_back(unscale<double>(along_x ? hit.x() : hit.y()));
    std::sort(positions.begin(), positions.end());
    REQUIRE(positions.size() == size_t(2 * multiline));
    for (size_t i = 1; i < positions.size(); ++i)
        CHECK_THAT(positions[i] - positions[i - 1], Catch::Matchers::WithinAbs(spacing, 0.01));
}

TEST_CASE("Multiline adaptive cubic infill keeps its lines apart without closing them around the cells", "[Fill]")
{
    const std::string pattern   = GENERATE("adaptivecubic", "supportcubic");
    const int         multiline = GENERATE(2, 3);
    CAPTURE(pattern, multiline);

    // A sphere refines the octree all around, so the finer lines end on the coarser ones at every layer.
    TriangleMesh sphere = Slic3r::Test::mesh(Slic3r::Test::TestMesh::sphere_50mm);
    sphere.scale(0.3f);
    Print print;
    Slic3r::Test::init_and_process_print({sphere}, print,
                                        {{"sparse_infill_pattern", pattern},
                                         {"sparse_infill_density", "40%"},
                                         {"fill_multiline", multiline},
                                         {"infill_anchor", 0},
                                         {"infill_anchor_max", 0},
                                         {"layer_height", 0.3}});

    size_t paths = 0, loops = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        Polylines printed;
        Polygons  sparse;
        double    spacing = 0.;
        for (const LayerRegion *region : layer->regions()) {
            for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                if (entity->role() == erInternalInfill)
                    entity->collect_polylines(printed);
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal)
                    append(sparse, shrink(to_polygons(surface.expolygon), scale_(1.)));
            spacing = region->flow(frInfill).spacing();
        }
        if (printed.empty())
            continue;
        CAPTURE(layer->print_z);
        paths += printed.size();
        loops += std::count_if(printed.begin(), printed.end(), [](const Polyline &pl) { return pl.first_point() == pl.last_point(); });
        CHECK(get_intersections(to_lines(printed)).empty());

        // Neighbouring lines stay a line spacing apart, less the overlap of a line end with the wall it stops on.
        // Pieces of one line that meet end to end are one line.
        std::vector<size_t> line_of(printed.size());
        std::iota(line_of.begin(), line_of.end(), 0);
        std::function<size_t(size_t)> find = [&](size_t i) { return line_of[i] == i ? i : line_of[i] = find(line_of[i]); };
        for (size_t i = 0; i < printed.size(); ++i)
            for (size_t j = i + 1; j < printed.size(); ++j)
                for (const Point &a : { printed[i].first_point(), printed[i].last_point() })
                    for (const Point &b : { printed[j].first_point(), printed[j].last_point() })
                        if ((a - b).cast<double>().norm() < SCALED_EPSILON)
                            line_of[find(i)] = find(j);
        Lines               lines;
        std::vector<size_t> owner;
        for (size_t i = 0; i < printed.size(); ++i)
            for (const Line &line : printed[i].lines()) {
                lines.push_back(line);
                owner.push_back(find(i));
            }
        AABBTreeLines::LinesDistancer<Line> tree(lines);
        double closest = spacing;
        for (size_t i = 0; i < printed.size(); ++i)
            for (const Point &p : printed[i].equally_spaced_points(scale_(0.1)))
                if (contains(sparse, p))
                    for (size_t k : tree.all_lines_in_radius(p, scale_(spacing)))
                        if (owner[k] != find(i))
                            closest = std::min(closest, unscale<double>(lines[k].distance_to(p)));
        CHECK(closest > 0.45 * spacing);
    }
    REQUIRE(paths > 0);
    // The lines run on through the cells instead of each cell getting its own loops.
    CHECK(loops < paths / 4);
}

TEST_CASE("Multiline adaptive cubic paths touch where they bounce off each other", "[Fill]")
{
    const int    sweep = GENERATE(0, 1, 2);
    // Offset of the third family in walls, so the three meet in points or in small triangles.
    const double shift = GENERATE(0., 0.1, 0.5, 1., 2.5, -0.5, -1.);
    // Like finer octree lines ending on coarser ones, the 60 degree lines may start on the horizontal line through 0.
    const bool   starting = GENERATE(false, true);
    CAPTURE(sweep, shift, starting);

    const double d1 = scale_(0.8), pitch = scale_(8.), inner = scale_(12.);
    Lines        lines;
    for (int k = 0; k < 3; ++k) {
        const Vec2d dir(std::cos(k * M_PI / 3.), std::sin(k * M_PI / 3.)), normal(-dir.y(), dir.x());
        for (int i = -6; i <= 6; ++i) {
            const Vec2d  mid   = (i * pitch + (k == 2 ? shift * d1 : 0.)) * normal;
            const double start = k == 1 && starting ? -mid.y() / dir.y() : -10. * pitch;
            lines.emplace_back((mid + start * dir).cast<coord_t>(), (mid + 10. * pitch * dir).cast<coord_t>());
        }
    }
    const Polylines paths = FillAdaptive::multiline_paths(lines, d1, 0., sweep, BoundingBox(Point::new_scale(-20., -20.), Point::new_scale(20., 20.)));
    REQUIRE_FALSE(paths.empty());
    CHECK(get_intersections(to_lines(paths)).empty());

    Lines               pieces;
    std::vector<size_t> owner;
    for (size_t i = 0; i < paths.size(); ++i)
        for (const Line &line : paths[i].lines()) {
            pieces.push_back(line);
            owner.push_back(i);
        }
    AABBTreeLines::LinesDistancer<Line> tree(pieces);
    auto clearance = [&](size_t i) {
        const Line &a        = pieces[i];
        double      distance = std::numeric_limits<double>::max();
        for (size_t j : tree.all_lines_in_radius(a.midpoint(), 0.5 * a.length() + 2. * d1))
            if (owner[j] != owner[i]) {
                const Line &b = pieces[j];
                distance      = std::min({ distance, a.distance_to(b.a), a.distance_to(b.b), b.distance_to(a.a), b.distance_to(a.b) });
            }
        return distance;
    };
    auto inside = [inner](const Point &p) { return std::abs(p.x()) < inner && std::abs(p.y()) < inner; };

    double closest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < pieces.size(); ++i)
        if (inside(pieces[i].midpoint()))
            closest = std::min(closest, clearance(i));
    CHECK(closest > 0.99 * d1);

    // Each path at a crossing touches another one there, none stops short of it.
    double widest = 0.;
    for (size_t i = 0; i < lines.size(); ++i)
        for (size_t j = i + 1; j < lines.size(); ++j)
            if (Point crossing; line_alg::intersection(lines[i], lines[j], &crossing) && inside(crossing)) {
                std::map<size_t, double> at;
                for (size_t k : tree.all_lines_in_radius(crossing, 1.2 * d1))
                    at.emplace(owner[k], std::numeric_limits<double>::max());
                for (size_t k : tree.all_lines_in_radius(crossing, 2. * d1))
                    if (auto it = at.find(owner[k]); it != at.end())
                        it->second = std::min(it->second, clearance(k));
                for (const auto &path : at)
                    widest = std::max(widest, path.second);
            }
    CHECK(widest < 1.02 * d1);
}

TEST_CASE("Multiline adaptive cubic paths reach the line they end on when another path ends on them", "[Fill]")
{
    const int    sweep = GENERATE(0, 1, 2);
    // Where the 120 degree line starts on the horizontal one, in walls from the 60 degree line.
    const double start = GENERATE(0.3, 0.6, 1., 2.);
    CAPTURE(sweep, start);

    const double d1 = scale_(0.8), overlap = 0.1 * d1, length = scale_(30.);
    const Vec2d  diagonal(0.5, 0.5 * std::sqrt(3.)), horizontal(1., 0.), steep(-0.5, 0.5 * std::sqrt(3.));
    const Vec2d  on_horizontal = start * d1 * horizontal;
    const Lines  lines{ Line((-length * diagonal).cast<coord_t>(), (length * diagonal).cast<coord_t>()),
                        Line(Point(0, 0), (length * horizontal).cast<coord_t>()),
                        Line(on_horizontal.cast<coord_t>(), (on_horizontal - length * steep).cast<coord_t>()) };
    const Polylines paths = FillAdaptive::multiline_paths(lines, d1, overlap, sweep, BoundingBox(Point::new_scale(-40., -40.), Point::new_scale(40., 40.)));

    // The end of the path along each line nearest to where that line starts.
    auto end_along = [&paths](const Line &line) {
        for (const Polyline &path : paths)
            if (line.distance_to(path.first_point()) < SCALED_EPSILON && line.distance_to(path.last_point()) < SCALED_EPSILON)
                return (path.first_point() - line.a).cast<double>().norm() < (path.last_point() - line.a).cast<double>().norm() ? path.first_point() : path.last_point();
        return Point(std::numeric_limits<coord_t>::max(), 0);
    };
    const Point horizontal_end = end_along(lines[1]), steep_end = end_along(lines[2]);
    REQUIRE(horizontal_end.x() != std::numeric_limits<coord_t>::max());
    REQUIRE(steep_end.x() != std::numeric_limits<coord_t>::max());
    // Both reach the overlap into the wall of the path they stop at, none stops short of it.
    CHECK_THAT(line_alg::distance_to_infinite(lines[0], horizontal_end) / d1, Catch::Matchers::WithinAbs(0.9, 0.01));
    CHECK(lines[1].distance_to(steep_end) / d1 < 0.91);
    CHECK(get_intersections(to_lines(paths)).empty());
}

TEST_CASE("3D honeycomb infill rounds its octahedral waves with the smooth factor", "[Fill]")
{
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "3dhoneycomb"},
                                             {"sparse_infill_density", "20%"},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.point_count > 0);
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
    REQUIRE(smooth.length < sharp.length);
}

TEST_CASE("Smoothed concentric infill stays inside the fill region", "[Fill][Regression]")
{
    // The concentric loops are offsets of the fill region and are never clipped to it, so a corner
    // rounded across its boundary ends up in a hole or over a wall. Rounding cuts toward the inside of
    // the turn, which leaves the region at every corner of a hole, and in a region thinner than the
    // curve even at a corner turning inwards.
    const bool  thin_region = GENERATE(false, true);
    ExPolygon   region;
    if (thin_region) {
        // An L of two 1.2mm wide arms: cutting the corner they meet at crosses both of them.
        region = ExPolygon{ Slic3r::Points{
            Point::new_scale(0., 0.), Point::new_scale(20., 0.), Point::new_scale(20., 1.2),
            Point::new_scale(1.2, 1.2), Point::new_scale(1.2, 20.), Point::new_scale(0., 20.) } };
    } else {
        region = ExPolygon{ Slic3r::Points{ Point::new_scale(0., 0.), Point::new_scale(50., 0.),
                                            Point::new_scale(50., 50.), Point::new_scale(0., 50.) },
                            Slic3r::Points{ Point::new_scale(30., 20.), Point::new_scale(30., 30.),
                                            Point::new_scale(20., 30.), Point::new_scale(20., 20.) } };
    }
    CAPTURE(thin_region);

    auto fill = [&region](double smooth_factor) {
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type("concentric"));
        filler->spacing = 0.45;

        FillParams params;
        params.density       = 0.1f;
        params.dont_adjust   = true;
        params.smooth_factor = smooth_factor;

        Slic3r::Surface surface(stInternal, region);
        return filler->fill_surface(&surface, params);
    };
    auto point_count = [](const Slic3r::Polylines &polylines) {
        return std::accumulate(polylines.begin(), polylines.end(), size_t(0),
                               [](size_t count, const Polyline &polyline) { return count + polyline.size(); });
    };

    const Slic3r::Polylines sharp  = fill(0.);
    const Slic3r::Polylines smooth = fill(1.);
    REQUIRE(!sharp.empty());

    // Nothing leaves the fill region, which the unrounded loops already touch from the inside.
    const ExPolygons bounds = offset_ex(region, float(SCALED_EPSILON));
    REQUIRE(diff_pl(sharp, bounds).empty());
    REQUIRE(diff_pl(smooth, bounds).empty());
    // The corners that the region has room for are still rounded.
    if (!thin_region)
        REQUIRE(point_count(smooth) > point_count(sharp));
}

TEST_CASE("Smoothing multiline lightning infill keeps its outlines connected", "[Fill][Regression]")
{
    // With more than one line per infill wall, the branches are printed as outlines drawn around them,
    // and the outlines of branches that run close to each other merge into one. Rounding the branches
    // before those outlines are built moves them apart, which breaks the merged outlines up into
    // separate loops - many more of them, each needing its own travel move.
    // A micron change of the cube moves the loop count of a single slice by several percent, so the
    // shapes of a few nearly equal cubes are added up.
    auto shape_for = [](const std::string &smooth_factor) {
        SparseInfillShape sum;
        for (const double size : {20., 20.001, 20.002, 20.003}) {
            Print print;
            Slic3r::Test::init_and_process_print({Slic3r::Test::cube(size)}, print,
                                                {{"sparse_infill_pattern", "lightning"},
                                                 {"sparse_infill_density", "50%"},
                                                 {"fill_multiline", 2},
                                                 {"sparse_infill_smooth_factor", smooth_factor},
                                                 {"layer_height", 0.2}});
            const SparseInfillShape shape = sparse_infill_shape(print);
            sum.path_count += shape.path_count;
            sum.point_count += shape.point_count;
            sum.sharp_turns += shape.sharp_turns;
        }
        return sum;
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.path_count > 0);
    // Smoothing should leave the path count about where it was; uncapping the smoothing reach, the
    // regression this guards against, adds a quarter to a third. Outline ends next to each other are
    // joined into one path, so a few loops more or less are a larger share of the count.
    const size_t allowed_extra = sharp.path_count / 10; // 10%
    REQUIRE(smooth.path_count <= sharp.path_count + allowed_extra);
    // The outlines are still rounded.
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
}

TEST_CASE("Sparse plane-path anchors match the printed infill", "[Fill][InternalBridge][Regression]")
{
    // Orca: Compare generated anchors with actual extrusion across plane-path patterns,
    // smoothing, multiline and rotations; an origin shift must not pass as valid support.
    const std::string pattern = GENERATE("hilbertcurve", "octagramspiral", "archimedeanchords");
    const std::string smoothing = GENERATE("0%", "100%");
    const int multiline = GENERATE(1, 2);
    const bool rotated = GENERATE(false, true);
    const bool separated = GENERATE(false, true);
    CAPTURE(pattern, smoothing, multiline, rotated, separated);

    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "15%"},
                                   {"sparse_infill_smooth_factor", smoothing},
                                   {"fill_multiline", multiline},
                                   {"infill_direction", 45},
                                   {"sparse_infill_rotate_template", rotated ? "0,25,50" : ""},
                                   {"align_infill_direction_to_model", rotated},
                                   {"separated_infills", separated},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"resolution", 0.012}});
    Print print;
    Model model;
    TriangleMesh mesh = make_cube(30, 24, 1);
    if (separated) {
        // Orca: Two disconnected bodies in one object must each use their own infill origin.
        TriangleMesh second = make_cube(30, 24, 1);
        second.translate(50, 0, 0);
        mesh.merge(second);
    }
    Slic3r::Test::init_print({mesh}, print, model, config, nullptr, false);
    if (rotated) {
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(23.)));
        print.apply(model, config);
    }
    print.process();

    const Layer &layer = *print.objects().front()->get_layer(4);
    const Polylines printed = layer_paths(layer, erInternalInfill);
    REQUIRE_FALSE(printed.empty());
    const AABBTreeLines::LinesDistancer<Line> printed_tree(to_lines(printed));

    // Orca: Exclude perimeter connections: anchoring and extrusion can trim those differently.
    const Polylines anchors = intersection_pl(layer.generate_sparse_infill_polylines_for_anchoring(nullptr, nullptr),
                                              shrink(to_polygons(layer.lslices), scale_(3.)));
    REQUIRE_FALSE(anchors.empty());
    double max_distance = 0.;
    for (const Polyline &path : anchors)
        for (const Point &point : path.equally_spaced_points(scale_(0.25)))
            max_distance = std::max(max_distance, printed_tree.distance_from_lines<false>(point));
    // Orca: Allow only the configured simplification tolerance; infill-scale offsets
    // would hide anchors that no longer coincide with printed lines.
    CHECK(unscale<double>(max_distance) <= config.opt_float("resolution"));
}

// Orca: Slices the meshes as the parts of one object, where they are, with modifiers of their own config.
static Print &slice_parts(Print &print, DynamicPrintConfig config, const std::vector<TriangleMesh> &parts,
                          const std::vector<std::pair<TriangleMesh, DynamicPrintConfig>> &modifiers = {})
{
    config.set_deserialize_strict({{"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"elefant_foot_compensation", 0},
                                   {"top_shell_thickness", 0},
                                   {"bottom_shell_thickness", 0}});
    Model model;
    Slic3r::Test::init_print({parts.front()}, print, model, config, nullptr, false);
    for (size_t i = 1; i < parts.size(); ++ i)
        model.objects.front()->add_volume(TriangleMesh(parts[i]), ModelVolumeType::MODEL_PART, false);
    for (const auto &[mesh, modifier_config] : modifiers)
        model.objects.front()->add_volume(TriangleMesh(mesh), ModelVolumeType::PARAMETER_MODIFIER, false)->config.apply(modifier_config);
    print.apply(model, config);
    print.process();
    return print;
}

// Orca: Two identical cubes in one mesh that never touch, so each is a body of its own.
static Print &slice_two_bodies(Print &print, const DynamicPrintConfig &config, double height)
{
    TriangleMesh mesh = make_cube(20, 20, height);
    TriangleMesh second = make_cube(20, 20, height);
    second.translate(33, 7, 0);
    mesh.merge(second);
    return slice_parts(print, config, {mesh});
}

// Orca: Counts the points sampled along both sets that the other set does not repeat.
static void count_unmatched(const Polylines &a, const Polylines &b, size_t &sampled, size_t &unmatched)
{
    const std::array<const Polylines *, 2> sets{&a, &b};
    for (size_t i = 0; i < 2; ++ i) {
        const Polylines                          &other = *sets[1 - i];
        const AABBTreeLines::LinesDistancer<Line> distancer(to_lines(other));
        for (const Polyline &path : *sets[i])
            for (const Point &point : path.equally_spaced_points(scale_(0.2))) {
                ++ sampled;
                unmatched += other.empty() || distancer.distance_from_lines<false>(point) > scale_(0.05);
            }
    }
}

// Orca: Share of the paths of a role that the other body does not repeat around its own center.
static double unmatched_between_bodies(const Print &print, ExtrusionRole role)
{
    size_t sampled = 0, unmatched = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        REQUIRE(layer->lslices.size() == 2);
        const Polylines          polylines = layer_paths(*layer, role);
        std::array<Polylines, 2> paths;
        for (size_t body = 0; body < 2; ++ body) {
            // Orca: Exclude the links along the walls, which each body may chain differently.
            paths[body] = intersection_pl(polylines, shrink(to_polygons(layer->lslices[body]), scale_(3.)));
            for (Polyline &path : paths[body])
                path.translate(-layer->lslices_bboxes[body].center());
        }
        count_unmatched(paths[0], paths[1], sampled, unmatched);
    }
    REQUIRE(sampled > 0);
    return double(unmatched) / double(sampled);
}

// Orca: Share of the paths of a role inside a bed region that two slices of the same body do not share.
static double unmatched_between_prints(const Print &a, const Print &b, ExtrusionRole role, const Polygons &region)
{
    const PrintObject &object_a = *a.objects().front(), &object_b = *b.objects().front();
    REQUIRE(object_a.layer_count() == object_b.layer_count());
    size_t sampled = 0, unmatched = 0;
    for (size_t i = 0; i < object_a.layer_count(); ++ i) {
        std::array<Polylines, 2> paths;
        for (const PrintObject *object : {&object_a, &object_b}) {
            Polylines &out = paths[object == &object_b];
            out = layer_paths(*object->get_layer(int(i)), role);
            for (Polyline &path : out)
                path.translate(object->instances().front().shift);
            out = intersection_pl(out, region);
        }
        count_unmatched(paths[0], paths[1], sampled, unmatched);
    }
    REQUIRE(sampled > 0);
    return double(unmatched) / double(sampled);
}

TEST_CASE("Separated infill centers the sparse infill of each body on itself", "[Fill][Regression]")
{
    const std::string pattern = GENERATE("line", "zigzag", "crosszag", "honeycomb", "3dhoneycomb", "crosshatch", "tpmsd", "tpmsfk", "gyroid");
    const bool separated = GENERATE(false, true);
    CAPTURE(pattern, separated);
    auto config = DynamicPrintConfig::full_print_config();
    // Orca: The Zig Zag patterns mirror each body about its own center.
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "20%"},
                                   {"symmetric_infill_y_axis", true},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"separated_infills", separated}});
    Print print;
    const double unmatched = unmatched_between_bodies(slice_two_bodies(print, config, 2.), erInternalInfill);
    // Orca: Without separation both bodies cut one object-wide pattern at different places.
    if (separated)
        CHECK(unmatched < 0.02);
    else
        CHECK(unmatched > 0.5);
}

TEST_CASE("Separated infill centers monotonic and rectilinear bridges on each body", "[Fill][InternalBridge][Regression]")
{
    // Orca: Bridges use the Monotonic pattern below monotonic top surfaces and Rectilinear otherwise.
    const std::string top_pattern = GENERATE("monotonicline", "rectilinear");
    const bool separated = GENERATE(false, true);
    CAPTURE(top_pattern, separated);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", "rectilinear"},
                                   {"sparse_infill_density", "15%"},
                                   {"top_surface_pattern", top_pattern},
                                   {"top_shell_layers", 4},
                                   {"bottom_shell_layers", 0},
                                   {"separated_infills", separated}});
    Print print;
    const double unmatched = unmatched_between_bodies(slice_two_bodies(print, config, 4.), erInternalBridgeInfill);
    if (separated)
        CHECK(unmatched < 0.02);
    else
        CHECK(unmatched > 0.5);
}

// Orca: Share of the infill of an off center pillar, and of the frame of four overlapping bars around it,
// that each body sliced alone does not repeat. The frame is one body of several parts that holds the pillar.
static std::pair<double, double> frame_and_pillar_unmatched(const DynamicPrintConfig &config)
{
    auto box = [](double x, double y, double size_x, double size_y) {
        TriangleMesh mesh = make_cube(size_x, size_y, 6);
        mesh.translate(x, y, 0);
        return mesh;
    };
    const std::vector<TriangleMesh> frame{box(0, 0, 60, 14), box(0, 46, 60, 14), box(0, 0, 14, 60), box(46, 0, 14, 60)};
    const std::vector<TriangleMesh> pillar{box(18, 20, 16, 16)};
    std::vector<TriangleMesh>       both = frame;
    both.push_back(pillar.front());
    Print print_both, print_frame, print_pillar;
    slice_parts(print_both, config, both);
    slice_parts(print_frame, config, frame);
    slice_parts(print_pillar, config, pillar);

    // Orca: Bed regions 3 mm inside the walls, away from the links along them.
    auto rect = [](double x0, double y0, double x1, double y1) {
        return Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
    };
    return {unmatched_between_prints(print_both, print_pillar, erInternalInfill, {rect(21, 23, 31, 33)}),
            unmatched_between_prints(print_both, print_frame, erInternalInfill, diff(Polygons{rect(3, 3, 57, 57)}, Polygons{rect(11, 11, 49, 49)}))};
}

TEST_CASE("Separated infill fills each body like the body sliced alone", "[Fill][Regression]")
{
    // Orca: Hilbert Curve and the Zig Zag links follow the extent of the box, not only its center.
    const std::string pattern = GENERATE("hilbertcurve", "zigzag", "crosszag", "gyroid");
    CAPTURE(pattern);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "20%"},
                                   {"symmetric_infill_y_axis", true},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"separated_infills", true}});
    const std::pair<double, double> unmatched = frame_and_pillar_unmatched(config);
    CHECK(unmatched.first < 0.02);
    CHECK(unmatched.second < 0.02);
}

TEST_CASE("Adaptive infill fills each body like the body sliced alone", "[Fill][Regression]")
{
    const std::string pattern = GENERATE("adaptivecubic", "supportcubic");
    // Orca: Octree infill centers each body whether or not separated infills are enabled.
    const bool separated = GENERATE(false, true);
    CAPTURE(pattern, separated);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "40%"},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"separated_infills", separated}});
    // Orca: The octree of the whole object is laid out from its center, which the off center pillar does not share.
    const std::pair<double, double> unmatched = frame_and_pillar_unmatched(config);
    CHECK(unmatched.first < 0.02);
    CHECK(unmatched.second < 0.02);
}

TEST_CASE("Adaptive infill of a modifier leaves the density of the other regions", "[Fill][Regression]")
{
    const std::string pattern = GENERATE("adaptivecubic", "supportcubic");
    CAPTURE(pattern);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"sparse_infill_pattern", pattern},
                                   {"sparse_infill_density", "15%"},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0}});
    TriangleMesh bodies = make_cube(30, 30, 6), second = make_cube(30, 30, 6);
    second.translate(40, 0, 0);
    bodies.merge(second);
    // Orca: A denser modifier over the right half of the second body.
    TriangleMesh modifier = make_cube(20, 40, 10);
    modifier.translate(55, -5, -2);
    DynamicPrintConfig dense = config;
    dense.set_deserialize_strict({{"sparse_infill_density", "60%"}});
    Print print, print_sparse, print_dense;
    slice_parts(print, config, {bodies}, {{modifier, dense}});
    slice_parts(print_sparse, config, {bodies});
    slice_parts(print_dense, dense, {bodies});

    // Orca: Bed regions 3 mm inside the walls and the modifier, away from the links along them.
    auto rect = [](double x0, double y0, double x1, double y1) {
        return Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
    };
    CHECK(unmatched_between_prints(print, print_sparse, erInternalInfill, {rect(3, 3, 27, 27), rect(43, 3, 52, 27)}) < 0.02);
    CHECK(unmatched_between_prints(print, print_dense, erInternalInfill, {rect(58, 3, 67, 27)}) < 0.02);
}

// Length of the sparse infill of the first object over the layers with print_z in [z_min, z_max], inside clip.
static double sparse_infill_length(const Print &print, coordf_t z_min, coordf_t z_max, const Polygons &clip)
{
    Polylines polylines;
    for (const Layer *layer : print.objects().front()->layers())
        if (layer->print_z >= z_min && layer->print_z <= z_max)
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                    if (entity->role() == erInternalInfill)
                        entity->collect_polylines(polylines);
    return unscale<double>(total_length(intersection_pl(polylines, clip)));
}

static Polygons centered_square(double half)
{
    return {Polygon({Point::new_scale(-half, -half), Point::new_scale(half, -half), Point::new_scale(half, half), Point::new_scale(-half, half)})};
}

TEST_CASE("Adaptive TPMS infill thins out from the surface to the interior density", "[Fill]")
{
    // The modes following the distance print more lines than their target where its levels meet: along the shells, or
    // where the patterns blend.
    const auto row = GENERATE(table<std::string, std::string, double>({{"tpmsd", "lobes", 0.5},
                                                                        {"tpmsfk", "lobes", 0.5},
                                                                        {"gyroid", "lobes", 0.5},
                                                                        {"gyroid", "stepped_shells", 0.6},
                                                                        {"tpmsfk", "smooth_blend", 0.6},
                                                                        {"tpmsd", "distance_warp", 0.5}}));
    const std::string pattern  = std::get<0>(row);
    const std::string mode     = std::get<1>(row);
    const double      max_core = std::get<2>(row);
    CAPTURE(pattern, mode);
    // A 60 mm cube in 0.4 mm layers, centered on the origin in XY. Its center is 30 mm from every face.
    auto slice = [&pattern](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(60)}, print,
                                            {{"sparse_infill_pattern", pattern},
                                             {"sparse_infill_density", "25%"},
                                             {"adaptive_infill_mode", adaptive},
                                             {"adaptive_infill_interior_density", "5%"},
                                             {"adaptive_infill_gradient", "linear"},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
    };
    Print uniform, adaptive;
    slice("disabled", uniform);
    slice(mode, adaptive);

    // The middle of the cube, at most 10 mm from the center: at most 5% + 20% * 10 / 30 = 11.7% dense.
    const Polygons core = centered_square(10.);
    // Along the sides at mid height, 24 to 28 mm from the center: at least 5% + 20% * 24 / 30 = 21% dense.
    const Polygons shell = diff(centered_square(28.), centered_square(24.));
    const double core_uniform = sparse_infill_length(uniform, 25., 35., core);
    const double shell_uniform = sparse_infill_length(uniform, 25., 35., shell);
    REQUIRE(core_uniform > 0.);
    REQUIRE(shell_uniform > 0.);
    CHECK(sparse_infill_length(adaptive, 25., 35., core) < max_core * core_uniform);
    CHECK(sparse_infill_length(adaptive, 25., 35., shell) > 0.75 * shell_uniform);
}

TEST_CASE("Adaptive TPMS infill of a tall object is sparsest at its middle height", "[Fill]")
{
    // A 30 x 30 x 90 mm box in 0.4 mm layers: its center is 45 mm high, the middle of its core, not a column.
    Print print;
    Slic3r::Test::init_and_process_print({make_cube(30., 30., 90.)}, print,
                                        {{"sparse_infill_pattern", "tpmsd"},
                                         {"sparse_infill_density", "25%"},
                                         {"adaptive_infill_mode", "lobes"},
                                         {"adaptive_infill_interior_density", "5%"},
                                         {"adaptive_infill_gradient", "linear"},
                                         {"layer_height", 0.4},
                                         {"initial_layer_print_height", 0.4}});
    const Polygons core   = centered_square(5.);
    const double   middle = sparse_infill_length(print, 40., 50., core);
    const double   low    = sparse_infill_length(print, 10., 20., core);
    REQUIRE(low > 0.);
    CHECK(middle < 0.7 * low);
}

TEST_CASE("2D adaptive TPMS infill does not change along its axis", "[Fill]")
{
    // A box of 30 x 30 mm sections, 90 mm long along the axis, centered on the origin in XY.
    const std::string mode = GENERATE("normal_x", "normal_z");
    CAPTURE(mode);
    const bool   along_x = mode == "normal_x";
    auto         slice   = [&](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({along_x ? make_cube(90., 30., 30.) : make_cube(30., 30., 90.)}, print,
                                                         {{"sparse_infill_pattern", "gyroid"},
                                                          {"sparse_infill_density", "25%"},
                                                          {"adaptive_infill_mode", adaptive},
                                                          {"adaptive_infill_interior_density", "5%"},
                                                          {"adaptive_infill_gradient", "linear"},
                                                          {"layer_height", 0.4},
                                                          {"initial_layer_print_height", 0.4}});
    };
    Print uniform, adaptive;
    slice("disabled", uniform);
    slice(mode, adaptive);

    // The core of the sections near an end and at the middle of the box, at most 5 mm from their center.
    auto rectangle = [](double x0, double x1) {
        return Polygons{Polygon({Point::new_scale(x0, -5.), Point::new_scale(x1, -5.), Point::new_scale(x1, 5.), Point::new_scale(x0, 5.)})};
    };
    auto core = [&](const Print &print, bool end) {
        return along_x ? sparse_infill_length(print, 10., 20., rectangle(end ? -40. : -5., end ? -30. : 5.)) :
                         sparse_infill_length(print, end ? 10. : 40., end ? 20. : 50., centered_square(5.));
    };
    const double end    = core(adaptive, true);
    const double middle = core(adaptive, false);
    REQUIRE(end > 0.);
    CHECK(middle > 0.8 * end);
    CHECK(middle < 1.25 * end);
    CHECK(middle < 0.7 * core(uniform, false));
}

TEST_CASE("Adaptive TPMS shells and blend keep the whole deep core of a tall object sparse", "[Fill]")
{
    // Distance warp grades it partly along the height, like Lobes, as its warp is centered on the middle.
    const std::string mode = GENERATE("stepped_shells", "smooth_blend");
    CAPTURE(mode);
    // A 30 x 30 x 90 mm box: from 15 to 75 mm high its axis is 15 mm deep, as deep as its center.
    auto slice = [](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({make_cube(30., 30., 90.)}, print,
                                            {{"sparse_infill_pattern", "tpmsd"},
                                             {"sparse_infill_density", "25%"},
                                             {"adaptive_infill_mode", adaptive},
                                             {"adaptive_infill_interior_density", "5%"},
                                             {"adaptive_infill_gradient", "linear"},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
    };
    Print uniform, distance;
    slice("disabled", uniform);
    slice(mode, distance);
    // At 15 to 25 mm high the axis is as sparse as at the middle, where Lobes grades it towards the bottom.
    const Polygons core        = centered_square(5.);
    const double   uniform_low = sparse_infill_length(uniform, 15., 25., core);
    const double   low         = sparse_infill_length(distance, 15., 25., core);
    const double   middle      = sparse_infill_length(distance, 40., 50., core);
    CAPTURE(uniform_low, low, middle);
    REQUIRE(middle > 0.);
    CHECK(low < 0.7 * uniform_low);
    CHECK(low < 1.25 * middle);
}

TEST_CASE("Adaptive TPMS infill is sparse at the center of both of two united spheres", "[Fill]")
{
    // Two spheres of 20 mm, their centers 32 mm apart at 20 mm high: centered on the origin, they are at x = -16 and 16.
    TriangleMesh spheres = make_sphere(20., 2. * PI / 90.);
    spheres.translate(-16.f, 0.f, 20.f);
    TriangleMesh second = make_sphere(20., 2. * PI / 90.);
    second.translate(16.f, 0.f, 20.f);
    spheres.merge(second);
    auto slice = [&spheres](const std::string &adaptive, Print &print) {
        Slic3r::Test::init_and_process_print({TriangleMesh(spheres)}, print,
                                            {{"sparse_infill_pattern", "gyroid"},
                                             {"sparse_infill_density", "25%"},
                                             {"adaptive_infill_mode", adaptive},
                                             {"adaptive_infill_interior_density", "5%"},
                                             {"adaptive_infill_gradient", "linear"},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
    };
    Print uniform, adaptive;
    slice("disabled", uniform);
    slice("lobes", adaptive);
    // Within 5 mm of either center: at most 5% + 20% * 7.1 / 20 = 12.1% dense, where one center for both
    // would leave the other sphere at more than 60% of the uniform density.
    for (const double x : {-16., 16.}) {
        CAPTURE(x);
        Polygons core = centered_square(5.);
        for (Polygon &square : core)
            square.translate(Point::new_scale(x, 0.));
        const double core_uniform = sparse_infill_length(uniform, 18., 22., core);
        REQUIRE(core_uniform > 0.);
        CHECK(sparse_infill_length(adaptive, 18., 22., core) < 0.45 * core_uniform);
    }
}

TEST_CASE("Adaptive TPMS gradients keep the surface density deeper in the order quadratic, linear, exponential", "[Fill]")
{
    // With a denser surface, t^2 <= t and the geometric interpolation is below the linear one at every depth.
    auto length_for = [](const std::string &gradient) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(40)}, print,
                                            {{"sparse_infill_pattern", "tpmsd"},
                                             {"sparse_infill_density", "25%"},
                                             {"adaptive_infill_mode", "lobes"},
                                             {"adaptive_infill_interior_density", "5%"},
                                             {"adaptive_infill_gradient", gradient},
                                             {"layer_height", 0.4},
                                             {"initial_layer_print_height", 0.4}});
        return sparse_infill_length(print, 0., 40., centered_square(20.));
    };
    const double quadratic   = length_for("quadratic");
    const double linear      = length_for("linear");
    const double exponential = length_for("exponential");
    CHECK(quadratic > linear);
    CHECK(linear > exponential);
}

TEST_CASE("Adaptive TPMS settings leave the infill unchanged when they do not apply", "[Fill]")
{
    // Adaptive density turned off, or turned on for a pattern that is no TPMS.
    const auto [pattern, adaptive] = GENERATE(
        table<std::string, std::string>({{"tpmsd", "disabled"}, {"tpmsfk", "disabled"}, {"gyroid", "disabled"}, {"grid", "lobes"}, {"grid", "stepped_shells"}, {"grid", "smooth_blend"}, {"grid", "distance_warp"}, {"grid", "normal_z"}}));
    CAPTURE(pattern, adaptive);
    Print reference, tuned;
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, reference,
                                        {{"sparse_infill_pattern", pattern}, {"sparse_infill_density", "20%"}, {"layer_height", 0.2}});
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, tuned,
                                        {{"sparse_infill_pattern", pattern},
                                         {"sparse_infill_density", "20%"},
                                         {"layer_height", 0.2},
                                         {"adaptive_infill_mode", adaptive},
                                         {"adaptive_infill_interior_density", "40%"},
                                         {"adaptive_infill_gradient", "exponential"}});
    const SparseInfillShape expected = sparse_infill_shape(reference);
    REQUIRE(expected.path_count > 0);
    CHECK(sparse_infill_shape(tuned).sequence == expected.sequence);
}

TEST_CASE("Adaptive gyroid infill ignores the Z-buckling optimization", "[Fill]")
{
    auto shape_for = [](const std::string &gyroid_optimized) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "gyroid"},
                                             {"sparse_infill_density", "10%"},
                                             {"gyroid_optimized", gyroid_optimized},
                                             {"adaptive_infill_mode", "lobes"},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };
    const SparseInfillShape expected = shape_for("0");
    REQUIRE(expected.path_count > 0);
    CHECK(shape_for("1").sequence == expected.sequence);
}

TEST_CASE("Adaptive TPMS infill of a region matches the infill of a larger region of the same object", "[Fill]")
{
    const InfillPattern pattern = GENERATE(ipGyroid, ipTpmsD, ipTpmsFK);
    CAPTURE(pattern);
    // An 80 x 80 x 40 mm box around both regions, which share its radial field.
    const ExPolygons box{ExPolygon(Points{Point::new_scale(60., 20.), Point::new_scale(140., 20.), Point::new_scale(140., 100.),
                                          Point::new_scale(60., 100.)})};
    std::vector<AdaptiveInfillField::Slice> slices;
    for (int i = 0; i < 200; ++i)
        slices.push_back({0.2 * i, 0.2 * (i + 1), &box});
    const AdaptiveInfillField field(slices, get_extents(box), AdaptiveInfillMode::Lobes, [] {});

    auto circle = [](double radius) {
        Polygon contour = make_circle_num_segments(scale_(radius), 120);
        contour.translate(Point::new_scale(100., 60.));
        return ExPolygon(std::move(contour));
    };
    const ExPolygon region = circle(20.);
    const ExPolygon larger = circle(30.);
    auto fill = [pattern, &field](const ExPolygon &expolygon, double z) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
        filler->spacing           = 0.45;
        filler->angle             = float(M_PI / 7.);
        filler->z                 = z;
        filler->adaptive_infill_field = &field;

        FillParams params;
        params.density                = 0.2f;
        params.adaptive_infill_mode          = AdaptiveInfillMode::Lobes;
        params.adaptive_infill_interior_density  = 0.05f;
        params.adaptive_infill_gradient = AdaptiveInfillGradient::Linear;
        params.layer_height           = 0.2;
        params.dont_adjust            = true;
        Surface surface(stInternal, expolygon);
        return filler->fill_surface(&surface, params);
    };
    // Away from the boundary of the region, where both are clipped and connected the same way.
    const Polygons inner = shrink(to_polygons(region), scale_(1.));
    auto farthest = [&inner](const Polylines &from, const Polylines &to) {
        const AABBTreeLines::LinesDistancer<Line> tree(to_lines(to));
        double distance = 0.;
        for (const Polyline &path : intersection_pl(from, inner))
            for (const Point &point : path.equally_spaced_points(scale_(0.2)))
                distance = std::max(distance, tree.distance_from_lines<false>(point));
        return unscale<double>(distance);
    };
    // Marching squares simplifies rings that start elsewhere in each region. At 15.325 mm TPMS-FK has a saddle,
    // whose lines connect one way or the other with the sampling grid.
    const double tolerance = SPARSE_INFILL_RESOLUTION + 0.01;
    for (const double z : {5.1, 12.3, 15.325, 20.1, 27.9, 34.7}) {
        CAPTURE(z);
        const Polylines paths = fill(region, z);
        REQUIRE_FALSE(paths.empty());
        const Polylines reference = fill(larger, z);
        CHECK(farthest(reference, paths) < tolerance);
        CHECK(farthest(paths, reference) < tolerance);
    }
}

TEST_CASE("Infill lines do not end where the rings of the pattern start", "[Fill][Regression]")
{
    // Marching squares patterns and multiline outlines are closed rings, starting anywhere along their lines.
    const auto [pattern, mode, optimized, first_multiline] = GENERATE(table<InfillPattern, AdaptiveInfillMode, bool, int>(
        {{ipGyroid, AdaptiveInfillMode::DistanceWarp, false, 1},
         {ipTpmsD, AdaptiveInfillMode::DistanceWarp, false, 1},
         {ipTpmsFK, AdaptiveInfillMode::DistanceWarp, false, 1},
         {ipTpmsFK, AdaptiveInfillMode::Disabled, false, 1},
         {ipGyroid, AdaptiveInfillMode::Disabled, true, 1},
         {ipGyroid, AdaptiveInfillMode::Disabled, false, 2},
         {ipTpmsD, AdaptiveInfillMode::Disabled, false, 2},
         {ipHoneycomb, AdaptiveInfillMode::Disabled, false, 2},
         {ipQuarterCubic, AdaptiveInfillMode::Disabled, false, 2},
         {ipLateralLattice, AdaptiveInfillMode::Disabled, false, 2},
         {ipLateralHoneycomb, AdaptiveInfillMode::Disabled, false, 2},
         {ipRectilinear, AdaptiveInfillMode::Disabled, false, 2},
         {ipAlignedRectilinear, AdaptiveInfillMode::Disabled, false, 2},
         {ipGrid, AdaptiveInfillMode::Disabled, false, 2},
         {ipTriangles, AdaptiveInfillMode::Disabled, false, 2},
         {ipStars, AdaptiveInfillMode::Disabled, false, 2},
         {ipCubic, AdaptiveInfillMode::Disabled, false, 2},
         {ipCrossHatch, AdaptiveInfillMode::Disabled, false, 2},
         {ipHilbertCurve, AdaptiveInfillMode::Disabled, false, 2}}));
    const int   multiline = GENERATE(1, 2, 3);
    const float density   = GENERATE(0.1f, 0.3f);
    const bool  with_hole = GENERATE(false, true);
    // A single line of the other patterns is not a ring, and the center line of these two may stop short of the boundary.
    if (multiline < first_multiline || (multiline == 3 && (pattern == ipCrossHatch || pattern == ipHilbertCurve)))
        return;
    CAPTURE(pattern, mode, optimized, multiline, density, with_hole);

    // A 60 mm cube, filled with lines connected along the boundary of its 0.45 mm narrower section.
    const double     spacing = 0.45;
    const ExPolygons box{ExPolygon(Points{Point::new_scale(0., 0.), Point::new_scale(60., 0.), Point::new_scale(60., 60.), Point::new_scale(0., 60.)})};
    ExPolygon        region = box.front();
    if (with_hole) {
        Polygon hole = make_circle_num_segments(scale_(10.), 60);
        hole.translate(Point::new_scale(30., 30.));
        hole.reverse();
        region.holes.emplace_back(std::move(hole));
    }
    std::unique_ptr<AdaptiveInfillField> field;
    if (mode != AdaptiveInfillMode::Disabled) {
        std::vector<AdaptiveInfillField::Slice> slices;
        for (int i = 0; i < 300; ++i)
            slices.push_back({0.2 * i, 0.2 * (i + 1), &box});
        field = std::make_unique<AdaptiveInfillField>(slices, get_extents(box), AdaptiveInfillMode::DistanceWarp, [] {});
    }
    const AABBTreeLines::LinesDistancer<Line> walls(to_lines(offset_ex(region, -float(scale_(0.5 * spacing)))));
    auto on_wall = [&walls](const Point &point) { return walls.distance_from_lines<false>(point) < scale_(0.01); };
    auto runs_between_walls = [&on_wall](const Polyline &path) {
        for (size_t i = 1; i < path.size(); ++i)
            if (path.points[i - 1].distance_to(path.points[i]) > scale_(0.1) && on_wall(path.points[i - 1]) && on_wall(path.points[i]))
                return true;
        return false;
    };

    for (const double z : {5.1, 12.3, 20.1, 25.3, 28.7, 34.7, 40.1, 47.9, 54.9}) {
        CAPTURE(z);
        std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
        filler->spacing  = spacing;
        filler->angle    = float(M_PI / 4.);
        filler->z        = z;
        filler->layer_id = size_t(std::lround(z / 0.2));
        filler->adaptive_infill_field = field.get();

        FillParams params;
        params.pattern                          = pattern;
        params.density                          = density;
        params.multiline                        = multiline;
        params.gyroid_optimized                 = optimized;
        params.adaptive_infill_mode             = mode;
        params.adaptive_infill_interior_density = 0.05f;
        params.adaptive_infill_gradient         = AdaptiveInfillGradient::Linear;
        params.layer_height                     = 0.2;
        params.dont_adjust                      = true;
        Surface surface(stInternal, region);
        const Polylines paths = filler->fill_surface(&surface, params);
        REQUIRE_FALSE(paths.empty());

        for (const Polyline &path : paths)
            if (path.first_point() == path.last_point()) {
                // Not a line joined with its duplicate.
                CHECK(path.size() > 3);
                // The anchors of a line may meet on the boundary. Away from it this is an island of the pattern, unless
                // it runs along the boundary, which means a ring cut at its start.
                if (!on_wall(path.first_point()))
                    CHECK_FALSE(runs_between_walls(path));
            } else {
                CHECK(on_wall(path.first_point()));
                CHECK(on_wall(path.last_point()));
            }
    }
}

TEST_CASE("Adaptive TPMS anchors match the printed infill", "[Fill][InternalBridge]")
{
    const std::string pattern = GENERATE("tpmsd", "tpmsfk", "gyroid");
    CAPTURE(pattern);
    Print print;
    Slic3r::Test::init_and_process_print({Slic3r::Test::cube(30)}, print,
                                        {{"sparse_infill_pattern", pattern},
                                         {"sparse_infill_density", "25%"},
                                         {"adaptive_infill_mode", "lobes"},
                                         {"adaptive_infill_interior_density", "5%"},
                                         {"layer_height", 0.2},
                                         {"initial_layer_print_height", 0.2},
                                         {"resolution", 0.012}});

    const Layer &layer = *print.objects().front()->get_layer(40);
    const Polylines printed = layer_paths(layer, erInternalInfill);
    REQUIRE_FALSE(printed.empty());
    const AABBTreeLines::LinesDistancer<Line> printed_tree(to_lines(printed));

    // Exclude the perimeter connections, which anchoring and extrusion trim differently.
    const Polylines anchors = intersection_pl(layer.generate_sparse_infill_polylines_for_anchoring(nullptr, nullptr),
                                              shrink(to_polygons(layer.lslices), scale_(3.)));
    REQUIRE_FALSE(anchors.empty());
    double max_distance = 0.;
    for (const Polyline &path : anchors)
        for (const Point &point : path.equally_spaced_points(scale_(0.25)))
            max_distance = std::max(max_distance, printed_tree.distance_from_lines<false>(point));
    CHECK(unscale<double>(max_distance) <= 0.012);
}

TEST_CASE("Infill lines whose ends are next to each other join into one path", "[Fill]")
{
    // An out-and-back pair 1 mm apart, as multiline infill leaves, and a line 19 mm away.
    Polylines lines { Polyline(Point::new_scale(0., 0.), Point::new_scale(10., 0.)), Polyline(Point::new_scale(0., 1.), Point::new_scale(10., 1.)),
                      Polyline(Point::new_scale(0., 20.), Point::new_scale(10., 20.)) };
    join_close_ends(lines, scale_(1.5), true);

    REQUIRE(lines.size() == 2);
    const auto joined = std::find_if(lines.begin(), lines.end(), [](const Polyline &pl) { return pl.size() == 4; });
    REQUIRE(joined != lines.end());
    // Both lines plus the 1 mm between them, joined at one end only, as joining the other end too would close a loop.
    CHECK_THAT(unscale<double>(joined->length()), Catch::Matchers::WithinAbs(21., 1e-6));
    CHECK(joined->first_point() != joined->last_point());
}

TEST_CASE("Joined infill lines print each line once", "[Fill]")
{
    // The middle line comes first and both of its ends get joined, so the group has to be walked from an outer line.
    Polylines lines { Polyline(Point::new_scale(0., 0.), Point::new_scale(10., 0.)), Polyline(Point::new_scale(0., 1.), Point::new_scale(10., 1.)),
                      Polyline(Point::new_scale(10., -1.), Point::new_scale(0., -1.)) };
    join_close_ends(lines, scale_(1.5), true);

    REQUIRE(lines.size() == 1);
    CHECK(lines.front().size() == 6);
    // Three 10 mm lines and two 1 mm joins.
    CHECK_THAT(unscale<double>(lines.front().length()), Catch::Matchers::WithinAbs(32., 1e-6));
}

TEST_CASE("Infill line ends are not joined across another line", "[Fill]")
{
    // A pair 1 mm apart whose left ends have a third line running between them, which a join there would cross.
    Polylines lines { Polyline(Point::new_scale(0., 0.), Point::new_scale(10., 0.)), Polyline(Point::new_scale(0., 1.), Point::new_scale(10., 1.)),
                      Polyline(Point::new_scale(-5., 0.5), Point::new_scale(5., 0.5)) };
    join_close_ends(lines, scale_(1.5), true);

    REQUIRE(lines.size() == 2);
    const auto joined = std::find_if(lines.begin(), lines.end(), [](const Polyline &pl) { return pl.size() == 4; });
    REQUIRE(joined != lines.end());
    // Joined on the right, where nothing is in between, so the left ends stay open.
    CHECK(joined->first_point().x() == 0);
    CHECK(joined->last_point().x() == 0);
}

TEST_CASE("Infill lines that cannot reverse join only end to start", "[Fill]")
{
    const Point a(Point::new_scale(0., 0.)), b(Point::new_scale(10., 0.)), c(Point::new_scale(10., 1.)), d(Point::new_scale(0., 1.));
    SECTION("Ends that would need a reversal stay apart") {
        Polylines lines { Polyline(a, b), Polyline(d, c) };
        join_close_ends(lines, scale_(1.5), false);
        CHECK(lines.size() == 2);
    }
    SECTION("The end of one line joins the start of the next, both kept in their direction") {
        Polylines lines { Polyline(c, d), Polyline(a, b) };
        join_close_ends(lines, scale_(1.5), false);
        REQUIRE(lines.size() == 1);
        const Points &points = lines.front().points;
        REQUIRE(points.size() == 4);
        auto position = [&points](const Point &p) { return std::find(points.begin(), points.end(), p) - points.begin(); };
        CHECK(position(a) < position(b));
        CHECK(position(c) < position(d));
    }
}

// Orca: the top surface fills of the topmost layer, which keep their order, each as its extrusions in print order.
static std::vector<std::vector<const ExtrusionEntity *>> top_surface_fills(const Print &print)
{
    std::vector<std::vector<const ExtrusionEntity *>> fills;
    for (const LayerRegion *region : print.objects().front()->layers().back()->regions())
        for (const ExtrusionEntity *entity : region->fills.entities)
            if (auto *fill = dynamic_cast<const ExtrusionEntityCollection *>(entity); fill && fill->no_sort)
                if (std::any_of(fill->entities.begin(), fill->entities.end(), [](const ExtrusionEntity *ee) { return ee->role() == erTopSolidInfill; }))
                    fills.emplace_back(fill->entities.begin(), fill->entities.end());
    return fills;
}

static double travel_between(const std::vector<const ExtrusionEntity *> &extrusions)
{
    double travel = 0.;
    for (size_t i = 1; i < extrusions.size(); ++ i)
        travel += unscale<double>((extrusions[i]->first_point() - extrusions[i - 1]->last_point()).cast<double>().norm());
    return travel;
}

TEST_CASE("Monotonic line top surfaces print their gap fill next to their lines and keep the lines in order", "[Fill]")
{
    // The slanted legs of the letter A leave gap fill along the walls where the lines end.
    auto slice_letter = [](Print &print, Model &model, const char *gap_fill_target) {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"top_surface_pattern", "monotonicline"},
                                       {"gap_fill_target", gap_fill_target},
                                       {"filter_out_gap_fill", 0},
                                       {"top_shell_layers", 3},
                                       {"layer_height", 0.2},
                                       {"initial_layer_print_height", 0.2}});
        Slic3r::Test::init_print({Slic3r::Test::TestMesh::A}, print, model, config);
        print.process();
    };
    Print print_without, print_with;
    Model model_without, model_with;
    slice_letter(print_without, model_without, "nowhere");
    slice_letter(print_with, model_with, "topbottom");
    const auto without = top_surface_fills(print_without);
    const auto with    = top_surface_fills(print_with);
    REQUIRE(with.size() == without.size());

    auto   is_gap_fill = [](const ExtrusionEntity *ee) { return ee->role() == erGapFill; };
    size_t gap_fills = 0, gap_fills_before_a_line = 0;
    for (size_t i = 0; i < with.size(); ++ i) {
        std::vector<const ExtrusionEntity *> lines, gaps;
        std::partition_copy(with[i].begin(), with[i].end(), std::back_inserter(gaps), std::back_inserter(lines), is_gap_fill);
        const auto last_line = std::find_if_not(with[i].rbegin(), with[i].rend(), is_gap_fill);
        gap_fills += gaps.size();
        gap_fills_before_a_line += std::count_if(last_line, with[i].rend(), is_gap_fill);
        // The lines are those of the surface without gap fill, in the same order and direction.
        REQUIRE(lines.size() == without[i].size());
        for (size_t j = 0; j < lines.size(); ++ j) {
            CHECK(lines[j]->first_point() == without[i][j]->first_point());
            CHECK(lines[j]->last_point() == without[i][j]->last_point());
        }
        // Printed where they are, the gap fills travel less than printed after the last line.
        if (! gaps.empty()) {
            std::vector<const ExtrusionEntity *> appended = lines;
            appended.insert(appended.end(), gaps.begin(), gaps.end());
            CHECK(travel_between(with[i]) < travel_between(appended));
        }
    }
    REQUIRE(gap_fills > 0);
    CHECK(gap_fills_before_a_line > 0);
}

// Orca: counts the gap fills of the fills that keep their order, and those printed before an extrusion of the fill they touch.
static std::pair<size_t, size_t> gap_fills_and_those_before_an_extrusion_they_touch(const Print &print)
{
    size_t gap_fills = 0, before = 0;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.entities) {
                auto *fill = dynamic_cast<const ExtrusionEntityCollection *>(entity);
                if (fill == nullptr || ! fill->no_sort)
                    continue;
                for (size_t i = 0; i < fill->entities.size(); ++ i) {
                    if (fill->entities[i]->role() != erGapFill)
                        continue;
                    ++ gap_fills;
                    const Polygons gap = fill->entities[i]->polygons_covered_by_width(float(scale_(0.05)));
                    before += std::any_of(fill->entities.begin() + i + 1, fill->entities.end(), [&gap](const ExtrusionEntity *ee) {
                        return ee->role() != erGapFill && ! intersection(gap, ee->polygons_covered_by_width()).empty();
                    });
                }
            }
    return { gap_fills, before };
}

TEST_CASE("Gap fill of a fill that keeps its order prints after the extrusions it touches", "[Fill]")
{
    using Slic3r::Test::TestMesh;
    const auto [pattern_key, pattern, target] = GENERATE(table<const char *, const char *, const char *>({
        { "internal_solid_infill_pattern", "spiralinset", "everywhere" },
        { "top_surface_pattern", "concentric", "topbottom" },
        { "top_surface_pattern", "monotonic", "topbottom" },
        { "top_surface_pattern", "monotonicline", "topbottom" },
    }));
    // The slanted legs of the V and the hole of the cube leave gap fill next to the lines of each pattern.
    const TestMesh mesh = GENERATE(TestMesh::V, TestMesh::cube_with_hole);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{pattern_key, pattern},
                                   {"gap_fill_target", target},
                                   {"filter_out_gap_fill", 0},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2}});
    Print print;
    Model model;
    Slic3r::Test::init_print({mesh}, print, model, config);
    print.process();
    const auto [gap_fills, before] = gap_fills_and_those_before_an_extrusion_they_touch(print);
    INFO(pattern << " on " << Slic3r::Test::mesh_names.at(mesh));
    REQUIRE(gap_fills > 0);
    CHECK(before == 0);
}
