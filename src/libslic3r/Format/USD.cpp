#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "USD.hpp"

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace Slic3r {

namespace {

// A stage declares its own unit. metersPerUnit=1 means one unit is one metre,
// so geometry scales by 1000 into millimetres. Getting this wrong is not subtle
// -- it is a model 1000x too big -- but it is silent, so it is done here once
// and logged.
double mm_per_unit(const UsdStageRefPtr &stage)
{
    const double meters = UsdGeomGetStageMetersPerUnit(stage);
    return (meters > 0.0 ? meters : 0.01) * 1000.0;
}

// USD finds its file-format plugins (usda, usdc, usdz) and its asset resolver
// through plugInfo.json, which it searches for next to the *library* that
// contains it. We link USD statically into the OrcaSlicer binary, so that
// search looks beside the executable and finds nothing -- and the failure is a
// fatal abort inside UsdStage::Open, not a null return we could report.
//
// Registering explicitly keeps the app relocatable. PXR_INSTALL_LOCATION would
// also work but bakes an absolute build-machine path into the library.
//
// Returns false rather than latching, and says why: USD's reaction to a missing
// asset resolver is a fatal abort inside UsdStage::Open, so a quiet no-op here
// becomes a crash with no explanation. Not latching also lets an embedder that
// has not yet called set_resources_dir() succeed on a later attempt.
bool register_usd_plugins()
{
    namespace fs = boost::filesystem;

    static bool registered = false;
    if (registered)
        return true;

    // Beside the binary covers the macOS bundle and build-tree runs; resources/
    // covers the install() and AppImage layouts. resources_dir() can be empty in a test
    // binary or an embedder, which is why it is not the only candidate.
    std::vector<fs::path> candidates;
    candidates.emplace_back(boost::dll::program_location().parent_path() / "usd");
    if (!resources_dir().empty())
        candidates.emplace_back(fs::path(resources_dir()) / "usd");

    for (const fs::path &dir : candidates) {
        if (!fs::is_directory(dir))
            continue;
        if (!PlugRegistry::GetInstance().RegisterPlugins(dir.string()).empty()) {
            registered = true;
            return true;
        }
    }

    std::string tried;
    for (const fs::path &dir : candidates)
        tried += (tried.empty() ? "" : ", ") + dir.string();
    BOOST_LOG_TRIVIAL(error)
        << "load_usd: OpenUSD's plugInfo.json tree was not found (looked in: "
        << (tried.empty() ? "nowhere -- no candidate paths" : tried)
        << "). USD files cannot be read without it.";
    return false;
}

// Read at the earliest available time sample, NOT at Default().
//
// Usd.TimeCode.Default() never consults time samples. Houdini's USD ROP writes
// `points` as a single sample with no default value, so a stage that looks
// perfectly static reads back as zero meshes and the importer reports an empty
// file. EarliestTime() resolves both the sampled and the defaulted case.
// The exception is a stage that cannot say how fast its samples run. Resolving a
// transform at EarliestTime() on a stage authoring timeCodesPerSecond = 0
// segfaults inside OpenUSD -- UsdGeomXformCache::GetLocalToWorldTransform() ->
// SdfData::GetBracketingTimeSamplesForPath(). Reproducible in stock pxr with no
// Slic3r involved, and it is not a contrived shape: the USD Working Group's own
// conformance suite ships one, in
// test_assets/foundation/stage_configuration/timeCodesPerSecond/. Such a stage
// has no meaningful earliest sample anyway, so fall back rather than crash.
UsdTimeCode read_time(const UsdStageRefPtr &stage)
{
    const double per_second = stage->GetTimeCodesPerSecond();
    if (!std::isfinite(per_second) || per_second <= 0.0) {
        BOOST_LOG_TRIVIAL(warning)
            << "load_usd: stage declares timeCodesPerSecond = " << per_second
            << ", so time samples cannot be placed; reading at the default time.";
        return UsdTimeCode::Default();
    }
    return UsdTimeCode::EarliestTime();
}

// UsdGeomCube, Sphere, Cylinder and Cone are geometry in their own right, not
// meshes -- a stage may contain nothing else. Apple's ModelIO tessellates them,
// so skipping them left this importer behind the path it replaces: measured over
// 3142 published USD files, 351 held only intrinsic shapes and were refused
// outright, and another 59 lost the shapes from a stage that also had meshes.
// `def Sphere "sphere" {}` is the simplest valid USD file there is.
//
// Built with Slic3r's own primitive builders at the facet angle Orca uses for
// shapes added from its own menu -- PI/90, two degrees, per GUI_ObjectList.cpp
// -- so an imported sphere is exactly as round as one Orca makes itself.
//
// USD centres all four on the prim origin, while its_make_cube() starts at a
// corner and its_make_cylinder()/its_make_cone() start at z = 0, so each is
// re-centred. `axis` lays a cylinder or cone along X or Y instead of Z.
//
// Capsule and Plane are deliberately absent: a capsule needs composing from a
// cylinder and two hemispheres, and a plane is a zero-thickness sheet that
// encloses no volume. Both are counted and named rather than passed over.
bool tessellate_gprim(const UsdPrim &prim, const UsdTimeCode when, indexed_triangle_set &its)
{
    const double facet = PI / 90.0;

    auto recentre = [&its](float dx, float dy, float dz) {
        for (Vec3f &v : its.vertices) v += Vec3f(dx, dy, dz);
    };
    // Slic3r builds along +Z; rotate that axis onto X or Y as the prim asks.
    auto orient = [&its](const TfToken &axis) {
        if (axis == UsdGeomTokens->x)
            for (Vec3f &v : its.vertices) v = Vec3f(v.z(), v.y(), -v.x());
        else if (axis == UsdGeomTokens->y)
            for (Vec3f &v : its.vertices) v = Vec3f(v.x(), v.z(), -v.y());
    };

    if (UsdGeomCube cube = UsdGeomCube(prim)) {
        double size = 2.0;
        cube.GetSizeAttr().Get(&size, when);
        if (!(size > 0.0)) return false;
        its = its_make_cube(size, size, size);
        recentre(float(-0.5 * size), float(-0.5 * size), float(-0.5 * size));
        return true;
    }
    if (UsdGeomSphere sphere = UsdGeomSphere(prim)) {
        double radius = 1.0;
        sphere.GetRadiusAttr().Get(&radius, when);
        if (!(radius > 0.0)) return false;
        its = its_make_sphere(radius, facet);   // already centred on the origin
        return true;
    }
    if (UsdGeomCylinder cylinder = UsdGeomCylinder(prim)) {
        double radius = 1.0, height = 2.0;
        TfToken axis = UsdGeomTokens->z;
        cylinder.GetRadiusAttr().Get(&radius, when);
        cylinder.GetHeightAttr().Get(&height, when);
        cylinder.GetAxisAttr().Get(&axis, when);
        if (!(radius > 0.0) || !(height > 0.0)) return false;
        its = its_make_cylinder(radius, height, facet);
        recentre(0.f, 0.f, float(-0.5 * height));
        orient(axis);
        return true;
    }
    if (UsdGeomCone cone = UsdGeomCone(prim)) {
        double radius = 1.0, height = 2.0;
        TfToken axis = UsdGeomTokens->z;
        cone.GetRadiusAttr().Get(&radius, when);
        cone.GetHeightAttr().Get(&height, when);
        cone.GetAxisAttr().Get(&axis, when);
        if (!(radius > 0.0) || !(height > 0.0)) return false;
        its = its_make_cone(radius, height, facet);
        recentre(0.f, 0.f, float(-0.5 * height));
        orient(axis);
        return true;
    }
    return false;
}

// One mesh per USD prim, so a stage's structure survives into Orca's model
// rather than being flattened on the way in.
struct NamedMesh
{
    std::string          name;
    indexed_triangle_set its;
};

// Why the counters exist: every reason a mesh is skipped has to reach the user.
// A stage where all forty meshes are `proxy` must not report only that no
// printable meshes were found,
// and one mesh dropped from a stage of ten must not import silently.
struct SkipCounts
{
    size_t purpose = 0;      // proxy or guide
    size_t invisible = 0;    // computed visibility is `invisible`
    size_t no_data = 0;      // points/counts/indices unreadable or empty
    size_t bad_topology = 0; // counts and indices disagree, or an index is out of range
    size_t holes = 0;        // authored holeIndices, which are not honoured
    size_t degenerate = 0;   // fewer than two triangles, so not a printable solid
    size_t nonfinite = 0;    // a point coordinate is NaN or infinite
    size_t gprim = 0;        // an intrinsic shape this does not tessellate
};

std::string describe(const SkipCounts &s)
{
    std::string out;
    auto add = [&out](size_t n, const char *what) {
        if (!n) return;
        if (!out.empty()) out += ", ";
        out += std::to_string(n) + " " + what;
    };
    add(s.purpose,      "skipped as proxy/guide");
    add(s.invisible,    "skipped as invisible");
    add(s.no_data,      "skipped with unreadable point data");
    add(s.bad_topology, "skipped with inconsistent topology");
    add(s.holes,        "skipped for authored holeIndices");
    add(s.degenerate,   "skipped as single triangles");
    add(s.nonfinite,    "skipped for non-finite coordinates");
    add(s.gprim,        "skipped as untessellated intrinsic shapes");
    return out.empty() ? out : "USD import: " + out + ".";
}

// `purpose` and `visibility` are INHERITED, so the local opinion is the wrong
// question. Real content authors them on a group -- `/World/Proxy` with a plain
// Mesh underneath -- and reading the leaf's own attribute returns the fallback
// `default`, importing exactly the stand-in geometry this is meant to exclude.
// ComputePurpose()/ComputeVisibility() resolve down the hierarchy.
bool is_renderable(const UsdPrim &prim, const UsdTimeCode when, SkipCounts &skipped)
{
    UsdGeomImageable imageable(prim);
    if (!imageable)
        return false;

    const TfToken purpose = imageable.ComputePurpose();
    if (purpose == UsdGeomTokens->proxy || purpose == UsdGeomTokens->guide) {
        ++ skipped.purpose;
        return false;
    }
    if (imageable.ComputeVisibility(when) == UsdGeomTokens->invisible) {
        ++ skipped.invisible;
        return false;
    }
    return true;
}

bool read_stage(const char *path, std::vector<NamedMesh> &out, std::string &message)
{
    try {
        if (!register_usd_plugins()) {
            message = "OpenUSD's plugin data is missing from this build, so USD files cannot be read.";
            return false;
        }

        // TfErrorMark catches USD's non-fatal diagnostics so they can be
        // reported rather than only logged by USD itself. It cannot catch a
        // TF_FATAL_ERROR, which calls abort() -- see the note above about the
        // plugin registry, which is the reachable cause of that.
        TfErrorMark usd_errors;

        UsdStageRefPtr stage = UsdStage::Open(path);
        if (!stage) {
            message = "Could not open USD stage " + std::string(path) + ".";
            for (const auto &e : usd_errors)
                message += " " + e.GetCommentary();
            BOOST_LOG_TRIVIAL(error) << "load_usd: " << message;
            return false;
        }

        const double scale = mm_per_unit(stage);
        const bool   y_up  = (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->y);
        const UsdTimeCode when = read_time(stage);

        UsdGeomXformCache    xf_cache(when);
        size_t               mesh_count = 0;
        size_t               cages      = 0;   // imported unsubdivided, not skipped
        SkipCounts           skipped;

        // Instance proxies: a plain Traverse() returns instanceable prims but not
        // their prototype children, so an instanced scene yields zero meshes and
        // is refused as empty -- a file that plainly contains geometry.
        //
        // The predicate is USD's default one, which is
        // IsActive && IsDefined && IsLoaded && !IsAbstract -- spelling it out by
        // hand and omitting IsActive would import prims deactivated in an
        // override layer, and `active = false` is how a USD pipeline deletes
        // geometry non-destructively. !IsAbstract keeps class prototypes from
        // being counted as geometry alongside the instances referencing them.
        UsdPrimRange range = UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies());

        for (const UsdPrim &prim : range) {
            UsdGeomMesh mesh(prim);
            if (!mesh)
                continue;
            if (!is_renderable(prim, when, skipped))
                continue;

            VtVec3fArray points;
            VtIntArray   counts, indices;
            if (!mesh.GetPointsAttr().Get(&points, when) ||
                !mesh.GetFaceVertexCountsAttr().Get(&counts, when) ||
                !mesh.GetFaceVertexIndicesAttr().Get(&indices, when) ||
                points.empty() || counts.empty()) {
                // Never a bare `continue`. A mesh dropped here used to vanish
                // from a mixed stage with nothing said and the import still
                // reporting success, which is the failure this file exists to
                // prevent.
                ++ skipped.no_data;
                continue;
            }

            // A subdivision cage is imported as its control mesh, unsmoothed,
            // and counted so the import says so.
            //
            // Absent means catmullClark per the USD spec -- mayaUSD writes the
            // attribute only when it is NOT the default -- so absence is
            // reported distinctly, because the fix differs: an author who meant
            // a polygon mesh just needs to author `uniform token
            // subdivisionScheme = "none"`.
            TfToken scheme = UsdGeomTokens->catmullClark;
            const bool declared = mesh.GetSubdivisionSchemeAttr().HasAuthoredValue();
            mesh.GetSubdivisionSchemeAttr().Get(&scheme, when);
            // Imported as the control mesh, not evaluated, which is what the
            // existing macOS ModelIO path does -- measured, not assumed: on the
            // USD Working Group's Open Chess Set it returns world bounds of
            // 0.705416 x 0.168996 x 0.705416, which are the cage's own to six
            // decimals. Catmull-Clark contracts toward the convex hull, so a
            // subdividing importer could not have returned them unchanged.
            //
            // Refusing instead was implemented first and is worse in practice:
            // every mesh in that asset is a catmullClark cage, so a refusal loses
            // the whole model, and DCC exporters emit subdivision by default. How
            // far a control mesh sits from its limit surface depends on edge
            // length against local curvature, not on face count, and nothing here
            // bounds it -- so every cage is counted, and the first few are named
            // in the log, rather than being quietly accepted. Crease, corner and
            // interpolateBoundary attributes are ignored along with the scheme.
            const bool is_cage = (scheme != UsdGeomTokens->none);

            // holeIndices deletes faces from the authored surface. Ignoring it
            // yields a closed solid where the author specified an opening, so it
            // is refused rather than silently filled.
            VtIntArray holes;
            if (mesh.GetHoleIndicesAttr().Get(&holes, when) && !holes.empty()) {
                BOOST_LOG_TRIVIAL(error)
                    << "load_usd: " << prim.GetPath().GetString() << " authors "
                    << holes.size() << " holeIndices, which are not honoured;"
                       " importing it would fill openings the author specified.";
                ++ skipped.holes;
                continue;
            }

            // The counts must describe exactly the indices present. A truncated
            // faceVertexIndices is a common broken-export shape, and dropping the
            // affected faces one at a time produces a mesh with a hole in it and
            // a successful-looking import.
            // Summed over every face, including a degenerate one: stopping at the
            // first left a partial total that was then reported as what the counts
            // describe, so [4, 2, 4] against ten indices said "describe 4" when
            // they describe 10. The two faults are also reported separately --
            // quoting a length mismatch of "10 indices but 10" to say a face was
            // malformed explained nothing.
            size_t needed = 0;
            bool   degenerate = false;
            for (int n : counts) {
                if (n < 3) degenerate = true;
                if (n > 0) needed += size_t(n);
            }
            if (degenerate || needed != indices.size()) {
                std::string why;
                if (needed != indices.size())
                    why = "faceVertexCounts describe " + std::to_string(needed) +
                          " indices but faceVertexIndices holds " +
                          std::to_string(indices.size());
                if (degenerate)
                    why += (why.empty() ? "" : ", and ") +
                           std::string("a face has fewer than three vertices");
                BOOST_LOG_TRIVIAL(error)
                    << "load_usd: " << prim.GetPath().GetString()
                    << " has inconsistent topology: " << why << ".";
                ++ skipped.bad_topology;
                continue;
            }

            // USD permits inf and nan in a point array, and OpenUSD reads them
            // back faithfully. Nothing downstream does: the bounding box, the
            // volume and every slicing decision become nan, and the object
            // reports a successful import with no size. Apple's ModelIO refuses
            // such a file outright, so accepting it would be strictly worse than
            // the path this replaces. OpenUSD's own test suite ships one
            // (testUsdviewInfGeom/infGeom.usda), a pentagon with one vertex at
            // infinity.
            bool finite = true;
            for (const GfVec3f &p : points)
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
                    finite = false;
                    break;
                }
            if (!finite) {
                BOOST_LOG_TRIVIAL(error)
                    << "load_usd: " << prim.GetPath().GetString()
                    << " has a non-finite point coordinate (nan or inf), which"
                       " would give the imported object no measurable size.";
                ++ skipped.nonfinite;
                continue;
            }

            // Every index value must address a point of THIS mesh. Checking only
            // the cursor against the array length leaves the values unchecked,
            // and they land in an indexed_triangle_set whose consumers index
            // std::vector with operator[] -- an out-of-bounds read on the first
            // volume computation. A negative value is worse than a crash: it
            // silently addresses a previous mesh's vertices.
            const long long npoints = (long long) points.size();
            bool indices_ok = true;
            for (int v : indices)
                if (v < 0 || (long long) v >= npoints) { indices_ok = false; break; }
            if (!indices_ok) {
                BOOST_LOG_TRIVIAL(error)
                    << "load_usd: " << prim.GetPath().GetString()
                    << " has a faceVertexIndices value outside [0, " << npoints << ").";
                ++ skipped.bad_topology;
                continue;
            }

            // Each prim becomes its own mesh, so indices start at zero and the
            // int that Slic3r's triangle indices use only has to hold this
            // prim's vertex count. Refuse rather than wrap into garbage.
            if (points.size() > size_t(std::numeric_limits<int>::max())) {
                message = "Refusing " + std::string(path) + ": " +
                          prim.GetPath().GetString() + " has " +
                          std::to_string(points.size()) + " vertices, more than a"
                          " triangle index can address.";
                BOOST_LOG_TRIVIAL(error) << "load_usd: " << message;
                return false;
            }

            // leftHanded reverses the winding, and Slic3r derives facet
            // orientation from winding, so this must be honoured or the mesh
            // imports inside-out.
            TfToken orientation = UsdGeomTokens->rightHanded;
            mesh.GetOrientationAttr().Get(&orientation, when);
            bool flip = (orientation == UsdGeomTokens->leftHanded);

            const GfMatrix4d world = xf_cache.GetLocalToWorldTransform(prim);
            // A mirroring transform flips winding a second time; two flips cancel.
            if (world.GetDeterminant() < 0.0)
                flip = !flip;

            indexed_triangle_set its;
            its.vertices.reserve(points.size());
            for (const GfVec3f &p : points) {
                GfVec3d w = world.Transform(GfVec3d(p[0], p[1], p[2]));
                // Y-up to Z-up is a +90 deg rotation about X: (x, y, z) -> (x, -z, y).
                // The sign matters and is not symmetric -- the inverse rotation
                // has identical extents but places the model below the plate.
                double x = w[0], y = w[1], z = w[2];
                if (y_up) { double t = y; y = -z; z = t; }
                its.vertices.emplace_back(float(x * scale), float(y * scale), float(z * scale));
            }

            // Fan-triangulate each n-gon. USD's authored content is mostly quads,
            // and a fan is correct for any convex face; concave faces produce
            // overlapping triangles, which the slicer's boolean resolves anyway.
            // Both the counts and every index value were validated above, so this
            // loop needs no defensive branches and cannot drop a face.
            size_t cursor = 0;
            for (int n : counts) {
                for (int k = 1; k + 1 < n; ++ k) {
                    int a = indices[cursor];
                    int b = indices[cursor + k];
                    int c = indices[cursor + k + 1];
                    if (flip) std::swap(b, c);
                    its.indices.emplace_back(a, b, c);
                }
                cursor += size_t(n);
            }
            // A ModelVolume built from one triangle never gets a convex hull:
            // Model.hpp guards calculate_convex_hull() behind facets_count() > 1,
            // and ModelVolume::calculate_convex_hull_2d() then dereferences the
            // null m_convex_hull, so the first hull computation segfaults. One
            // ModelVolume per prim makes that reachable from an ordinary file --
            // Apple's toy_drummer.usdz has seven single-triangle prims left over
            // from its rig, and crashed the slicer on import. A lone triangle
            // encloses no volume and cannot be printed, so it is dropped, and
            // counted rather than dropped quietly.
            if (its.indices.size() < 2) {
                BOOST_LOG_TRIVIAL(warning)
                    << "load_usd: " << prim.GetPath().GetString() << " has only "
                    << its.indices.size() << " triangle(s), which encloses no"
                       " volume; skipping it.";
                ++ skipped.degenerate;
                continue;
            }

            out.push_back({prim.GetPath().GetString(), std::move(its)});
            ++ mesh_count;

            // Counted only once the mesh is actually emitted. Counting it at the
            // scheme check instead let a cage that was then skipped for
            // holeIndices or bad topology report "1 of the 0 mesh(es) imported
            // are subdivision cages", and claim in the log that a control mesh
            // had been imported when nothing had been.
            if (is_cage) {
                if (cages < 5)
                    BOOST_LOG_TRIVIAL(warning)
                        << "load_usd: " << prim.GetPath().GetString() << " is a "
                        << scheme.GetString() << " subdivision cage"
                        << (declared ? "" : " (subdivisionScheme is absent, which USD"
                                            " defines as catmullClark)")
                        << "; importing its control mesh without subdividing.";
                ++ cages;
            }
        }

        // Second pass: geometry that is not a Mesh. Kept separate from the loop
        // above because none of that loop's validation applies -- these shapes
        // are generated, not read, so their topology is correct by construction.
        for (const UsdPrim &prim : UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies())) {
            if (prim.IsA<UsdGeomMesh>() || !prim.IsA<UsdGeomGprim>())
                continue;
            if (!is_renderable(prim, when, skipped))
                continue;

            indexed_triangle_set its;
            if (!tessellate_gprim(prim, when, its)) {
                // Named, not merely counted: "a Capsule was skipped" is
                // actionable, "something was skipped" is not.
                BOOST_LOG_TRIVIAL(error)
                    << "load_usd: " << prim.GetPath().GetString() << " is a "
                    << prim.GetTypeName().GetString()
                    << ", which this importer does not tessellate; skipping it.";
                ++ skipped.gprim;
                continue;
            }

            TfToken orientation = UsdGeomTokens->rightHanded;
            UsdGeomGprim(prim).GetOrientationAttr().Get(&orientation, when);
            bool flip = (orientation == UsdGeomTokens->leftHanded);

            const GfMatrix4d world = xf_cache.GetLocalToWorldTransform(prim);
            if (world.GetDeterminant() < 0.0)
                flip = !flip;

            for (Vec3f &v : its.vertices) {
                GfVec3d w = world.Transform(GfVec3d(v.x(), v.y(), v.z()));
                double x = w[0], y = w[1], z = w[2];
                if (y_up) { double t = y; y = -z; z = t; }
                v = Vec3f(float(x * scale), float(y * scale), float(z * scale));
            }
            if (flip)
                for (stl_triangle_vertex_indices &t : its.indices)
                    std::swap(t[1], t[2]);

            out.push_back({prim.GetPath().GetString(), std::move(its)});
            ++ mesh_count;
        }

        // Anything skipped is stated, whether or not the import goes on to
        // succeed -- a stage that imported nine of ten meshes must not look like
        // a clean import. Cages are counted here too: they were imported, but as
        // their control mesh, which is not the surface the author authored.
        //
        // On SUCCESS this reaches the log and the `message` out-param, but
        // Model::read_from_file reads `message` only when the load fails, so a
        // user importing a cage sees correct-looking geometry and a log line.
        // Refusals do reach the error dialog. See the note in USD.hpp.
        // The skip tally rides along on every refusal below. The cage note does
        // NOT: it describes meshes that were imported, so attaching it to a
        // refusal asserts an import that did not happen. A stage of one
        // importable cage and one holed mesh otherwise put "1 of the 1 mesh(es)
        // imported are subdivision cages ... re-export to silence this" into the
        // error dialog for a load that imported nothing and silenced nothing.
        const std::string tally = describe(skipped);
        const std::string cage_note = cages == 0 ? std::string() :
            std::to_string(cages) + " of the " + std::to_string(mesh_count) +
            " mesh(es) imported are subdivision cages, taken as their control mesh"
            " because subdivision is not evaluated yet; where the scheme smooths,"
            " the printed surface will be boxier than the author's, and crease and"
            " corner sharpness are ignored. Re-export with"
            " subdivisionScheme = \"none\" to silence this.";

        // holeIndices used to be counted as bad_topology and so was covered by
        // that refusal; with its own counter it needs its own, or a stage of
        // nothing but holed meshes falls through to the generic no-printable-
        // meshes error.
        if (skipped.holes > 0) {
            message = "Refusing " + std::string(path) + ": " +
                      std::to_string(skipped.holes) +
                      " mesh(es) author holeIndices, which are not honoured;"
                      " importing them would fill openings the author specified." +
                      (tally.empty() ? std::string() : " " + tally);
            BOOST_LOG_TRIVIAL(error) << "load_usd: " << message;
            return false;
        }
        if (skipped.bad_topology > 0) {
            message = "Refusing " + std::string(path) + ": " +
                      std::to_string(skipped.bad_topology) +
                      " mesh(es) have inconsistent topology (see the log for each)." +
                      (tally.empty() ? std::string() : " " + tally);
            BOOST_LOG_TRIVIAL(error) << "load_usd: " << message;
            return false;
        }
        if (out.empty()) {
            message = "No printable meshes in " + std::string(path) +
                      (tally.empty() ? "." : ". " + tally);
            BOOST_LOG_TRIVIAL(error) << "load_usd: " << message;
            return false;
        }
        // Success: both parts, and logged once. The cage note used to be logged
        // here and again where it was built.
        message = tally;
        if (!cage_note.empty())
            message = message.empty() ? "USD import: " + cage_note
                                      : message + " " + cage_note;
        if (!message.empty())
            BOOST_LOG_TRIVIAL(warning) << "load_usd: " << message;
        size_t triangles = 0;
        for (const NamedMesh &m : out)
            triangles += m.its.indices.size();
        BOOST_LOG_TRIVIAL(info)
            << "load_usd: " << mesh_count << " mesh(es), " << triangles
            << " triangles, " << scale << " mm/unit, " << (y_up ? "Y-up" : "Z-up");
    } catch (const std::exception &e) {
        // Without this the caller raises the generic "Loading of a model file
        // failed", which is the one failure that does not say why -- and OpenUSD
        // throws here for a corrupt .usdz or an allocation failure on a large
        // points array.
        message = "Could not read " + std::string(path) + ": " + e.what();
        BOOST_LOG_TRIVIAL(error) << "load_usd: " << message;
        return false;
    }
    return true;
}

} // namespace

// Merges the whole stage into one mesh. Kept for callers that want geometry
// without Orca's model, and used by the tests.
bool load_usd(const char *path, TriangleMesh *meshptr, std::string &message)
{
    std::vector<NamedMesh> meshes;
    if (!read_stage(path, meshes, message))
        return false;

    indexed_triangle_set merged;
    for (NamedMesh &m : meshes) {
        const int base = int(merged.vertices.size());
        merged.vertices.insert(merged.vertices.end(), m.its.vertices.begin(), m.its.vertices.end());
        for (const stl_triangle_vertex_indices &t : m.its.indices)
            merged.indices.emplace_back(t[0] + base, t[1] + base, t[2] + base);
    }
    *meshptr = TriangleMesh(std::move(merged));
    return true;
}

// One ModelVolume per mesh prim, following Format/STEP.cpp -- a stage's parts
// stay separately selectable, with their own settings, instead of arriving as
// one indivisible lump.
bool load_usd(const char *path, Model *model, std::string &message, const char *object_name_in)
{
    std::vector<NamedMesh> meshes;
    if (!read_stage(path, meshes, message))
        return false;

    std::string object_name;
    if (object_name_in == nullptr) {
        const char *last_slash = strrchr(path, DIR_SEPARATOR);
        object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
    } else
        object_name.assign(object_name_in);

    ModelObject *object = model->add_object();
    object->name             = object_name;
    object->input_file       = path;

    for (NamedMesh &m : meshes) {
        ModelVolume *volume = object->add_volume(TriangleMesh(std::move(m.its)));
        // The prim path, so a volume in the UI can be traced back to the stage.
        volume->name                 = m.name;
        volume->source.input_file    = path;
        volume->source.object_idx    = (int) model->objects.size() - 1;
        volume->source.volume_idx    = (int) object->volumes.size() - 1;
    }

    return true;
}

}; // namespace Slic3r
