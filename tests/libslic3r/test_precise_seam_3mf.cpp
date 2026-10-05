#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include "test_utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"

#include <algorithm>
#include <array>
#include "libslic3r/Config.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <cstddef>
#include "libslic3r/Geometry.hpp"
#include <miniz.h>
#include <string>
#include <utility>
#include <vector>
#include "libslic3r/Point.hpp"

using namespace Slic3r;

namespace {
enum class Backend { Bbs, Prusa };
constexpr std::array<ModelVolumeType, 6> seam_types = {
    ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_LEFT,
    ModelVolumeType::PRECISE_SEAM_RIGHT, ModelVolumeType::PRECISE_SEAM_ENFORCED,
    ModelVolumeType::PRECISE_SEAM_BLOCKED, ModelVolumeType::PRECISE_SEAM_NEUTRAL
};
// Literal entities and backslashes distinguish XML escaping from config serialization.
const std::string notes = "quoted \"value\" & <tag>\tcolumn\nnext line &amp; path\\file";

struct Scene {
    // Keep the backup directory alive longer than its model; no project files are touched.
    ScopedTemporaryDir backup{"orca_seam_3mf"};
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    bool share_mesh = false;

    Scene()
    {
        model.set_backup_path(backup.string());
        // Import normalizes extruder indices against this list; explicitly provide filament 2.
        config.set_key_value("filament_settings_id", new ConfigOptionStrings(std::vector<std::string>{"A", "B"}));
    }

    void populate(bool all_modes, bool shared_mesh = false)
    {
        share_mesh = shared_mesh;
        auto *object = model.add_object();
        object->name = "seam round trip";
        auto *part = object->add_volume(make_cube(20, 20, 2));
        part->name = "printable";
        ModelVolume *first_helper = nullptr;
        for (size_t i = 0; i < (all_modes ? seam_types.size() : size_t(1)); ++i) {
            auto *volume = shared_mesh && first_helper ? object->add_volume_with_shared_mesh(*first_helper) :
                                                        object->add_volume(make_cube(2, 3, 4));
            if (!first_helper) first_helper = volume;
            volume->name = "helper_" + std::to_string(i);
            volume->set_type(seam_types[i]);
            Geometry::Transformation transform;
            transform.set_offset(Vec3d(3.0 * double(i), -2.0, 1.0));
            transform.set_rotation(Vec3d(0.0, 0.0, 0.1 * double(i + 1)));
            transform.set_scaling_factor(Vec3d(1.0, 1.2, 0.8));
            volume->set_transformation(transform);
            // Simulate settings retained by conversion from a part/modifier into a seam helper.
            volume->config.set_key_value("extruder", new ConfigOptionInt(2));
            volume->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100.0 - 5.0 * double(i)));
            volume->config.set_key_value("notes", new ConfigOptionString(notes + std::to_string(i)));
        }
        object->add_instance();
    }
};

void save(const std::string &path, Backend backend, Scene &scene)
{
    if (backend == Backend::Prusa) {
        REQUIRE(store_3mf(path.c_str(), &scene.model, &scene.config, false));
    } else {
        StoreParams params;
        params.path = path.c_str();
        params.model = &scene.model;
        params.config = &scene.config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        if (scene.share_mesh) params.strategy = params.strategy | SaveStrategy::ShareMesh;
        REQUIRE(store_bbs_3mf(params));
    }
}

void load(const std::string &path, Backend backend, Scene &scene)
{
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    if (backend == Backend::Prusa) {
        REQUIRE(load_3mf(path.c_str(), scene.config, substitutions, &scene.model, false));
    } else {
        struct ImportedResources {
            PlateDataPtrs plates;
            std::vector<Preset*> presets;
            // Catch failures must not leak importer-owned auxiliary objects.
            ~ImportedResources() { release_PlateData_list(plates); for (auto *preset : presets) delete preset; }
        } resources;
        bool bbs = false, orca = false;
        Semver version;
        REQUIRE(load_bbs_3mf(path.c_str(), &scene.config, &substitutions, &scene.model, &resources.plates,
                            &resources.presets, &bbs, &orca, &version, nullptr,
                            LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    }
}

using Archive = std::vector<std::pair<std::string, std::string>>;
Archive read_archive(const std::string &path)
{
    struct Reader {
        mz_zip_archive zip{};
        ~Reader() { if (zip.m_pState) close_zip_reader(&zip); }
    } reader;
    REQUIRE(open_zip_reader(&reader.zip, path));
    Archive entries;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader.zip); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&reader.zip, i, &stat));
        if (stat.m_is_directory) continue;
        std::string name(stat.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        std::string data(size_t(stat.m_uncomp_size), '\0');
        if (!data.empty()) REQUIRE(mz_zip_reader_extract_to_mem(&reader.zip, i, data.data(), data.size(), 0));
        entries.emplace_back(std::move(name), std::move(data));
    }
    return entries;
}

std::string &model_xml(Archive &archive, Backend backend)
{
    const std::string name = backend == Backend::Bbs ? "Metadata/model_settings.config" : "Metadata/Slic3r_PE_model.config";
    const auto found = std::find_if(archive.begin(), archive.end(), [&](const auto &entry) { return entry.first == name; });
    REQUIRE(found != archive.end());
    return found->second;
}

void write_archive(const std::string &path, const Archive &archive)
{
    // Repack a separate temporary archive, preserving every entry except explicitly edited metadata.
    Zipper zipper(path);
    for (const auto &entry : archive) zipper.add_entry(entry.first, entry.second.data(), entry.second.size());
    zipper.finalize();
}

void replace_once(std::string &text, const std::string &from, const std::string &to)
{
    const auto pos = text.find(from);
    REQUIRE(pos != std::string::npos);
    REQUIRE(text.find(from, pos + from.size()) == std::string::npos);
    text.replace(pos, from.size(), to);
}

std::string remove_mode_metadata(std::string &xml)
{
    // Mutations must locate exactly one real metadata element; silent no-op rewrites would give false confidence.
    const auto key = xml.find("key=\"precise_seam_type\"");
    REQUIRE(key != std::string::npos);
    REQUIRE(xml.find("key=\"precise_seam_type\"", key + 1) == std::string::npos);
    const auto begin = xml.rfind("<metadata ", key);
    const auto end = xml.find("/>", key);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    const std::string result = xml.substr(begin, end + 2 - begin);
    xml.erase(begin, result.size());
    return result;
}

void check_config(const ModelVolume &volume, size_t index = 0)
{
    REQUIRE(volume.config.has("extruder"));
    CHECK(volume.config.opt_int("extruder") == 2);
    REQUIRE(volume.config.has("sparse_infill_density"));
    CHECK_THAT(volume.config.opt_float("sparse_infill_density"), Catch::Matchers::WithinAbs(100.0 - 5.0 * double(index), 1e-9));
    REQUIRE(volume.config.has("notes"));
    CHECK(volume.config.get().opt_string("notes") == notes + std::to_string(index));
}

void check_modifier_config(const ModelVolume &volume, Backend backend, size_t index = 0)
{
    if (backend == Backend::Bbs) {
        check_config(volume, index);
    } else {
        // The Prusa importer whitelists extruder, but drops ordinary notes and infill settings.
        REQUIRE(volume.config.has("extruder"));
        CHECK(volume.config.opt_int("extruder") == 2);
        CHECK_FALSE(volume.config.has("sparse_infill_density"));
        CHECK_FALSE(volume.config.has("notes"));
    }
}

void check_geometry(const ModelVolume &before, const ModelVolume &after)
{
    // Importers may recenter a mesh and compensate in its transform; compare transformed vertices.
    REQUIRE(after.mesh().its.vertices.size() == before.mesh().its.vertices.size());
    CHECK(after.mesh().its.indices.size() == before.mesh().its.indices.size());
    std::vector<Vec3d> expected;
    for (const auto &v : before.mesh().its.vertices) expected.push_back(before.get_matrix() * v.cast<double>());
    for (const auto &v : after.mesh().its.vertices) {
        const Vec3d actual = after.get_matrix() * v.cast<double>();
        const auto match = std::find_if(expected.begin(), expected.end(), [&](const Vec3d &p) { return (p - actual).norm() < 1e-4; });
        REQUIRE(match != expected.end());
        expected.erase(match); // Match multiplicities, not merely membership in the vertex set.
    }
    CHECK(expected.empty());
}
} // namespace

TEST_CASE("All Precise Seam types and dormant settings survive a 3MF round trip", "[PreciseSeam3mf]")
{
    const auto backend = GENERATE(Backend::Bbs, Backend::Prusa);
    const bool shared = GENERATE(false, true);
    CAPTURE(int(backend), shared);
    Scene source;
    source.populate(true, shared);
    ScopedTemporaryFile file(".3mf");
    save(file.string(), backend, source);
    Archive archive = read_archive(file.string());
    const auto &xml = model_xml(archive, backend);
    const std::string open = backend == Backend::Bbs ? "<part " : "<volume ";
    const std::string close = backend == Backend::Bbs ? "</part>" : "</volume>";
    size_t pos = 0, helpers = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        const auto end = xml.find(close, pos);
        REQUIRE(end != std::string::npos);
        const auto block = xml.substr(pos, end - pos);
        if (block.find("key=\"precise_seam_type\"") != std::string::npos) {
            ++helpers;
            CHECK(block.find(backend == Backend::Bbs ? "subtype=\"modifier_part\"" : "value=\"ParameterModifier\"") != std::string::npos);
            for (const std::string key : {"extruder", "sparse_infill_density", "notes"}) {
                CAPTURE(key);
                CHECK(block.find("key=\"" + key + "\"") == std::string::npos);
                CHECK(block.find("key=\"precise_seam_config:" + key + "\"") != std::string::npos);
            }
        }
        pos = end + close.size();
    }
    REQUIRE(helpers == seam_types.size());
    Scene destination;
    load(file.string(), backend, destination);
    REQUIRE(destination.model.objects.size() == 1);
    const auto &volumes = destination.model.objects.front()->volumes;
    REQUIRE(volumes.size() == 1 + seam_types.size());
    CHECK(volumes.front()->is_model_part());
    for (size_t i = 0; i < seam_types.size(); ++i) {
        CAPTURE(i);
        CHECK(volumes[i + 1]->type() == seam_types[i]);
        CHECK(volumes[i + 1]->name == "helper_" + std::to_string(i));
        check_geometry(*source.model.objects.front()->volumes[i + 1], *volumes[i + 1]);
        check_config(*volumes[i + 1], i);
        if (shared && backend == Backend::Bbs)
            CHECK(volumes[i + 1]->mesh_ptr().get() == volumes[1]->mesh_ptr().get());
        volumes[i + 1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    }

    // Reverse conversion must survive another file round trip, not just retain config in memory.
    destination.share_mesh = shared;
    ScopedTemporaryFile converted_file(".3mf");
    save(converted_file.string(), backend, destination);
    Archive converted_archive = read_archive(converted_file.string());
    const auto &converted_xml = model_xml(converted_archive, backend);
    CHECK(converted_xml.find("key=\"precise_seam_type\"") == std::string::npos);
    CHECK(converted_xml.find("key=\"precise_seam_config:") == std::string::npos);
    Scene converted;
    load(converted_file.string(), backend, converted);
    REQUIRE(converted.model.objects.size() == 1);
    const auto &converted_volumes = converted.model.objects.front()->volumes;
    REQUIRE(converted_volumes.size() == volumes.size());
    for (size_t i = 0; i < seam_types.size(); ++i) {
        CAPTURE(i);
        CHECK(converted_volumes[i + 1]->type() == ModelVolumeType::PARAMETER_MODIFIER);
        CHECK(converted_volumes[i + 1]->name == "helper_" + std::to_string(i));
        check_geometry(*volumes[i + 1], *converted_volumes[i + 1]);
        check_modifier_config(*converted_volumes[i + 1], backend, i);
    }
}

TEST_CASE("Ordinary modifier settings are escaped once in 3MF attributes", "[PreciseSeam3mf][Regression]")
{
    const auto backend = GENERATE(Backend::Bbs, Backend::Prusa);
    CAPTURE(int(backend));
    Scene source;
    source.populate(false);
    source.model.objects.front()->volumes[1]->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    ScopedTemporaryFile file(".3mf");
    save(file.string(), backend, source);
    Archive archive = read_archive(file.string());
    const auto &xml = model_xml(archive, backend);
    // Check the serialized value independently of the writer's escaping helper. Prusa drops
    // ordinary notes on import, so successful loading alone would not detect double escaping.
    const std::string encoded_notes = R"(quoted \&quot;value\&quot; &amp; &lt;tag>&#x9;column\nnext line &amp;amp; path\\file0)";
    CHECK(xml.find("key=\"notes\" value=\"" + encoded_notes + "\"") != std::string::npos);
    Scene destination;
    load(file.string(), backend, destination);
    REQUIRE(destination.model.objects.size() == 1);
    REQUIRE(destination.model.objects.front()->volumes.size() == 2);
    const ModelVolume &modifier = *destination.model.objects.front()->volumes[1];
    CHECK(modifier.type() == ModelVolumeType::PARAMETER_MODIFIER);
    check_modifier_config(modifier, backend);
}

TEST_CASE("Seam metadata restores only recognized modes on compatible base types", "[PreciseSeam3mf][Regression]")
{
    const auto backend = GENERATE(Backend::Bbs, Backend::Prusa);
    // Include both XML orders, fallback cases, and the previous inline type representation.
    const int variant = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(int(backend), variant);
    Scene source;
    source.populate(false);
    ScopedTemporaryFile original(".3mf"), edited(".3mf");
    save(original.string(), backend, source);
    Archive archive = read_archive(original.string());
    auto &xml = model_xml(archive, backend);
    if (variant == 0 || variant == 1) {
        const std::string metadata = remove_mode_metadata(xml);
        const std::string tag = backend == Backend::Bbs ? "part" : "volume";
        // Locate the helper via its dormant config, then move its mode before/after base metadata.
        const auto key = xml.find("key=\"precise_seam_config:extruder\"");
        REQUIRE(key != std::string::npos);
        const auto start = xml.rfind("<" + tag + " ", key);
        REQUIRE(start != std::string::npos);
        if (backend == Backend::Bbs) {
            // Exercise base-type metadata too, not just the subtype attribute preceding all metadata.
            const auto end = xml.find("</part>", key);
            REQUIRE(end != std::string::npos);
            xml.insert(end, "<metadata key=\"part_type\" value=\"modifier_part\"/>");
        }
        const auto opening_end = xml.find('>', start);
        REQUIRE(opening_end != std::string::npos);
        const auto insertion = variant == 0 ? opening_end + 1 : xml.find("</" + tag + ">", key);
        REQUIRE(insertion != std::string::npos);
        xml.insert(insertion, metadata);
    } else if (variant == 2) {
        replace_once(xml, "value=\"precise_seam_center\"", "value=\"unknown_future_seam\"");
    } else if (variant == 3) {
        remove_mode_metadata(xml);
    } else if (variant == 4) {
        // A known seam mode must not reinterpret an ordinary printable part.
        if (backend == Backend::Bbs) replace_once(xml, "subtype=\"modifier_part\"", "subtype=\"normal_part\"");
        else {
            replace_once(xml, "key=\"modifier\" value=\"1\"", "key=\"modifier\" value=\"0\"");
            replace_once(xml, "value=\"ParameterModifier\"", "value=\"ModelPart\"");
        }
    } else {
        remove_mode_metadata(xml);
        if (backend == Backend::Bbs) replace_once(xml, "subtype=\"modifier_part\"", "subtype=\"precise_seam_center\"");
        else replace_once(xml, "value=\"ParameterModifier\"", "value=\"precise_seam_center\"");
        // Older files stored settings as ordinary volume keys, without the new dormant namespace.
        for (const std::string key : {"extruder", "sparse_infill_density", "notes"})
            replace_once(xml, "key=\"precise_seam_config:" + key + "\"", "key=\"" + key + "\"");
    }
    write_archive(edited.string(), archive);
    Scene destination;
    load(edited.string(), backend, destination);
    REQUIRE(destination.model.objects.size() == 1);
    REQUIRE(destination.model.objects.front()->volumes.size() == 2);
    const ModelVolume &helper = *destination.model.objects.front()->volumes[1];
    if (variant == 0 || variant == 1 || variant == 5) {
        CHECK(helper.type() == ModelVolumeType::PRECISE_SEAM_CENTER);
        if (variant == 5 && backend == Backend::Prusa) {
            // The legacy Prusa whitelist only accepted extruder among these ordinary setting keys.
            REQUIRE(helper.config.has("extruder"));
            CHECK(helper.config.opt_int("extruder") == 2);
            CHECK_FALSE(helper.config.has("sparse_infill_density"));
            CHECK_FALSE(helper.config.has("notes"));
        } else check_config(helper);
    } else {
        CHECK(helper.type() == (variant == 4 ? ModelVolumeType::MODEL_PART : ModelVolumeType::PARAMETER_MODIFIER));
        CHECK_FALSE(helper.config.has("extruder"));
        CHECK_FALSE(helper.config.has("sparse_infill_density"));
        CHECK_FALSE(helper.config.has("notes"));
    }
}
