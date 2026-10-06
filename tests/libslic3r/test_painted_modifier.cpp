#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "test_utils.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <miniz.h>

#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {
// An object with one part and a painted modifier covering the facets of its +X side.
struct Scene {
    // Keep the backup directory alive longer than its model.
    ScopedTemporaryDir backup{"orca_painted_modifier"};
    Model              model;
    ModelObject       *object;
    ModelVolume       *part;
    ModelVolume       *painted_modifier;

    Scene()
    {
        model.set_backup_path(backup.string());
        object = model.add_object();
        part   = object->add_volume(make_cube(20, 20, 20));
        Geometry::Transformation transformation;
        transformation.set_offset(Vec3d(1., 2., 3.));
        transformation.set_rotation(Vec3d(0., 0., 0.3));
        part->set_transformation(transformation);
        painted_modifier       = object->add_painted_modifier(*part);
        painted_modifier->name = "painted";
        painted_modifier->painted_modifier_depth = 2.5f;
        painted_modifier->config.set_key_value("wall_loops", new ConfigOptionInt(5));
        const indexed_triangle_set &its = part->mesh().its;
        TriangleSelector            selector(part->mesh());
        for (int facet_idx = 0; facet_idx < int(its.indices.size()); ++facet_idx)
            if (its_face_normal(its, facet_idx).x() > 0.99f)
                selector.set_facet(facet_idx, EnforcerBlockerType::ENFORCER);
        painted_modifier->painted_modifier_facets.set(selector);
        object->add_instance();
    }
};

// Area in mm² of the facets a painted modifier covers.
double painted_area(const ModelVolume &painted_modifier)
{
    const indexed_triangle_set its = painted_modifier.painted_modifier_facets.get_facets_strict(painted_modifier, EnforcerBlockerType::ENFORCER);
    double area = 0.;
    for (const stl_triangle_vertex_indices &face : its.indices) {
        const Vec3d a = its.vertices[face(0)].cast<double>();
        area += 0.5 * (its.vertices[face(1)].cast<double>() - a).cross(its.vertices[face(2)].cast<double>() - a).norm();
    }
    return area;
}

void save(const std::string &path, Model &model, bool share_mesh)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    StoreParams        params;
    params.path     = path.c_str();
    params.model    = &model;
    params.config   = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    if (share_mesh)
        params.strategy = params.strategy | SaveStrategy::ShareMesh;
    REQUIRE(store_bbs_3mf(params));
}

void load(const std::string &path, Model &model)
{
    struct ImportedResources {
        PlateDataPtrs        plates;
        std::vector<Preset*> presets;
        ~ImportedResources() { release_PlateData_list(plates); for (Preset *preset : presets) delete preset; }
    } resources;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    bool                      bbs = false, orca = false;
    Semver                    version;
    REQUIRE(load_bbs_3mf(path.c_str(), &config, &substitutions, &model, &resources.plates, &resources.presets, &bbs, &orca, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
}

std::string model_settings(const std::string &path)
{
    struct Reader {
        mz_zip_archive zip{};
        ~Reader() { if (zip.m_pState) close_zip_reader(&zip); }
    } reader;
    REQUIRE(open_zip_reader(&reader.zip, path));
    const int index = mz_zip_reader_locate_file(&reader.zip, "Metadata/model_settings.config", nullptr, 0);
    REQUIRE(index >= 0);
    mz_zip_archive_file_stat stat;
    REQUIRE(mz_zip_reader_file_stat(&reader.zip, mz_uint(index), &stat));
    std::string data(size_t(stat.m_uncomp_size), '\0');
    REQUIRE(mz_zip_reader_extract_to_mem(&reader.zip, mz_uint(index), data.data(), data.size(), 0));
    return data;
}

// The <part> element of the volume holding the given metadata key.
std::string part_block(const std::string &xml, const std::string &key)
{
    const size_t key_pos = xml.find("key=\"" + key + "\"");
    REQUIRE(key_pos != std::string::npos);
    const size_t begin = xml.rfind("<part ", key_pos);
    const size_t end   = xml.find("</part>", key_pos);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    return xml.substr(begin, end - begin);
}
} // namespace

TEST_CASE("A painted modifier survives a 3MF round trip", "[PaintedModifier]")
{
    const bool share_mesh = GENERATE(false, true);
    CAPTURE(share_mesh);
    Scene source;
    ScopedTemporaryFile file(".3mf");
    save(file.string(), source.model, share_mesh);

    Model destination;
    load(file.string(), destination);
    REQUIRE(destination.objects.size() == 1);
    const ModelObject &object = *destination.objects.front();
    REQUIRE(object.volumes.size() == 2);
    const ModelVolume &part             = *object.volumes[0];
    const ModelVolume &painted_modifier = *object.volumes[1];
    REQUIRE(painted_modifier.is_painted_modifier());
    CHECK(painted_modifier.name == "painted");
    CHECK_THAT(painted_modifier.painted_modifier_depth, WithinAbs(2.5, 1e-6));
    REQUIRE(painted_modifier.config.has("wall_loops"));
    CHECK(painted_modifier.config.opt_int("wall_loops") == 5);
    CHECK(object.painted_modifier_host(painted_modifier) == &part);
    CHECK(painted_modifier.mesh_ptr() == part.mesh_ptr());
    CHECK(painted_modifier.get_matrix().isApprox(part.get_matrix()));
    CHECK(painted_modifier.painted_modifier_facets.get_data() == source.painted_modifier->painted_modifier_facets.get_data());
}

TEST_CASE("A painted modifier is stored as a modifier whose settings older readers drop", "[PaintedModifier]")
{
    Scene source;
    ScopedTemporaryFile file(".3mf");
    save(file.string(), source.model, false);

    const std::string block = part_block(model_settings(file.string()), "painted_modifier_depth");
    CHECK(block.find("subtype=\"modifier_part\"") != std::string::npos);
    CHECK(block.find("key=\"painted_modifier_config:wall_loops\"") != std::string::npos);
    CHECK(block.find("key=\"wall_loops\"") == std::string::npos);
}

TEST_CASE("Sorting volumes keeps painted and mesh modifiers in the user's order", "[PaintedModifier]")
{
    Scene scene;
    ModelVolume *blocker  = scene.object->add_volume(make_cube(1, 1, 1), ModelVolumeType::SUPPORT_BLOCKER);
    ModelVolume *modifier = scene.object->add_volume(make_cube(1, 1, 1), ModelVolumeType::PARAMETER_MODIFIER);
    ModelVolume *second   = scene.object->add_painted_modifier(*scene.part);
    scene.object->sort_volumes(true);

    const ModelVolumePtrs expected{scene.part, scene.painted_modifier, modifier, second, blocker};
    CHECK(scene.object->volumes == expected);
}

TEST_CASE("A painted modifier follows the transformation and the mesh of its host", "[PaintedModifier]")
{
    Scene scene;
    scene.part->set_offset(Vec3d(7., -4., 2.));
    CHECK(scene.object->sync_painted_modifiers());
    CHECK(scene.painted_modifier->get_matrix().isApprox(scene.part->get_matrix()));

    // Replacing the host mesh with an equal one keeps the paint and shares the new mesh.
    const auto data = scene.painted_modifier->painted_modifier_facets.get_data();
    scene.part->set_mesh(TriangleMesh(scene.part->mesh()));
    CHECK(scene.object->sync_painted_modifiers());
    CHECK(scene.painted_modifier->mesh_ptr() == scene.part->mesh_ptr());
    CHECK(scene.painted_modifier->painted_modifier_facets.get_data() == data);
    CHECK_FALSE(scene.object->sync_painted_modifiers());
}

TEST_CASE("A painted modifier stays on its host when the host gets a new ID", "[PaintedModifier]")
{
    Scene scene;
    // Mesh edits such as simplify or repair renew the ID of the edited part.
    scene.part->set_new_unique_id();
    CHECK(scene.object->painted_modifier_host(*scene.painted_modifier) == scene.part);
}

TEST_CASE("Replacing the mesh of a painted modifier keeps its paint", "[PaintedModifier]")
{
    Scene scene;
    REQUIRE_THAT(painted_area(*scene.painted_modifier), WithinAbs(20. * 20., 1e-3));
    const auto saved = scene.painted_modifier->save_painting();
    REQUIRE(saved.has_value());
    // Like the mesh operations, rebuild the mesh from its triangles.
    scene.painted_modifier->set_mesh(TriangleMesh(scene.part->mesh().its));
    scene.painted_modifier->restore_painting(saved);
    CHECK_THAT(painted_area(*scene.painted_modifier), WithinAbs(20. * 20., 1.));
}

TEST_CASE("A cloned object keeps its painted modifiers on their hosts", "[PaintedModifier]")
{
    Scene scene;
    Model copy;
    ModelObject *object = copy.add_object(*scene.object);
    REQUIRE(object->volumes.size() == 2);
    CHECK(object->volumes[0]->id() != scene.part->id());
    CHECK(object->painted_modifier_host(*object->volumes[1]) == object->volumes[0]);
}
