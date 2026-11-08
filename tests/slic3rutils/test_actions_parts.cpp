// Automation action tests. Owner: work package S3 (parts). Tag: [actions].
//
// The part / mesh / text descriptors (PartActions.cpp, MeshActions.cpp, TextActions.cpp) and the
// pure helpers behind their runners (PartsCommon.hpp). Runners need the running application and
// are covered by tools/bridge_e2e/cases/parts.yaml.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/CutPlaneRules.hpp"
#include "slic3r/GUI/Actions/PartsCommon.hpp"
#include "slic3r/GUI/Actions/Register.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::Actions;
using json = nlohmann::json;
using Catch::Matchers::WithinAbs;
namespace fs = boost::filesystem;

namespace {

const Catalog& s3_catalog()
{
    static Catalog c;
    static bool    done = false;
    if (!done) {
        register_part_actions(c);
        register_mesh_actions(c);
        register_text_actions(c);
        done = true;
    }
    return c;
}

const std::vector<std::string> S3_IDS = {
    "part.add",          "part.setType",      "part.setFilament",  "part.setTransform",   "part.splitToParts",
    "part.mergeToSinglePart", "part.replace", "object.addShape",   "object.addHandyModel", "part.boolean",
    "part.simplify",     "part.smooth",       "part.fix",          "object.cut",          "object.invalidateCutInfo",
    "object.skeletonCutPlanes", "text.add",   "text.edit",         "text.listFonts",      "svg.add",
    "svg.edit",          "object.cancelSkeleton",
};

fs::path part_manifest() { return fs::path(PROFILES_DIR).parent_path() / "actions" / "surface" / "part.json"; }

json read_json(const fs::path& p)
{
    std::ifstream     in(p.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

bool close_to(const Vec3d& a, const Vec3d& b, double eps = 1e-6) { return (a - b).norm() < eps; }

} // namespace

TEST_CASE("S3 actions are all registered with runners and examples", "[actions][parts]")
{
    const Catalog& c = s3_catalog();
    for (const std::string& id : S3_IDS) {
        INFO(id);
        const ActionEntry* e = c.find(id);
        REQUIRE(e != nullptr);
        CHECK(bool(e->runner));
        CHECK(e->descriptor.bridge_method.empty());
        CHECK(!e->descriptor.summary.empty());
        REQUIRE(e->descriptor.examples.is_array());
        CHECK(e->descriptor.examples.size() >= 1);
    }
    CHECK(c.size() == S3_IDS.size());
}

TEST_CASE("S3 examples validate against their own schemas", "[actions][parts]")
{
    for (const ActionEntry& e : s3_catalog().entries()) {
        for (const json& ex : e.descriptor.examples) {
            INFO(e.descriptor.id << " " << ex.dump());
            SchemaError err;
            const json  params = ex.contains("params") ? ex["params"] : json::object();
            CHECK(validate(e.descriptor.params, params, err));
            CHECK(err.path.empty());
            // Preconditions the generic E2E test runs first (and undoes): S3 actions, valid params.
            for (const json& st : ex.value("setup", json::array())) {
                const ActionEntry* pre = s3_catalog().find(st.value("id", ""));
                REQUIRE(pre != nullptr);
                SchemaError perr;
                CHECK(validate(pre->descriptor.params, st.value("params", json::object()), perr));
            }
        }
    }
}

TEST_CASE("S3 schemas reject what they must", "[actions][parts]")
{
    const Catalog& c = s3_catalog();
    SchemaError    err;

    const json& add = c.find("part.add")->descriptor.params;
    CHECK_FALSE(validate(add, {{"type", "lid"}}, err));
    CHECK_FALSE(validate(add, {{"sizeMm", json::array()}}, err));
    CHECK_FALSE(validate(add, {{"sizeMm", {1, 2, 3, 4}}}, err));
    CHECK_FALSE(validate(add, {{"positionMm", {1, 2}}}, err));
    CHECK_FALSE(validate(add, {{"bogus", 1}}, err));

    // simplify: exactly one of ratio / triangles / detail / maxError
    const json& simp = c.find("part.simplify")->descriptor.params;
    CHECK(validate(simp, {{"ratio", 0.5}}, err));
    CHECK(validate(simp, {{"detail", "low"}}, err));
    CHECK_FALSE(validate(simp, json::object(), err));
    CHECK_FALSE(validate(simp, {{"ratio", 0.5}, {"triangles", 100}}, err));
    CHECK_FALSE(validate(simp, {{"detail", "low"}, {"maxError", 0.2}}, err));
    CHECK_FALSE(validate(simp, {{"detail", "ultra"}}, err));

    // boolean keeps the tool unless told otherwise, as the tool's "Delete input" defaults to off
    CHECK(c.find("part.boolean")->descriptor.params["properties"]["keepTool"]["default"] == true);

    // cut: exactly one of z / plane; connectors are objects with a position
    const json& cut = c.find("object.cut")->descriptor.params;
    CHECK(validate(cut, {{"z", 5}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"plane", {{"point", {0, 0, 5}}, {"normal", {0, 0, 1}}}}}, err));
    CHECK_FALSE(validate(cut, {{"plane", {{"point", {0, 0, 5}}}}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"keep", "middle"}}, err));
    CHECK(validate(cut, {{"z", 5}, {"connectors", json::array({json::object()})}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"connectors", json::array({{{"type", "nail"}}})}}, err));
    CHECK(validate(cut, {{"z", 5}, {"mode", "dovetail"}, {"groove", {{"depthMm", 2}, {"flapAngleDeg", 45}, {"count", 3}}}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"groove", {{"flapAngleDeg", 10}}}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"groove", {{"angleDeg", 20}}}}, err));
    CHECK(validate(cut, {{"z", 5}, {"assign", json::array({{{"pointMm", {1, 2, 3}}, {"side", "lower"}}})}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"assign", json::array({{{"pointMm", {1, 2, 3}}}})}}, err));
    CHECK(validate(cut, {{"z", 5}, {"connectors", json::array({{{"type", "snap"}, {"bulgePct", 10}, {"spacePct", 30}, {"rotationDeg", 45}}})}}, err));
    CHECK_FALSE(validate(cut, {{"z", 5}, {"connectors", json::array({{{"rotationDeg", 200}}})}}, err));

    // skeleton: the tool's slider ranges
    const json& skel = c.find("object.skeletonCutPlanes")->descriptor.params;
    CHECK(validate(skel, {{"contractionRate", 0.5}, {"voxelResolution", 128}, {"traceBores", "off"}, {"cutAt", 2}}, err));
    CHECK_FALSE(validate(skel, {{"voxelResolution", 16}}, err));
    CHECK_FALSE(validate(skel, {{"bedRoot", "side"}}, err));

    // text: text required, emboss types only
    const json& text = c.find("text.add")->descriptor.params;
    CHECK_FALSE(validate(text, {{"heightMm", 5}}, err));
    CHECK_FALSE(validate(text, {{"text", "A"}, {"type", "blocker"}}, err));
    CHECK(validate(text, {{"text", "A"}, {"side", "top"}, {"offsetMm", {1, 2}}}, err));
}

TEST_CASE("S3 risk, targets and long-running flags", "[actions][parts]")
{
    const Catalog& c = s3_catalog();
    for (const ActionEntry& e : c.entries()) {
        INFO(e.descriptor.id);
        if (e.descriptor.id == "text.listFonts")
            CHECK(e.descriptor.risk == Risk::Read);
        else if (e.descriptor.id == "object.cancelSkeleton")
            CHECK(e.descriptor.risk == Risk::Ui); // stops a computation; changes nothing in the model
        else
            CHECK(e.descriptor.risk == Risk::Mutate); // every model edit is undoable: no confirmation
        CHECK(e.descriptor.gate == Gate::Settings);
    }
    for (const char* id : {"part.simplify", "part.smooth", "part.fix", "part.replace", "svg.add", "object.skeletonCutPlanes"})
        CHECK(c.find(id)->descriptor.long_running);
    CHECK(c.find("part.boolean")->descriptor.target["kind"] == "volumes");
    CHECK(c.find("object.cut")->descriptor.target["kind"] == "object");
    CHECK(c.find("object.addShape")->descriptor.target["kind"] == "none");
}

TEST_CASE("S3 ui keys and the part manifest agree", "[actions][parts]")
{
    const fs::path p = part_manifest();
    REQUIRE(fs::exists(p));
    const json manifest = read_json(p);
    // A ui key may live in another domain's manifest (the object list's columns are in object.json),
    // so the claims are checked against every manifest; each key lives in exactly one.
    json all = json::object();
    for (fs::directory_iterator it(p.parent_path()), end; it != end; ++it)
        if (it->path().extension() == ".json") {
            const json m = read_json(it->path());
            for (auto kv = m.begin(); kv != m.end(); ++kv)
                all[kv.key()] = kv.value();
        }

    std::set<std::string> ids;
    for (const ActionEntry& e : s3_catalog().entries()) {
        ids.insert(e.descriptor.id);
        for (const std::string& key : e.descriptor.ui) {
            INFO(e.descriptor.id << " claims " << key);
            REQUIRE(all.contains(key));
            const json& v = all[key];
            const bool  names_it = (v.contains("action") && v["action"] == e.descriptor.id) ||
                                  (v.contains("actions") && std::find(v["actions"].begin(), v["actions"].end(), e.descriptor.id) != v["actions"].end());
            CHECK(names_it);
        }
    }
    for (auto it = manifest.begin(); it != manifest.end(); ++it) {
        if (it.key().front() == '$')
            continue;
        INFO(it.key());
        CHECK_FALSE(it.value().contains("todo"));
        std::vector<std::string> named;
        if (it.value().contains("action"))
            named.push_back(it.value()["action"].get<std::string>());
        if (it.value().contains("actions"))
            for (const auto& a : it.value()["actions"])
                named.push_back(a.get<std::string>());
        for (const std::string& a : named)
            CHECK(ids.count(a) == 1);
    }
}

TEST_CASE("Volume type names round-trip", "[actions][parts]")
{
    for (const std::string& n : parts::volume_type_names())
        CHECK(parts::volume_type_name(parts::volume_type_from_name(n)) == n);
    CHECK(parts::volume_type_from_name("lid") == ModelVolumeType::INVALID);
    CHECK(parts::volume_type_from_name("negative") == ModelVolumeType::NEGATIVE_VOLUME);
    CHECK(parts::volume_type_from_name("infillMesh") == ModelVolumeType::INFILL_MESH);
}

TEST_CASE("Primitives have the menu's proportions and fit a requested size", "[actions][parts]")
{
    for (const std::string& s : parts::shape_names()) {
        INFO(s);
        TriangleMesh m = parts::make_primitive(s, 20.);
        REQUIRE_FALSE(m.empty());
        const Vec3d sz = m.bounding_box().size();
        // The menu's torus has ring radius side/2 and tube radius side/8, so it spans 1.25 sides.
        const double expected = s == "torus" ? 25. : 20.;
        CHECK_THAT(std::max({sz.x(), sz.y(), sz.z()}), WithinAbs(expected, 0.05));

        TriangleMesh u = m;
        REQUIRE(parts::fit_mesh_to_size(u, {10.}));
        const Vec3d su = u.bounding_box().size();
        CHECK_THAT(std::max({su.x(), su.y(), su.z()}), WithinAbs(10., 1e-3));

        TriangleMesh b = m;
        REQUIRE(parts::fit_mesh_to_size(b, {4., 6., 8.}));
        const Vec3d sb = b.bounding_box().size();
        CHECK_THAT(sb.x(), WithinAbs(4., 1e-3));
        CHECK_THAT(sb.y(), WithinAbs(6., 1e-3));
        CHECK_THAT(sb.z(), WithinAbs(8., 1e-3));
    }
    CHECK(parts::make_primitive("pyramid", 10.).empty());
    TriangleMesh m = parts::make_primitive("cube", 10.);
    CHECK_FALSE(parts::fit_mesh_to_size(m, {0.}));
    CHECK_FALSE(parts::fit_mesh_to_size(m, {1., -1., 1.}));
    CHECK_FALSE(parts::fit_mesh_to_size(m, {}));
}

TEST_CASE("A volume placed in the plate frame lands there under any instance transform", "[actions][parts]")
{
    // Instance: moved, rotated 90 degrees about Z and scaled 2x.
    const Transform3d inst = Geometry::translation_transform(Vec3d(100., 50., 0.)) *
                             Geometry::rotation_transform(Vec3d(0., 0., M_PI / 2.)) * Geometry::scale_transform(2.);
    const Vec3d       center(110., 60., 7.);
    const Transform3d vol   = parts::volume_matrix_for_world(inst, center, Vec3d(0., 0., 30.));
    const Transform3d world = inst * vol;
    CHECK(close_to(world * Vec3d::Zero(), center));
    // The volume is unscaled on the plate (its size in mm is the size asked for)...
    CHECK_THAT((world.linear() * Vec3d::UnitX()).norm(), WithinAbs(1., 1e-9));
    // ...and rotated 30 degrees about the plate Z.
    const Vec3d x = world.linear() * Vec3d::UnitX();
    CHECK_THAT(std::atan2(x.y(), x.x()) * 180. / M_PI, WithinAbs(30., 1e-6));
}

TEST_CASE("Cut planes and sides", "[actions][parts]")
{
    for (const Vec3d& n : std::vector<Vec3d>{Vec3d(0., 0., 1.), Vec3d(1., 0., 1.).normalized(), Vec3d(0., -1., 0.), Vec3d(0., 0., -1.)}) {
        const Transform3d r = parts::rotation_z_to(n);
        CHECK(close_to(r.linear() * Vec3d::UnitZ(), n, 1e-9));
        CHECK_THAT(r.linear().determinant(), WithinAbs(1., 1e-9));
    }
    CHECK(close_to(*parts::side_normal("front"), Vec3d(0., -1., 0.)));
    CHECK(close_to(*parts::side_normal("top"), Vec3d(0., 0., 1.)));
    CHECK_FALSE(parts::side_normal("inside").has_value());

    BoundingBoxf3 bb(Vec3d(0., 0., 0.), Vec3d(20., 20., 20.));
    CHECK(close_to(parts::default_part_center(bb, Vec3d(4., 4., 4.)), Vec3d(22., 2., 2.)));
}

TEST_CASE("vec3 params", "[actions][parts]")
{
    CHECK(close_to(*parts::vec3_param({{"p", {1, 2, 3}}}, "p"), Vec3d(1., 2., 3.)));
    CHECK_FALSE(parts::vec3_param({{"p", {1, 2}}}, "p").has_value());
    CHECK_FALSE(parts::vec3_param({{"p", {1, "a", 3}}}, "p").has_value());
    CHECK_FALSE(parts::vec3_param(json::object(), "p").has_value());
    CHECK(parts::vec3_json(Vec3d(1.23456, 0., -2.)) == json::array({1.235, 0., -2.}));
}

TEST_CASE("Cut pieces follow the Cut tool's per-piece rules", "[actions][parts]")
{
    using parts::CutPieceFlags;
    CHECK(parts::check_cut_piece({true, true, false}, "upper", false).empty());
    CHECK(parts::check_cut_piece({true, false, true}, "lower", false).empty());
    CHECK_FALSE(parts::check_cut_piece({true, true, true}, "upper", false).empty());  // place on cut and flip exclude each other
    CHECK_FALSE(parts::check_cut_piece({false, true, false}, "lower", false).empty()); // a dropped piece is not placed
    CHECK_FALSE(parts::check_cut_piece({true, false, true}, "upper", true).empty());   // cut to parts disables both
    CHECK(parts::check_cut_piece({true, false, false}, "upper", true).empty());
}

TEST_CASE("The cut plane's spin turns its in-plane axes about the normal", "[actions][parts]")
{
    const Vec3d n(0., 0., 1.);
    const Transform3d r0 = parts::cut_rotation(n, 0.);
    const Transform3d r9 = parts::cut_rotation(n, 90.);
    CHECK(close_to(r0.linear() * Vec3d::UnitZ(), n));
    CHECK(close_to(r9.linear() * Vec3d::UnitZ(), n));
    // A quarter turn about the normal takes the old X axis to the old Y axis.
    CHECK(close_to(r9.linear() * Vec3d::UnitX(), r0.linear() * Vec3d::UnitY()));
    const Vec3d tilted = Vec3d(1., 0., 1.).normalized();
    CHECK(close_to(parts::cut_rotation(tilted, 37.).linear() * Vec3d::UnitZ(), tilted));
}

TEST_CASE("Dovetail defaults and the flap check follow the Cut tool", "[actions][parts]")
{
    double depth = 0., width = 0.;
    parts::default_groove(Vec3d(30., 30., 30.), depth, width); // (30+30+30)/60 = 1.5
    CHECK_THAT(depth, WithinAbs(1.5, 1e-9));
    CHECK_THAT(width, WithinAbs(6., 1e-9));
    parts::default_groove(Vec3d(5., 5., 5.), depth, width); // never under 1 mm
    CHECK_THAT(depth, WithinAbs(1., 1e-9));
    CHECK(parts::groove_flaps_fit(2., 8., 60. * M_PI / 180.));
    // An obtuse flap angle narrows the groove's mouth: 2 mm deep at 120 degrees needs 2.31 mm.
    CHECK(parts::groove_flaps_fit(2., 2.4, 120. * M_PI / 180.));
    CHECK_FALSE(parts::groove_flaps_fit(2., 2., 120. * M_PI / 180.));
}

TEST_CASE("Relative transforms along the plate's axes or the placement's own", "[actions][parts]")
{
    // A placement turned 90 degrees about Z, at (10, 0, 0).
    Transform3d m = Transform3d::Identity();
    m.translate(Vec3d(10., 0., 0.));
    m.rotate(Eigen::AngleAxisd(0.5 * M_PI, Vec3d::UnitZ()));

    const Vec3d move(5., 0., 0.);
    // World: along the plate's X. Object: along its own X, which is the plate's Y.
    CHECK(close_to(parts::relative_transform(m, move, std::nullopt, std::nullopt, false, Vec3d::Zero()).translation(), Vec3d(15., 0., 0.)));
    CHECK(close_to(parts::relative_transform(m, move, std::nullopt, std::nullopt, true, Vec3d::Zero()).translation(), Vec3d(10., 5., 0.)));

    // A world-X stretch of the turned placement stretches its own Y axis.
    const Transform3d s = parts::relative_transform(m, std::nullopt, std::nullopt, Vec3d(2., 1., 1.), false, Vec3d(10., 0., 0.));
    CHECK_THAT((s.linear() * Vec3d::UnitY()).norm(), WithinAbs(2., 1e-9));
    CHECK_THAT((s.linear() * Vec3d::UnitX()).norm(), WithinAbs(1., 1e-9));
    CHECK(close_to(s.translation(), Vec3d(10., 0., 0.))); // about the pivot

    // Turning about the pivot keeps the pivot in place.
    const Transform3d r = parts::relative_transform(m, std::nullopt, Vec3d(0., 0., 90.), std::nullopt, false, Vec3d(10., 0., 0.));
    CHECK(close_to(r * Vec3d::Zero(), Vec3d(10., 0., 0.)));
}

TEST_CASE("Cut-plane tilt clamp keeps the direction it leans in", "[actions][parts]")
{
    namespace CPR = CutPlaneRules;
    const Vec3d steep = Geometry::build_normal_from_bc(-60., 30.);
    const Vec3d held  = CPR::clamp_to_tilt(steep, 40.);
    CHECK_THAT(CPR::tilt_deg(held), WithinAbs(40., 1e-6));
    double b = 0., c = 0.;
    Geometry::bc_from_build_normal(held, b, c);
    CHECK_THAT(c, WithinAbs(30., 1e-6));
    CHECK(close_to(CPR::clamp_to_tilt(steep, 0.), steep));  // no limit
    CHECK(close_to(CPR::clamp_to_tilt(steep, 70.), steep)); // inside the limit
    CHECK_FALSE(CPR::tilt_azimuth(Vec3d::UnitZ()).has_value());
    // At the pole C is meaningless and taken as 0, as the tool's B/C fields do.
    CHECK(close_to(CPR::normal_from_bc(0., 77.), Vec3d::UnitZ()));
}

TEST_CASE("A cut-plane replace keeps the build direction of unchanged planes", "[actions][parts]")
{
    namespace CPR = CutPlaneRules;
    CutPlane a;
    a.point = Vec3d(0., 0., 10.);
    a.normal = Vec3d::UnitZ();
    a.build_normal = Vec3d(0., 1., 1.).normalized();
    a.has_build_normal = true;
    CutPlane b;
    b.point = Vec3d(0., 0., 30.);
    b.normal = Vec3d(1., 0., 1.).normalized();
    b.build_normal = Vec3d::UnitX();
    b.has_build_normal = true;
    const std::vector<CutPlane> old = {a, b};

    // Read back (rounded), reordered, the second one moved: only the first keeps its override.
    CutPlane a2 = a, b2 = b;
    a2.point += Vec3d(5., -3., 0.00004); // another point on the same plane, plus rounding
    a2.has_build_normal = false;
    b2.point += Vec3d(0., 0., 4.);
    b2.has_build_normal = false;
    std::vector<CutPlane> fresh = {b2, a2};
    int dropped = 0;
    CHECK(CPR::carry_build_normals(old, fresh, {false, false}, &dropped) == 1);
    CHECK(dropped == 1);
    CHECK_FALSE(fresh[0].has_build_normal);
    REQUIRE(fresh[1].has_build_normal);
    CHECK(close_to(fresh[1].build_normal, a.build_normal));

    // A plane that states its own direction (or resets it) is left as given and drops nothing.
    std::vector<CutPlane> stated = {a2};
    CHECK(CPR::carry_build_normals({a}, stated, {true}, &dropped) == 0);
    CHECK(dropped == 0);
    CHECK_FALSE(stated[0].has_build_normal);
}

TEST_CASE("Cut-plane normals map between the object and the plate as the tool maps them", "[actions][parts]")
{
    namespace CPR = CutPlaneRules;
    Transform3d m = Transform3d::Identity();
    m.translate(Vec3d(50., 40., 0.));
    m.rotate(Eigen::AngleAxisd(0.3, Vec3d(1., 2., 3.).normalized()));
    const Vec3d n_world = Vec3d(0.2, -0.4, 0.9).normalized();
    CHECK(close_to(CPR::world_normal(m, CPR::local_normal(m, n_world)), n_world, 1e-9));
}
