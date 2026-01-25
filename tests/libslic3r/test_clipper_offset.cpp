#include <catch2/catch_all.hpp>

#include <iostream>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;

// #define TESTS_EXPORT_SVGS

SCENARIO("Constant offset", "[ClipperUtils]") {
	coord_t s = 1000000;
	GIVEN("20mm box") {
		ExPolygon box20mm;
		box20mm.contour.points = { Vec2crd{ 0, 0 }, Vec2crd{ 20 * s, 0 }, Vec2crd{ 20 * s, 20 * s}, Vec2crd{ 0, 20 * s} };
		std::vector<float> deltas_plus(box20mm.contour.points.size(), 1. * s);
		std::vector<float> deltas_minus(box20mm.contour.points.size(), - 1. * s);
		Polygons output;
		WHEN("Slic3r::offset()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("plus 1mm, miter " << miter << "x") {
					output = Slic3r::offset(box20mm, 1. * s, ClipperLib::jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("constant_offset_box20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 22^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE(output.front().area() == Catch::Approx(22. * 22. * s * s));
					}
				}
				DYNAMIC_SECTION("minus 1mm, miter " << miter << "x") {
					output = Slic3r::offset(box20mm, - 1. * s, ClipperLib::jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("constant_offset_box20mm_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 18^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE(output.front().area() == Catch::Approx(18. * 18. * s * s));
					}
				}
			}
		}
		WHEN("Slic3r::variable_offset_outer/inner") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("plus 1mm, miter " << miter << "x") {
					output = Slic3r::variable_offset_outer(box20mm, { deltas_plus }, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("variable_offset_box20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 22^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE(output.front().area() == Catch::Approx(22. * 22. * s * s));
					}
				}
				DYNAMIC_SECTION("minus 1mm, miter " << miter << "x") {
					output = Slic3r::variable_offset_inner(box20mm, { deltas_minus }, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("variable_offset_box20mm_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 18^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE(output.front().area() == Catch::Approx(18. * 18. * s * s));
					}
				}
			}
		}
	}

	GIVEN("20mm box with 10mm hole") {
		ExPolygon box20mm;
		box20mm.contour.points = { Vec2crd{ 0, 0 }, Vec2crd{ 20 * s, 0 }, Vec2crd{ 20 * s, 20 * s}, Vec2crd{ 0, 20 * s} };
		box20mm.holes.emplace_back(Slic3r::Polygon({ Vec2crd{ 5 * s, 5 * s }, Vec2crd{ 5 * s, 15 * s}, Vec2crd{ 15 * s, 15 * s}, Vec2crd{ 15 * s, 5 * s } }));
		std::vector<float> deltas_plus(box20mm.contour.points.size(), 1. * s);
		std::vector<float> deltas_minus(box20mm.contour.points.size(), -1. * s);
		ExPolygons output;
		SECTION("Slic3r::offset()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("miter " << miter << "x") {
					WHEN("plus 1mm") {
						output = Slic3r::offset_ex(box20mm, 1. * s, ClipperLib::jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("constant_offset_box20mm_10mm_hole_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 22^2-8^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE(output.front().area() == Catch::Approx((22. * 22. - 8. * 8.) * s * s));
						}
					}
					WHEN("minus 1mm") {
						output = Slic3r::offset_ex(box20mm, - 1. * s, ClipperLib::jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("constant_offset_box20mm_10mm_hole_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 18^2-12^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE(output.front().area() == Catch::Approx((18. * 18. - 12. * 12.) * s * s));
						}
					}
				}
			}
		}
		SECTION("Slic3r::variable_offset_outer()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("miter " << miter << "x") {
					WHEN("plus 1mm") {
						output = Slic3r::variable_offset_outer_ex(box20mm, { deltas_plus, deltas_plus }, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("variable_offset_box20mm_10mm_hole_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 22^2-8^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE(output.front().area() == Catch::Approx((22. * 22. - 8. * 8.) * s * s));
						}
					}
					WHEN("minus 1mm") {
						output = Slic3r::variable_offset_inner_ex(box20mm, { deltas_minus, deltas_minus }, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("variable_offset_box20mm_10mm_hole_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 18^2-12^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE(output.front().area() == Catch::Approx((18. * 18. - 12. * 12.) * s * s));
						}
					}
				}
			}
		}
	}

	GIVEN("20mm right angle triangle") {
		ExPolygon triangle20mm;
		triangle20mm.contour.points = { Vec2crd{ 0, 0 }, Vec2crd{ 20 * s, 0 }, Vec2crd{ 0, 20 * s } };
		Polygons output;
		double offset = 1.;
		// Angle of the sharp corner bisector.
		double angle_bisector = M_PI / 8.;
		// Area tapered by mitering one sharp corner.
		double area_tapered = pow(offset * (1. / sin(angle_bisector) - 1.), 2.) * tan(angle_bisector);
		double l_triangle_side_offsetted = 20. + offset * (1. + 1. / tan(angle_bisector));
		double area_offsetted = (0.5 * l_triangle_side_offsetted * l_triangle_side_offsetted - 2. * area_tapered) * s * s;
		SECTION("Slic3r::offset()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("Outer offset 1mm, miter " << miter << "x") {
					output = Slic3r::offset(triangle20mm, offset * s, ClipperLib::jtMiter, 2.0);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("constant_offset_triangle20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(triangle20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area matches") {
						REQUIRE(output.size() == 1);
						REQUIRE(output.front().area() == Catch::Approx(area_offsetted));
					}
				}
			}
		}
		SECTION("Slic3r::variable_offset_outer()") {
			std::vector<float> deltas(triangle20mm.contour.points.size(), 1. * s);
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("Outer offset 1mm, miter " << miter << "x") {
					output = Slic3r::variable_offset_outer(triangle20mm, { deltas }, 2.0);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("variable_offset_triangle20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(triangle20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area matches") {
						REQUIRE(output.size() == 1);
						REQUIRE(output.front().area() == Catch::Approx(area_offsetted));
					}
				}
			}
		}
	}
}
