#include "sla_test_utils.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/SLA/AGGRaster.hpp"

#include <iomanip>

void test_support_model_collision(const std::string          &obj_filename,
                                  const sla::SupportTreeConfig   &input_supportcfg,
                                  const sla::HollowingConfig &hollowingcfg,
                                  const sla::DrainHoles      &drainholes)
{
    SupportByproducts byproducts;
    
    sla::SupportTreeConfig supportcfg = input_supportcfg;
    
    // Set head penetration to a small negative value which should ensure that
    // the supports will not touch the model body.
    supportcfg.head_penetration_mm = -0.15;
    
    test_supports(obj_filename, supportcfg, hollowingcfg, drainholes, byproducts);
    
    // Slice the support mesh given the slice grid of the model.
    std::vector<ExPolygons> support_slices =
            byproducts.supporttree.slice(byproducts.slicegrid, CLOSING_RADIUS);
    
    // The slices originate from the same slice grid so the numbers must match
    
    bool support_mesh_is_empty =
            byproducts.supporttree.retrieve_mesh(sla::MeshType::Pad).empty() &&
            byproducts.supporttree.retrieve_mesh(sla::MeshType::Support).empty();
    
    if (support_mesh_is_empty)
        REQUIRE(support_slices.empty());
    else
        REQUIRE(support_slices.size() == byproducts.model_slices.size());
    
    bool notouch = true;
    for (size_t n = 0; notouch && n < support_slices.size(); ++n) {
        const ExPolygons &sup_slice = support_slices[n];
        const ExPolygons &mod_slice = byproducts.model_slices[n];
        
        Polygons intersections = intersection(sup_slice, mod_slice);
        
        double pinhead_r  = scaled(input_supportcfg.head_front_radius_mm);

        // TODO:: make it strict without a threshold of PI * pihead_radius ^ 2
        notouch = notouch && area(intersections) < PI * pinhead_r * pinhead_r;
    }
    
    /*if (!notouch) */export_failed_case(support_slices, byproducts);
    
    REQUIRE(notouch);
}

void export_failed_case(const std::vector<ExPolygons> &support_slices, const SupportByproducts &byproducts)
{
    for (size_t n = 0; n < support_slices.size(); ++n) {
        const ExPolygons &sup_slice = support_slices[n];
        const ExPolygons &mod_slice = byproducts.model_slices[n];
        Polygons intersections = intersection(sup_slice, mod_slice);
        
        std::stringstream ss;
        if (!intersections.empty()) {
            ss << byproducts.obj_fname << std::setprecision(4) << n << ".svg";
            SVG svg(ss.str());
            svg.draw(sup_slice, "green");
            svg.draw(mod_slice, "blue");
            svg.draw(intersections, "red");
            svg.Close();
        }
    }

    indexed_triangle_set its;
    byproducts.supporttree.retrieve_full_mesh(its);
    TriangleMesh m{its};
    m.merge(byproducts.input_mesh);
    m.WriteOBJFile((Catch::getResultCapture().getCurrentTestName() + "_" +
                    byproducts.obj_fname).c_str());
}

void test_supports(const std::string          &obj_filename,
                   const sla::SupportTreeConfig   &supportcfg,
                   const sla::HollowingConfig &hollowingcfg,
                   const sla::DrainHoles      &drainholes,
                   SupportByproducts          &out)
{
    using namespace Slic3r;
    TriangleMesh mesh = load_model(obj_filename);
    
    REQUIRE_FALSE(mesh.empty());
    
    if (hollowingcfg.enabled) {
        sla::InteriorPtr interior = sla::generate_interior(mesh, hollowingcfg);
        REQUIRE(interior);
        mesh.merge(TriangleMesh{sla::get_mesh(*interior)});
    }
    
    auto   bb      = mesh.bounding_box();
    double zmin    = bb.min.z();
    double zmax    = bb.max.z();
    double gnd     = zmin - supportcfg.object_elevation_mm;
    auto   layer_h = 0.05f;
    
    out.slicegrid = grid(float(gnd), float(zmax), layer_h);
    out.model_slices = slice_mesh_ex(mesh.its, out.slicegrid, CLOSING_RADIUS);
    sla::cut_drainholes(out.model_slices, out.slicegrid, CLOSING_RADIUS, drainholes, []{});
    
    // Create the special index-triangle mesh with spatial indexing which
    // is the input of the support point and support mesh generators
    sla::IndexedMesh emesh{mesh};

#ifdef SLIC3R_HOLE_RAYCASTER
    if (hollowingcfg.enabled) 
        emesh.load_holes(drainholes);
#endif

    // TODO: do the cgal hole cutting...
    
    // Create the support point generator
    sla::SupportPointGenerator::Config autogencfg;
    autogencfg.head_diameter = float(2 * supportcfg.head_front_radius_mm);
    sla::SupportPointGenerator point_gen{emesh, autogencfg, [] {}, [](int) {}};
    
    point_gen.seed(0); // Make the test repeatable
    point_gen.execute(out.model_slices, out.slicegrid);
    
    // Get the calculated support points.
    std::vector<sla::SupportPoint> support_points = point_gen.output();
    
    int validityflags = ASSUME_NO_REPAIR;
    
    // If there is no elevation, support points shall be removed from the
    // bottom of the object.
    if (std::abs(supportcfg.object_elevation_mm) < EPSILON) {
        sla::remove_bottom_points(support_points, zmin + supportcfg.base_height_mm);
    } else {
        // Should be support points at least on the bottom of the model
        REQUIRE_FALSE(support_points.empty());
        
        // Also the support mesh should not be empty.
        validityflags |= ASSUME_NO_EMPTY;
    }
    
    // Generate the actual support tree
    sla::SupportTreeBuilder treebuilder;
    sla::SupportableMesh    sm{emesh, support_points, supportcfg};
    sla::SupportTreeBuildsteps::execute(treebuilder, sm);
    
    check_support_tree_integrity(treebuilder, supportcfg);
    
    TriangleMesh output_mesh{treebuilder.retrieve_mesh(sla::MeshType::Support)};
    
    check_validity(output_mesh, validityflags);
    
    // Quick check if the dimensions and placement of supports are correct
    auto obb = output_mesh.bounding_box();
    
    double allowed_zmin = zmin - supportcfg.object_elevation_mm;
    
    if (std::abs(supportcfg.object_elevation_mm) < EPSILON)
        allowed_zmin = zmin - 2 * supportcfg.head_back_radius_mm;
    
    REQUIRE(obb.min.z() >= Catch::Approx(allowed_zmin));
    REQUIRE(obb.max.z() <= Catch::Approx(zmax));
    
    // Move out the support tree into the byproducts, we can examine it further
    // in various tests.
    out.obj_fname   = std::move(obj_filename);
    out.supporttree = std::move(treebuilder);
    out.input_mesh  = std::move(mesh);
}

void check_support_tree_integrity(const sla::SupportTreeBuilder &stree, 
                                  const sla::SupportTreeConfig &cfg)
{
    double gnd  = stree.ground_level;
    double H1   = cfg.max_solo_pillar_height_mm;
    double H2   = cfg.max_dual_pillar_height_mm;
    
    for (const sla::Head &head : stree.heads()) {
        REQUIRE((!head.is_valid() || head.pillar_id != sla::SupportTreeNode::ID_UNSET ||
                head.bridge_id != sla::SupportTreeNode::ID_UNSET));
    }
    
    for (const sla::Pillar &pillar : stree.pillars()) {
        if (std::abs(pillar.endpoint().z() - gnd) < EPSILON) {
            double h = pillar.height;
            
            if (h > H1) REQUIRE(pillar.links >= 1);
            else if(h > H2) { REQUIRE(pillar.links >= 2); }
        }
        
        REQUIRE(pillar.links <= cfg.pillar_cascade_neighbors);
        REQUIRE(pillar.bridges <= cfg.max_bridges_on_pillar);
    }
    
    double max_bridgelen = 0.;
    auto chck_bridge = [&cfg](const sla::Bridge &bridge, double &max_brlen) {
        Vec3d n = bridge.endp - bridge.startp;
        double d = sla::distance(n);
        max_brlen = std::max(d, max_brlen);
        
        double z     = n.z();
        double polar = std::acos(z / d);
        double slope = -polar + PI / 2.;
        REQUIRE(std::abs(slope) >= cfg.bridge_slope - EPSILON);
    };
    
    for (auto &bridge : stree.bridges()) chck_bridge(bridge, max_bridgelen);
    REQUIRE(max_bridgelen <= Catch::Approx(cfg.max_bridge_length_mm));
    
    max_bridgelen = 0;
    for (auto &bridge : stree.crossbridges()) chck_bridge(bridge, max_bridgelen);
    
    double md = cfg.max_pillar_link_distance_mm / std::cos(-cfg.bridge_slope);
    REQUIRE(max_bridgelen <= md);
}

void test_pad(const std::string &obj_filename, const sla::PadConfig &padcfg, PadByproducts &out)
{
    REQUIRE(padcfg.validate().empty());
    
    TriangleMesh mesh = load_model(obj_filename);
    
    REQUIRE_FALSE(mesh.empty());
    
    // Create pad skeleton only from the model
    Slic3r::sla::pad_blueprint(mesh.its, out.model_contours);
    
    test_concave_hull(out.model_contours);
    
    REQUIRE_FALSE(out.model_contours.empty());
    
    // Create the pad geometry for the model contours only
    indexed_triangle_set out_its;
    Slic3r::sla::create_pad({}, out.model_contours, out_its, padcfg);
    out.mesh = TriangleMesh{out_its};
    
    check_validity(out.mesh);
    
    auto bb = out.mesh.bounding_box();
    REQUIRE(bb.max.z() - bb.min.z() == Catch::Approx(padcfg.full_height()));
}

static void _test_concave_hull(const Polygons &hull, const ExPolygons &polys)
{
    REQUIRE(polys.size() >=hull.size());
    
    double polys_area = 0;
    for (const ExPolygon &p : polys) polys_area += p.area();
    
    double cchull_area = 0;
    for (const Slic3r::Polygon &p : hull) cchull_area += p.area();
    
    REQUIRE(cchull_area >= Catch::Approx(polys_area));
    
    size_t cchull_holes = 0;
    for (const Slic3r::Polygon &p : hull)
        cchull_holes += p.is_clockwise() ? 1 : 0;
    
    REQUIRE(cchull_holes == 0);
    
    Polygons intr = diff(to_polygons(polys), hull);
    REQUIRE(intr.empty());
}

void test_concave_hull(const ExPolygons &polys) {
    sla::PadConfig pcfg;
    
    Slic3r::sla::ConcaveHull cchull{polys, pcfg.max_merge_dist_mm, []{}};
    
    _test_concave_hull(cchull.polygons(), polys);
    
    coord_t delta = scaled(pcfg.brim_size_mm + pcfg.wing_distance());
    ExPolygons wafflex = sla::offset_waffle_style_ex(cchull, delta);
    Polygons waffl = sla::offset_waffle_style(cchull, delta);
    
    _test_concave_hull(to_polygons(wafflex), polys);
    _test_concave_hull(waffl, polys);
}

//FIXME this functionality is gone after TriangleMesh refactoring to get rid of admesh.
void check_validity(const TriangleMesh &input_mesh, int flags)
{
    /*
    TriangleMesh mesh{input_mesh};
    
    if (flags & ASSUME_NO_EMPTY) {
        REQUIRE_FALSE(mesh.empty());
    } else if (mesh.empty())
        return; // If it can be empty and it is, there is nothing left to do.
    
    bool do_update_shared_vertices = false;
    mesh.repair(do_update_shared_vertices);
    
    if (flags & ASSUME_NO_REPAIR) {
        REQUIRE_FALSE(mesh.repaired());
    }
    
    if (flags & ASSUME_MANIFOLD) {
        if (!mesh.is_manifold()) mesh.WriteOBJFile("non_manifold.obj");
        REQUIRE(mesh.is_manifold());
    }
    */
}

void check_raster_transformations(sla::RasterBase::Orientation o, sla::RasterBase::TMirroring mirroring)
{
    double disp_w = 120., disp_h = 68.;
    sla::Resolution res{2560, 1440};
    sla::PixelDim pixdim{disp_w / res.width_px, disp_h / res.height_px};
    
    auto bb = BoundingBox({0, 0}, {scaled(disp_w), scaled(disp_h)});
    sla::RasterBase::Trafo trafo{o, mirroring};
    trafo.center_x = bb.center().x();
    trafo.center_y = bb.center().y();
    double gamma = 1.;
    
    sla::RasterGrayscaleAAGammaPower raster{res, pixdim, trafo, gamma};
    
    // create box of size 32x32 pixels (not 1x1 to avoid antialiasing errors)
    coord_t pw = 32 * coord_t(std::ceil(scaled<double>(pixdim.w_mm)));
    coord_t ph = 32 * coord_t(std::ceil(scaled<double>(pixdim.h_mm)));
    ExPolygon box;
    box.contour.points = {{-pw, -ph}, {pw, -ph}, {pw, ph}, {-pw, ph}};
    
    double tr_x = scaled<double>(20.), tr_y = tr_x;
    
    box.translate(tr_x, tr_y);
    ExPolygon expected_box = box;
    
    // Now calculate the position of the translated box according to output
    // trafo.
    if (o == sla::RasterBase::Orientation::roPortrait) expected_box.rotate(PI / 2.);
    
    if (mirroring[X])
        for (auto &p : expected_box.contour.points) p.x() = -p.x();
    
    if (mirroring[Y])
        for (auto &p : expected_box.contour.points) p.y() = -p.y();
    
    raster.draw(box);
    
    Point expected_coords = expected_box.contour.bounding_box().center();
    double rx = unscaled(expected_coords.x() + bb.center().x()) / pixdim.w_mm;
    double ry = unscaled(expected_coords.y() + bb.center().y()) / pixdim.h_mm;
    auto w = size_t(std::floor(rx));
    auto h = res.height_px - size_t(std::floor(ry));
    
    REQUIRE((w < res.width_px && h < res.height_px));
    
    auto px = raster.read_pixel(w, h);
    
    if (px != FullWhite) {
        std::fstream outf("out.png", std::ios::out);
        
        outf << raster.encode(sla::PNGRasterEncoder());
    }
    
    REQUIRE(px == FullWhite);
}

ExPolygon square_with_hole(double v)
{
    ExPolygon poly;
    coord_t V = scaled(v / 2.);
    
    poly.contour.points = {{-V, -V}, {V, -V}, {V, V}, {-V, V}};
    poly.holes.emplace_back();
    V = V / 2;
    poly.holes.front().points = {{-V, V}, {V, V}, {V, -V}, {-V, -V}};
    return poly;
}

long raster_pxsum(const sla::RasterGrayscaleAA &raster)
{
    auto res = raster.resolution();
    long a = 0;
    
    for (size_t x = 0; x < res.width_px; ++x)
        for (size_t y = 0; y < res.height_px; ++y)
            a += raster.read_pixel(x, y);
        
    return a;
}

double raster_white_area(const sla::RasterGrayscaleAA &raster)
{
    if (raster.resolution().pixels() == 0) return std::nan("");
    
    auto res = raster.resolution();
    double a = 0;
    
    for (size_t x = 0; x < res.width_px; ++x)
        for (size_t y = 0; y < res.height_px; ++y) {
            auto px = raster.read_pixel(x, y);
            a += pixel_area(px, raster.pixel_dimensions());
        }
    
    return a;
}

double predict_error(const ExPolygon &p, const sla::PixelDim &pd)
{
    auto lines = p.lines();
    double pix_err = pixel_area(FullWhite, pd)  / 2.;
    
    // Worst case is when a line is parallel to the shorter axis of one pixel,
    // when the line will be composed of the max number of pixels
    double pix_l = std::min(pd.h_mm, pd.w_mm);
    
    double error = 0.;
    for (auto &l : lines)
        error += (unscaled(l.length()) / pix_l) * pix_err;
    
    return error;
}

sla::SupportPoints calc_support_pts(
    const TriangleMesh &                      mesh,
    const sla::SupportPointGenerator::Config &cfg)
{
    // Prepare the slice grid and the slices
    auto                    bb      = cast<float>(mesh.bounding_box());
    std::vector<float>      heights = grid(bb.min.z(), bb.max.z(), 0.1f);
    std::vector<ExPolygons> slices  = slice_mesh_ex(mesh.its, heights, CLOSING_RADIUS);

    // Prepare the support point calculator
    sla::IndexedMesh emesh{mesh};
    sla::SupportPointGenerator spgen{emesh, cfg, []{}, [](int){}};

    // Calculate the support points
    spgen.seed(0);
    spgen.execute(slices, heights);

    return spgen.output();
}
