#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
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
        const int delta = ((angles[i] - angles[i - 1]) % 180 + 180) % 180;
        CAPTURE(i, angles[i - 1], angles[i]);
        // Split rather than "delta == 0 || delta == 45" so Catch2 can show the operands.
        REQUIRE(delta % 45 == 0);
        REQUIRE(delta <= 45);
        steps += delta == 45;
    }
    CHECK(steps > 0);
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

    for (size_t i = 0; i < at_0.size(); ++i) {
        const int delta = ((at_30[i] - at_0[i]) % 180 + 180) % 180;
        CAPTURE(i, at_0[i], at_30[i]);
        CHECK(delta == 30);
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

TEST_CASE("Trapezoidal grid infill rounds its corners only with more than one line", "[Fill]")
{
    auto shape_for = [](int multiline, const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "grid"},
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

    // A single line per infill wall is the plain crossing line grid, which has no corner of its own.
    const SparseInfillShape single_sharp  = shape_for(1, "0%");
    const SparseInfillShape single_smooth = shape_for(1, "100%");
    REQUIRE(single_sharp.point_count > 0);
    REQUIRE(single_smooth.point_count == single_sharp.point_count);
    REQUIRE(single_smooth.length == single_sharp.length);
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
    auto shape_for = [](const std::string &smooth_factor) {
        Print print;
        Slic3r::Test::init_and_process_print({Slic3r::Test::cube(20)}, print,
                                            {{"sparse_infill_pattern", "lightning"},
                                             {"sparse_infill_density", "50%"},
                                             {"fill_multiline", 2},
                                             {"sparse_infill_smooth_factor", smooth_factor},
                                             {"layer_height", 0.2}});
        return sparse_infill_shape(print);
    };

    const SparseInfillShape sharp  = shape_for("0%");
    const SparseInfillShape smooth = shape_for("100%");

    REQUIRE(sharp.path_count > 0);
    // The loop count varies by a loop or two between platforms and between runs, so this is not an
    // exact comparison. Smoothing should leave it about where it was; uncapping the smoothing
    // reach, the regression this guards against, adds about 10%.
    const size_t allowed_extra = sharp.path_count / 50; // 2%
    REQUIRE(smooth.path_count <= sharp.path_count + allowed_extra);
    // The outlines are still rounded.
    REQUIRE(smooth.point_count > sharp.point_count);
    REQUIRE(smooth.sharp_turns < sharp.sharp_turns);
}
