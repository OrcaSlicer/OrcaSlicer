#define NOMINMAX

#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <fstream>

#include <libslic3r/MarchingSquares.hpp>
#include <libslic3r/SLA/RasterToPolygons.hpp>

#include <libslic3r/SLA/AGGRaster.hpp>
#include <libslic3r/MTUtils.hpp>
#include <libslic3r/SVG.hpp>
#include <libslic3r/ClipperUtils.hpp>

#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/TriangulateWall.hpp>
#include <libslic3r/Tesselate.hpp>
#include <libslic3r/SlicesToTriangleMesh.hpp>
#include <libslic3r/StreamUtils.hpp>

using namespace Slic3r;
using namespace Catch::Matchers;

// Note this tests SLA/RasterToPolygons.hpp, SLA/AGGRaster.hpp, and
// ClipperUtils.hpp at least as much as MarchingSquares.hpp.

// Get the Point corresponding to a raster column and row.
Point rstPoint(const sla::RasterGrayscaleAA& rst, const size_t c, const size_t r)
{
    size_t  rows = rst.resolution().height_px, cols = rst.resolution().width_px;
    auto    pxd   = rst.pixel_dimensions();
    auto    tr    = rst.trafo();
    coord_t width = scaled(cols * pxd.h_mm), height = scaled(rows * pxd.w_mm);
    Point   p = Point::new_scale(c * pxd.w_mm, r * pxd.h_mm);
    // reverse the raster transformations
    if (tr.mirror_y)
        p.y() = height - p.y();
    if (tr.mirror_x)
        p.x() = width - p.x();
    p.x() -= tr.center_x;
    p.y() -= tr.center_y;
    if (tr.flipXY)
        std::swap(p.x(), p.y());
    return p;
}

// Get the size of a raster pixel in coord_t.
static Point rstPixel(const sla::RasterGrayscaleAA& rst)
{
    auto pxd = rst.pixel_dimensions();
    return Point::new_scale(pxd.w_mm, pxd.h_mm);
}

// Get the size of a raster in coord_t.
static Point rstSize(const sla::RasterGrayscaleAA& rst)
{
    auto pxd = rst.pixel_dimensions();
    auto res = rst.resolution();
    return Point::new_scale(pxd.w_mm * res.width_px, pxd.h_mm * res.height_px);
}

// Get the bounding box of a raster.
static BoundingBox rstBBox(const sla::RasterGrayscaleAA& rst)
{
    auto center = rst.trafo().get_center();
    return BoundingBox(Point(0, 0) - center, rstSize(rst) - center);
}

// Get the ExPolygons directly corresponding to a raster.
static ExPolygons rstGetPolys(sla::RasterGrayscaleAA& rst)
{
    size_t   rows = rst.resolution().height_px, cols = rst.resolution().width_px;
    Polygons polys;
    for (auto r = 0; r < rows; r++) {
        // use c0==cols as a sentinel marker for "no start column yet".
        size_t c0 = cols;
        for (auto c = 0; c <= cols; c++) {
            if (c < cols && rst.read_pixel(c, r) > 128) {
                // We have set pixels, set the c0 start column if it is not yet set.
                if (c0 == cols)
                    c0 = c;
            } else if (c0 < cols) {
                // There is no pixel set, but we do have a c0 start column. Output a
                // "row-rectangle" poly for this row between the start column c0 and
                // the current column.
                polys.push_back({rstPoint(rst, c0, r), rstPoint(rst, c0, r + 1), rstPoint(rst, c, r + 1), rstPoint(rst, c, r)});
                // Make sure the poly is anti-clockwise, which it might not be
                // depending on how rstPoint() reverses the raster transformations
                // from (c,r) to (x,y) coordinates.
                if (polys.back().is_clockwise())
                    polys.back().reverse();
                // Clear the start column c0 for the next row-rectangle.
                c0 = cols;
            }
        }
    }
    // Merge all the row-rectangle polys into contiguous raster ExPolygons.
    return union_ex(polys);
}

// Get the length in mm of a "vector" Point.
static double len(const Point& v) { return unscaled(v.norm()); }
// Get the area in mm^2 of a box with corners at the origin and a Point.
static double area(const Point& v) { return unscaled(v.x()) * unscaled(v.y()); }

// Find the index of the nearest extracted ExPolygon for a reference ExPolygon.
static int find_closest_ext(const ExPolygons& exts, ExPolygon ref)
{
    auto ref_center = ref.contour.bounding_box().center();

    auto closest = std::min_element(exts.begin(), exts.end(), [&ref_center](auto a, auto b) {
        auto a_center = a.contour.bounding_box().center();
        auto b_center = b.contour.bounding_box().center();
        return a_center.distance_to(ref_center) < b_center.distance_to(ref_center);
    });
    return std::distance(exts.begin(), closest);
}

static Slic3r::sla::RasterGrayscaleAA create_raster(const sla::Resolution& res, double disp_w = 100., double disp_h = 100.)
{
    sla::PixelDim pixdim{disp_w / res.width_px, disp_h / res.height_px};

    auto                   bb = BoundingBox({0, 0}, {scaled(disp_w), scaled(disp_h)});
    sla::RasterBase::Trafo trafo;
    trafo.center_x = bb.center().x();
    trafo.center_y = bb.center().y();

    return sla::RasterGrayscaleAA{res, pixdim, trafo, agg::gamma_threshold(.5)};
}

static ExPolygon square(double a, Point center = {0, 0})
{
    ExPolygon poly;
    coord_t   V = scaled(a / 2.);

    poly.contour.points = {{-V, -V}, {V, -V}, {V, V}, {-V, V}};
    poly.translate(center.x(), center.y());

    return poly;
}

static ExPolygon square_with_hole(double a, Point center = {0, 0})
{
    ExPolygon poly = square(a);

    poly.holes.emplace_back();
    coord_t V                 = scaled(a / 4.);
    poly.holes.front().points = {{-V, V}, {V, V}, {V, -V}, {-V, -V}};

    poly.translate(center.x(), center.y());

    return poly;
}

static ExPolygons circle_with_hole(double r, Point center = {0, 0})
{
    ExPolygon poly;

    std::vector<double> pis = linspace_vector(0., 2 * PI, 100);

    coord_t rs = scaled(r);
    for (double phi : pis) {
        poly.contour.points.emplace_back(rs * std::cos(phi), rs * std::sin(phi));
    }

    poly.holes.emplace_back(poly.contour);
    poly.holes.front().reverse();
    for (auto& p : poly.holes.front().points)
        p /= 2;

    poly.translate(center.x(), center.y());

    return {poly};
}

static const Vec2i32 W2x2 = {2, 2};
static const Vec2i32 W1x1 = {1, 1};

template<class Rst>
static void test_expolys(Rst&& rst, const ExPolygons& ref, Vec2i32 window, const std::string& name = "test", bool strict = true)
{
    auto   raster_bb  = rstBBox(rst);
    Point  pixel_size = rstPixel(rst);
    Point  window_size{coord_t(pixel_size.x() * window.x()), coord_t(pixel_size.y() * window.y())};
    double pixel_area  = area(pixel_size);
    double pixel_len   = len(pixel_size);
    double window_area = area(window_size);
    double window_len  = len(window_size);

    for (const ExPolygon& expoly : ref)
        rst.draw(expoly);

    std::fstream out(name + ".png", std::ios::out);
    out << rst.encode(sla::PNGRasterEncoder{});
    out.close();

    const ExPolygons bmp = rstGetPolys(rst);
    const ExPolygons ext = sla::raster_to_polygons(rst, window);

    SVG svg(name + ".svg", raster_bb);
    svg.draw(bmp, "green");
    if (pixel_size.x() >= scale_(0.5))
        svg.draw_grid(raster_bb, "grey", scale_(0.05), pixel_size.x());
    if (window_size.x() >= scale_(1.0))
        svg.draw_grid(raster_bb, "grey", scale_(0.10), window_size.x());
    svg.draw_outline(ref, "red", "red", scale_(0.3));
    svg.draw_outline(ext, "blue", "blue");
    svg.Close();

    // Note all these areas are unscaled back to mm^2.
    double raster_area    = unscaled(unscaled(area(bmp)));
    double reference_area = unscaled(unscaled(area(ref)));
    double extracted_area = unscaled(unscaled(area(ext)));

    // Note that errors accumulate with each step going from the reference
    // polys to the extracted polys. The rendering of the reference polys to
    // the raster does introduce pixelization errors too. This checks for
    // acceptable errors going from reference to raster, and raster to
    // reference.
    for (size_t i = 0; i < ref.size(); ++i) {
        if (ref[i].contour.size() < 20)
            UNSCOPED_INFO("reference ref[" << i << "]: " << ref[i]);
    }
    CHECK_THAT(raster_area, WithinRel(reference_area, pixel_len * 0.05) || WithinAbs(reference_area, pixel_area));
    for (size_t i = 0; i < ext.size(); ++i) {
        if (ext[i].contour.size() < 20)
            UNSCOPED_INFO("extracted ext[" << i << "]: " << ext[i]);
    }
    CHECK_THAT(extracted_area, WithinRel(raster_area, 0.05) || WithinAbs(raster_area, window_area));
    for (auto i = 0; i < ext.size(); ++i) {
        CHECK(ext[i].contour.is_counter_clockwise());
        for (auto& h : ext[i].holes)
            CHECK(h.is_clockwise());
    }

    BoundingBox ref_bb;
    for (auto& expoly : ref)
        ref_bb.merge(expoly.contour.bounding_box());
    BoundingBox ext_bb;
    for (auto& expoly : ext)
        ext_bb.merge(expoly.contour.bounding_box());
    CHECK(len(ext_bb.center() - ref_bb.center()) < pixel_len);

    // In ambigous cases (when polygons just touch) there are multiple equally
    // valid interpretations of the raster into polygons. Although
    // MarchingSquares currently systematically selects the solution that
    // breaks them into separate polygons, that might not always be true. Also,
    // SLA/RasterToPolygons.hpp, and in particular union_ex() from
    // ClipperUtils.hpp that it uses, can and does sometimes merge them back
    // together. This means we cannot reliably make assertions about the
    // extracted number of polygons and their shapes in these cases. So we skip
    // the individual polygon checks for strict=false.
    if (strict) {
        CHECK(ext.size() == ref.size());
        for (auto i = 0; i < ext.size(); ++i) {
            auto j = find_closest_ext(ref, ext[i]);
            INFO("Comparing ext[" << i << "] against closest ref[" << j << "]");
            CHECK(ext[i].holes.size() == ref[j].holes.size());
            double ext_i_area = unscaled(unscaled(ext[i].area()));
            double ref_j_area = unscaled(unscaled(ref[j].area()));
            CHECK_THAT(ext_i_area, WithinRel(ref_j_area, pixel_len * 0.05) || WithinAbs(ref_j_area, window_area));
            auto ext_i_bb = ext[i].contour.bounding_box();
            auto ref_j_bb = ref[j].contour.bounding_box();
            CHECK(len(ext_i_bb.center() - ref_j_bb.center()) < pixel_len);
        }
    }
}

TEST_CASE("Empty raster should result in empty polygons", "[MarchingSquares]")
{
    sla::RasterGrayscaleAAGammaPower rst{{}, {}, {}};
    ExPolygons                       extracted = sla::raster_to_polygons(rst);
    REQUIRE(extracted.size() == 0);
}

TEST_CASE("Marching squares directions", "[MarchingSquares]")
{
    using namespace marchsq;
    Coord crd{0, 0};

    __impl::step(crd, __impl::Dir::left);
    CHECK(crd == Coord(0, -1));
    __impl::step(crd, __impl::Dir::down);
    CHECK(crd == Coord(1, -1));
    __impl::step(crd, __impl::Dir::right);
    CHECK(crd == Coord(1, 0));
    __impl::step(crd, __impl::Dir::up);
    CHECK(crd == Coord(0, 0));
    __impl::step(crd, __impl::Dir::left, 7);
    CHECK(crd == Coord(0, -7));
    __impl::step(crd, __impl::Dir::down, 7);
    CHECK(crd == Coord(7, -7));
    __impl::step(crd, __impl::Dir::right, 7);
    CHECK(crd == Coord(7, 0));
    __impl::step(crd, __impl::Dir::up, 7);
    CHECK(crd == Coord(0, 0));
    __impl::step(crd, __impl::Dir::left, -3);
    CHECK(crd == Coord(0, 3));
    __impl::step(crd, __impl::Dir::down, -3);
    CHECK(crd == Coord(-3, 3));
    __impl::step(crd, __impl::Dir::right, -3);
    CHECK(crd == Coord(-3, 0));
    __impl::step(crd, __impl::Dir::up, -3);
    CHECK(crd == Coord(0, 0));
}

TEST_CASE("Fully covered raster should result in a rectangle", "[MarchingSquares]")
{
    auto rst = create_raster({4, 4}, 4., 4.);

    ExPolygon rect = square(4);

    SECTION("Full accuracy") { test_expolys(rst, {rect}, W1x1, "fully_covered_full_acc"); }

    SECTION("Half accuracy") { test_expolys(rst, {rect}, W2x2, "fully_covered_half_acc"); }
}

TEST_CASE("4x4 raster with one ring", "[MarchingSquares]")
{
    sla::PixelDim pixdim{1, 1};

    // We need one additional row and column to detect edges
    sla::RasterGrayscaleAA rst{{4, 4}, pixdim, {}, agg::gamma_threshold(.5)};

    ExPolygons one = {{{1, 1}, {3, 1}, {3, 3}, {2, 3}, {2, 2}, {1, 2}}};
    for (ExPolygon& p : one)
        p.scale(scaled(1.0));
    test_expolys(rst, one, W1x1, "one_4x4");
}

TEST_CASE("10x10 raster with two rings", "[MarchingSquares]")
{
    sla::PixelDim pixdim{1, 1};

    // We need one additional row and column to detect edges
    sla::RasterGrayscaleAA rst{{10, 10}, pixdim, {}, agg::gamma_threshold(.5)};

    SECTION("Ambiguous case with 'bd' square")
    {
        ExPolygons ac = {{{1, 1}, {3, 1}, {3, 2}, {2, 2}, {2, 3}, {1, 3}}, {{4, 4}, {2, 4}, {2, 3}, {3, 3}, {3, 2}, {4, 2}}};
        for (ExPolygon& p : ac)
            p.scale(scaled(2.0));
        test_expolys(rst, ac, W1x1, "bd_10x10", false);
    }

    SECTION("Ambiguous case with 'ac' square")
    {
        ExPolygons bd = {{{1, 4}, {1, 2}, {2, 2}, {2, 3}, {3, 3}, {3, 4}}, {{4, 1}, {4, 3}, {3, 3}, {3, 2}, {2, 2}, {2, 1}}};
        for (ExPolygon& p : bd)
            p.scale(scaled(2.0));
        test_expolys(rst, bd, W1x1, "ac_10x10", false);
    }
}

TEST_CASE("Square with hole in the middle", "[MarchingSquares]")
{
    using namespace Slic3r;

    ExPolygons inp = {square_with_hole(50.)};

    SECTION("Proportional raster, 1x1 mm pixel size, full accuracy")
    {
        test_expolys(create_raster({100, 100}, 100., 100.), inp, W1x1, "square_with_hole_proportional_1x1_mm_px_full");
    }

    SECTION("Proportional raster, 1x1 mm pixel size, half accuracy")
    {
        test_expolys(create_raster({100, 100}, 100., 100.), inp, W2x2, "square_with_hole_proportional_1x1_mm_px_half");
    }

    SECTION("Landscape raster, 1x1 mm pixel size, full accuracy")
    {
        test_expolys(create_raster({150, 100}, 150., 100.), inp, W1x1, "square_with_hole_landsc_1x1_mm_px_full");
    }

    SECTION("Landscape raster, 1x1 mm pixel size, half accuracy")
    {
        test_expolys(create_raster({150, 100}, 150., 100.), inp, W2x2, "square_with_hole_landsc_1x1_mm_px_half");
    }

    SECTION("Portrait raster, 1x1 mm pixel size, full accuracy")
    {
        test_expolys(create_raster({100, 150}, 100., 150.), inp, W1x1, "square_with_hole_portrait_1x1_mm_px_full");
    }

    SECTION("Portrait raster, 1x1 mm pixel size, half accuracy")
    {
        test_expolys(create_raster({100, 150}, 100., 150.), inp, W2x2, "square_with_hole_portrait_1x1_mm_px_half");
    }

    SECTION("Proportional raster, 2x2 mm pixel size, full accuracy")
    {
        test_expolys(create_raster({50, 50}, 100., 100.), inp, W1x1, "square_with_hole_proportional_2x2_mm_px_full");
    }

    SECTION("Proportional raster, 2x2 mm pixel size, half accuracy")
    {
        test_expolys(create_raster({50, 50}, 100., 100.), inp, W2x2, "square_with_hole_proportional_2x2_mm_px_half");
    }

    SECTION("Proportional raster, 0.5x0.5 mm pixel size, full accuracy")
    {
        test_expolys(create_raster({200, 200}, 100., 100.), inp, W1x1, "square_with_hole_proportional_0.5x0.5_mm_px_full");
    }

    SECTION("Proportional raster, 0.5x0.5 mm pixel size, half accuracy")
    {
        test_expolys(create_raster({200, 200}, 100., 100.), inp, W2x2, "square_with_hole_proportional_0.5x0.5_mm_px_half");
    }
}

TEST_CASE("Circle with hole in the middle", "[MarchingSquares]")
{
    using namespace Slic3r;

    test_expolys(create_raster({1000, 1000}), circle_with_hole(25.), W1x1, "circle_with_hole");
}

static void recreate_object_from_rasters(const std::string& objname, float lh)
{
    TriangleMesh mesh = load_model(objname);

    auto  bb = mesh.bounding_box();
    Vec3f tr = -bb.center().cast<float>();
    mesh.translate(tr.x(), tr.y(), tr.z());
    bb = mesh.bounding_box();

    std::vector<ExPolygons> layers = slice_mesh_ex(mesh.its, grid(float(bb.min.z()) + lh, float(bb.max.z()), lh));

    sla::Resolution res{2560, 1440};
    double          disp_w = 120.96;
    double          disp_h = 68.04;

#ifndef NDEBUG
    size_t cntr = 0;
#endif
    for (ExPolygons& layer : layers) {
        auto rst = create_raster(res, disp_w, disp_h);

        for (ExPolygon& island : layer) {
            rst.draw(island);
        }

#ifndef NDEBUG
        std::fstream out(objname + std::to_string(cntr) + ".png", std::ios::out);
        out << rst.encode(sla::PNGRasterEncoder{});
        out.close();
#endif

        ExPolygons layer_ = sla::raster_to_polygons(rst);
        //        float delta = scaled(std::min(rst.pixel_dimensions().h_mm,
        //                                      rst.pixel_dimensions().w_mm)) / 2;

        //        layer_ = expolygons_simplify(layer_, delta);

#ifndef NDEBUG
        SVG svg(objname + std::to_string(cntr) + ".svg", rstBBox(rst));
        svg.draw(layer_);
        svg.draw(layer, "green");
        svg.Close();
#endif

        double layera = 0., layera_ = 0.;
        for (auto& p : layer)
            layera += p.area();
        for (auto& p : layer_)
            layera_ += p.area();
#ifndef NDEBUG
        std::cout << cntr++ << std::endl;
#endif
        double diff = std::abs(layera_ - layera);
        REQUIRE((diff <= 0.1 * layera || diff < scaled<double>(1.) * scaled<double>(1.)));

        layer = std::move(layer_);
    }

    indexed_triangle_set out = slices_to_mesh(layers, bb.min.z(), double(lh), double(lh));

    its_write_obj(out, "out_from_rasters.obj");
}

TEST_CASE("Recreate object from rasters", "[SL1Import]") { recreate_object_from_rasters("frog_legs.obj", 0.05f); }

namespace marchsq {

static constexpr float  layerf = 0.20;  // layer height in mm (used for z values).
static constexpr float  gsizef = 100.0; // grid size in mm (box volume side length).
static constexpr float  wsizef = 0.50;  // grid window size in mm (roughly line segment length).
static constexpr float  psizef = 0.01;  // raster pixel size in mm (roughly point accuracy).
static constexpr float  isoval = 0.0;   // iso value threshold to use.
static const     long   wsize  = std::round(wsizef / psizef);

static float period = 10.0;            // gyroid "wavelength" in mm (2x line spacing).
static float freq   = 2 * PI / period; // gyroid frequency in waves per mm.

void set_period(float len = 10.0)
{
    period = len;
    freq   = 2 * PI / period;
}

static size_t layer_n;
static size_t ring_n;
static size_t point_n;
static size_t get_n;

void reset_stats()
{
    layer_n = 0;
    ring_n  = 0;
    point_n = 0;
    get_n   = 0;
}

using Rings = std::vector<Ring>;

template<> struct _RasterTraits<size_t>
{
    // using Rst = Slic3r::sla::RasterGrayscaleAA;
    //  The type of pixel cell in the raster
    using ValueType = float;

    // Value at a given position
    static float get(const size_t& layer, size_t row, size_t col)
    {
        get_n++;
        const float x = col * psizef * freq;
        const float y = row * psizef * freq;
        const float z = layer * psizef * freq;

        return sinf(x) * cosf(y) + sinf(y) * cosf(z) + sinf(z) * cosf(x);
    }

    // Number of rows and cols of the raster
    static size_t rows(const size_t& layer) { return std::round(gsizef / psizef); }
    static size_t cols(const size_t& layer) { return std::round(gsizef / psizef); }
};

Rings get_gyroids(size_t l)
{
    size_t layer = l;
    Rings  rings = execute(layer, isoval, {wsize, wsize});
    layer_n++;
    ring_n += rings.size();
    for (auto r : rings)
        point_n += r.size();
    return rings;
}

}; // namespace marchsq

void benchmark_gyroid(float period)
{
    marchsq::reset_stats();
    marchsq::set_period(period);
    INFO("grid size: " << marchsq::gsizef << "mm\nlayer height: " << marchsq::layerf << "mm\n");
    INFO("window size: " << marchsq::wsizef << "mm\npoint size: " << marchsq::psizef << "mm\n");
    INFO("gyroid period: " << marchsq::period << "mm\n");
    BENCHMARK("indexed", i) { return marchsq::get_gyroids(i); };
    INFO("output avg rings/layer: " << float(marchsq::ring_n) / float(marchsq::layer_n) << "\n");
    INFO("output avg points/layer: " << float(marchsq::point_n) / float(marchsq::layer_n) << "\n");
    INFO("output avg gets/layer: " << float(marchsq::get_n) / float(marchsq::layer_n) << "\n");

    REQUIRE(marchsq::layer_n > 0);
}

TEST_CASE("Benchmark gyroid cube period 10.0mm", "[MarchingSquares]") { benchmark_gyroid(10.0); }

TEST_CASE("Benchmark gyroid cube period 5.0mm", "[MarchingSquares]") { benchmark_gyroid(5.0); }
