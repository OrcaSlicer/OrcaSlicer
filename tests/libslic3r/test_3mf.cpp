
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/ProjectTask.hpp"

#include <boost/filesystem/operations.hpp>

#include <catch2/catch_tostring.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <type_traits> // for std::enable_if_t
#include <typeinfo>    // for typeid

namespace Catch {
    template <typename T>
    struct is_eigen_matrix : std::is_base_of<Eigen::MatrixBase<T>, T> {};

    template <typename T>
    struct StringMaker<T, std::enable_if_t<is_eigen_matrix<T>::value>> {
        static std::string convert(const T& eigen_obj) {
            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            ss << "Matrix<" << typeid(eigen_obj).name() << "> = \n";
            ss << eigen_obj.format(fmt);
            return ss.str();
        }
    };
    
    // We must manually specialize for Eigen::Transform as it doesn't derive from MatrixBase.
    // It's defined as: Eigen::Transform<Scalar, Dim, Mode, Options>
    template <typename Scalar, int Dim, int Mode, int Options>
    struct StringMaker<Eigen::Transform<Scalar, Dim, Mode, Options>> {
        static std::string convert(const Eigen::Transform<Scalar, Dim, Mode, Options>& trafo) {
            // We print the underlying matrix 
            const auto& matrix = trafo.matrix();

            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            
            ss << "Transform<Mode=" << Mode << ", Dim=" << Dim << "> = \n"; 
            ss << matrix.format(fmt);
            return ss.str();
        }
    };
    
    // Quaternions also need an explicit specialization
    template <typename Scalar, int Options>
    struct StringMaker<Eigen::Quaternion<Scalar, Options>> {
        static std::string convert(const Eigen::Quaternion<Scalar, Options>& quat) {
            std::stringstream ss;
            ss << "Quaternion(w=" << quat.w() << ", x=" << quat.x() << ", y=" << quat.y() << ", z=" << quat.z() << ")";
            return ss.str();
        }
    };
} // end namespace Catch

#include <catch2/catch_all.hpp>

using namespace Slic3r;


SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
            std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
            DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

// .3mf multi-nozzle round-trip.
// Locks the load/save handling for the H2C multi-nozzle plate metadata:
//   * filament_volume_maps  -> plate config "filament_volume_map" (with the >1 -> 0 clamp)
//   * nozzle_volume_type    -> PlateData::nozzle_volume_types (previously write-only)
// and pins the deliberately-lossy keys (enable_filament_dynamic_map) so a future change has to
// consciously unpin them. Uses a store_bbs_3mf -> load_bbs_3mf cycle (no external fixture needed).
SCENARIO("H2C multi-nozzle .3mf round-trip", "[3mf][MultiNozzle]") {
    GIVEN("a plate carrying multi-nozzle filament assignment metadata") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        // store_bbs_3mf stages Metadata/project_settings.config through the model's backup path;
        // point it at a writable temp dir (the default lives under a read-only root in CI).
        std::string backup_dir =
            (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_mn_%%%%%%%%")).string();
        boost::filesystem::create_directories(backup_dir);
        model.set_backup_path(backup_dir);

        // Global (printer) config: give nozzle_volume_type a non-default value so the slice_info
        // read-back is a meaningful assertion (High Flow == 1).
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("nozzle_volume_type",
                             new ConfigOptionEnumsGeneric({ (int) NozzleVolumeType::nvtHighFlow }));

        PlateData* plate = new PlateData();
        plate->plate_index      = 0;
        plate->is_sliced_valid  = true; // gate for the slice_info.config writer (nozzle_volume_type)
        plate->filament_maps    = { 1, 2, 1 }; // slice_info uses this; keep it == model_settings' value
        plate->config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
        plate->config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 1 }));
        // Deliberately include out-of-range volume-type ids (2 == Hybrid, 3 == TPU High Flow):
        // the loader must clamp them back to Standard (0).
        plate->config.set_key_value("filament_volume_map", new ConfigOptionInts({ 0, 2, 1, 3 }));
        // Known-lossy: a true value must NOT survive the round-trip (slice_info hardcodes false,
        // model_settings never writes it).
        plate->config.set_key_value("enable_filament_dynamic_map", new ConfigOptionBool(true));

        WHEN("stored to and reloaded from a .3mf") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/mn_roundtrip.3mf";

            StoreParams store_params;
            store_params.path    = test_file.c_str();
            store_params.model   = &model;
            store_params.config  = &config;
            store_params.plate_data_list.push_back(plate);
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            // LoadConfig is required for slice_info.config (nozzle_volume_type) to be parsed —
            // matches how the app loads projects.
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                       &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
            boost::filesystem::remove(test_file);

            THEN("every multi-nozzle key round-trips as expected") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() >= 1);
                PlateData* rt = dst_plates.front();

                // filament_map (model_settings + slice_info; already round-tripped)
                auto* fmap = rt->config.option<ConfigOptionInts>("filament_map");
                REQUIRE(fmap != nullptr);
                REQUIRE(fmap->values == std::vector<int>({ 1, 2, 1 }));

                // filament_volume_map (model_settings) with the >1 -> 0 clamp
                auto* fvmap = rt->config.option<ConfigOptionInts>("filament_volume_map");
                REQUIRE(fvmap != nullptr);
                REQUIRE(fvmap->values == std::vector<int>({ 0, 0, 1, 0 }));

                // nozzle_volume_type read-back into PlateData::nozzle_volume_types
                REQUIRE(rt->nozzle_volume_types == "1");

                // enable_filament_dynamic_map pinned lossy: model_settings never serializes it and
                // slice_info hardcodes false, so the `true` we set is dropped. Pinned here
                // (absent or false, never true) so a future change that persists it must update this.
                auto* dyn = rt->config.option<ConfigOptionBool>("enable_filament_dynamic_map");
                const bool persisted_true = (dyn != nullptr && dyn->value);
                REQUIRE_FALSE(persisted_true);
            }

            release_PlateData_list(dst_plates);
        }
        delete plate; // store_bbs_3mf does not take ownership of the source plate
        boost::filesystem::remove_all(backup_dir);
    }
}

// Saved nozzle diameter for a single-nozzle-per-extruder printer with a non-standard nozzle.
// The grouping result rounds every nozzle diameter to the nearest of {0.2,0.4,0.6,0.8} for its
// internal matching key. That rounded value must NOT reach the saved <filament>/<nozzle> metadata on
// a printer whose extruders each carry one nozzle: the exact per-extruder config diameter is written
// instead, so a 0.5 mm nozzle is preserved rather than saved as 0.4. (Only an extruder that carries a
// nozzle cluster, which the per-extruder config cannot express, keeps the grouping result's diameter.)
SCENARIO("Non-standard nozzle diameter survives .3mf save on a single-nozzle printer", "[3mf][MultiNozzle]") {
    GIVEN("a single-extruder plate whose nozzle is 0.5 mm and whose stamped diameter was rounded to 0.4") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        std::string backup_dir =
            (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_nd_%%%%%%%%")).string();
        boost::filesystem::create_directories(backup_dir);
        model.set_backup_path(backup_dir);

        // Single extruder with a non-standard 0.5 mm nozzle; extruder_max_nozzle_count stays at its
        // default (no nozzle cluster), so the writer must emit the exact config diameter.
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.5 }));

        PlateData* plate = new PlateData();
        plate->plate_index     = 0;
        plate->is_sliced_valid = true;      // gate for the slice_info.config writer
        plate->filament_maps   = { 1 };

        // Seed the stamped diameter with the grouping result's rounded value (0.5 -> 0.4) so the
        // assertion proves the writer ignores it and emits the exact config diameter instead.
        FilamentInfo fi;
        fi.id              = 0;
        fi.type            = "PLA";
        fi.color           = "#FFFFFFFF";
        fi.group_id        = { 0 };
        fi.nozzle_diameter = 0.4; // rounded; must NOT be the value written
        plate->slice_filaments_info.push_back(fi);

        WHEN("stored to and reloaded from a .3mf") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/nd_roundtrip.3mf";

            StoreParams store_params;
            store_params.path    = test_file.c_str();
            store_params.model   = &model;
            store_params.config  = &config;
            store_params.plate_data_list.push_back(plate);
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                       &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
            boost::filesystem::remove(test_file);

            THEN("the saved nozzle diameter is the exact 0.5, not the rounded 0.4") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() >= 1);
                PlateData* rt = dst_plates.front();

                // <nozzle> tag: device-facing per-nozzle diameter string, written verbatim.
                REQUIRE(rt->nozzles_info.size() >= 1);
                REQUIRE(rt->nozzles_info.front().diameter == "0.5");

                // <filament> tag: per-filament nozzle_diameter parsed back as 0.5, not 0.4.
                REQUIRE(rt->slice_filaments_info.size() >= 1);
                REQUIRE_THAT(rt->slice_filaments_info.front().nozzle_diameter, Catch::Matchers::WithinAbs(0.5, 1e-6));
            }

            release_PlateData_list(dst_plates);
        }
        delete plate; // store_bbs_3mf does not take ownership of the source plate
        boost::filesystem::remove_all(backup_dir);
    }
}

// A legacy / foreign project (no multi-nozzle metadata) must load crash-safe through the BBS
// importer and must not fabricate a filament_volume_map.
SCENARIO("Legacy project loads crash-safe via load_bbs_3mf", "[3mf][MultiNozzle]") {
    GIVEN("a project without any multi-nozzle metadata") {
        std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        Model                model;
        DynamicPrintConfig   config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
        PlateDataPtrs        plates;
        std::vector<Preset*> project_presets;
        bool   is_bbl_3mf = false, is_orca_3mf = false;
        Semver file_version;

        WHEN("loaded through the BBS importer") {
            bool loaded = false;
            REQUIRE_NOTHROW(loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates,
                                                  &project_presets, &is_bbl_3mf, &is_orca_3mf,
                                                  &file_version, nullptr,
                                                  LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
            THEN("it does not crash and invents no per-filament volume map") {
                for (PlateData* p : plates) {
                    REQUIRE(p->config.option<ConfigOptionInts>("filament_volume_map") == nullptr);
                }
            }
            release_PlateData_list(plates);
        }
    }
}

// Device-side nozzle-grouping serialization surface.
// Direct unit coverage for the pure serialize/deserialize + StaticNozzleGroupResult helpers that the
// gcode.3mf writer/reader lean on.
SCENARIO("MultiNozzle serialization helpers", "[3mf][MultiNozzle]") {
    using namespace Slic3r::MultiNozzleUtils;

    GIVEN("NozzleInfo / NozzleGroupInfo") {
        NozzleInfo n0; n0.group_id = 0; n0.extruder_id = 0; n0.diameter = "0.4"; n0.volume_type = nvtStandard;
        NozzleInfo n1; n1.group_id = 1; n1.extruder_id = 1; n1.diameter = "0.4"; n1.volume_type = nvtHighFlow;

        THEN("NozzleInfo::serialize matches the <nozzle> tag attributes (extruder_id 1-based)") {
            REQUIRE(n0.serialize() == "id=\"0\" extruder_id=\"1\" nozzle_diameter=\"0.4\" volume_type=\"Standard\"");
            REQUIRE(n1.serialize() == "id=\"1\" extruder_id=\"2\" nozzle_diameter=\"0.4\" volume_type=\"High Flow\"");
        }
        THEN("NozzleGroupInfo serialize/deserialize round-trips and rejects malformed input") {
            NozzleGroupInfo g("0.4", nvtHighFlow, 1, 3);
            REQUIRE(g.serialize() == "1-0.4-High Flow-3");
            auto rt = NozzleGroupInfo::deserialize(g.serialize());
            REQUIRE(rt.has_value());
            REQUIRE(*rt == g);
            REQUIRE_FALSE(NozzleGroupInfo::deserialize("1-0.4-Standard").has_value()); // too few tokens
            REQUIRE_FALSE(NozzleGroupInfo::deserialize("x-0.4-Standard-3").has_value()); // non-numeric extruder
        }
    }

    GIVEN("a StaticNozzleGroupResult built from filament + nozzle infos") {
        std::vector<NozzleInfo> nozzles;
        { NozzleInfo n; n.group_id = 0; n.extruder_id = 0; n.diameter = "0.4"; n.volume_type = nvtStandard; nozzles.push_back(n); }
        { NozzleInfo n; n.group_id = 1; n.extruder_id = 1; n.diameter = "0.4"; n.volume_type = nvtHighFlow; nozzles.push_back(n); }

        std::vector<FilamentInfo> filaments(3);
        filaments[0].id = 0; filaments[0].group_id = { 0 };
        filaments[1].id = 1; filaments[1].group_id = { 1 };
        filaments[2].id = 2; filaments[2].group_id = { 0, 1 };

        auto result = StaticNozzleGroupResult::create(filaments, nozzles, { 0, 1, 2 }, { 0, 1, 0 }, false);
        REQUIRE(result.has_value());

        THEN("filament->nozzle queries resolve to the stored mapping") {
            REQUIRE(result->get_extruder_count() == 2);
            REQUIRE(result->get_used_extruders() == std::vector<int>({ 0, 1 }));
            REQUIRE(result->get_used_filaments() == std::vector<unsigned int>({ 0, 1, 2 }));
            REQUIRE(result->get_nozzles_for_filament(0).size() == 1);
            REQUIRE(result->get_nozzles_for_filament(2).size() == 2);
            // first-use resolves through the (filament,nozzle) change sequences.
            auto first = result->get_first_nozzle_for_filament(1);
            REQUIRE(first.has_value());
            REQUIRE(first->group_id == 1);
        }
        THEN("empty inputs yield nullopt") {
            REQUIRE_FALSE(StaticNozzleGroupResult::create({}, nozzles, {}, {}, false).has_value());
            REQUIRE_FALSE(StaticNozzleGroupResult::create(filaments, {}, {}, {}, false).has_value());
        }
    }

    GIVEN("load_nozzle_infos_with_compatibility fallbacks") {
        std::vector<NozzleInfo> new_format;
        { NozzleInfo n; n.group_id = 1; n.extruder_id = 1; n.diameter = "0.4"; n.volume_type = nvtHighFlow; new_format.push_back(n); }
        { NozzleInfo n; n.group_id = 0; n.extruder_id = 0; n.diameter = "0.4"; n.volume_type = nvtStandard; new_format.push_back(n); }

        THEN("new-format <nozzle> tags are returned sorted by logical id") {
            auto out = load_nozzle_infos_with_compatibility(new_format, {}, {}, {}, {});
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].group_id == 0);
            REQUIRE(out[1].group_id == 1);
        }
        THEN("oldest single-nozzle 3mf (no tags, no filament group_id) rebuilds from diameters/volume types") {
            std::vector<NozzleVolumeType> vt = { nvtStandard, nvtHighFlow };
            std::vector<double>           dia = { 0.4, 0.4 };
            auto out = load_nozzle_infos_with_compatibility({}, {}, {}, vt, dia);
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].extruder_id == 0);
            REQUIRE(out[0].volume_type == nvtStandard);
            REQUIRE(out[1].volume_type == nvtHighFlow);
        }
    }
}

// The layer-aware grouping result must survive the gcode.3mf write/read as
// <nozzle> tags and the enable_filament_dynamic_map flag. Proves the parse_filament_info stamping,
// the NOZZLE_TAG writer, the _handle_config_nozzle reader, and the nozzles_info plate copy.
SCENARIO("Nozzle-group metadata .3mf round-trip", "[3mf][MultiNozzle]") {
    GIVEN("a plate carrying a two-nozzle LayeredNozzleGroupResult") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        std::string backup_dir =
            (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_ng_%%%%%%%%")).string();
        boost::filesystem::create_directories(backup_dir);
        model.set_backup_path(backup_dir);

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

        std::vector<MultiNozzleUtils::NozzleInfo> nozzles;
        { MultiNozzleUtils::NozzleInfo n; n.group_id = 0; n.extruder_id = 0; n.diameter = "0.4"; n.volume_type = NozzleVolumeType::nvtStandard; nozzles.push_back(n); }
        { MultiNozzleUtils::NozzleInfo n; n.group_id = 1; n.extruder_id = 1; n.diameter = "0.4"; n.volume_type = NozzleVolumeType::nvtHighFlow; nozzles.push_back(n); }
        auto group = MultiNozzleUtils::LayeredNozzleGroupResult::create(
            std::vector<int>{ 0, 1, 0 }, nozzles, std::vector<unsigned int>{ 0, 1, 2 });
        REQUIRE(group.has_value());

        PlateData* plate = new PlateData();
        plate->plate_index     = 0;
        plate->is_sliced_valid = true;
        plate->filament_maps   = { 1, 2, 1 };
        plate->nozzle_group_result = group;
        plate->config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
        plate->config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 1 }));

        WHEN("stored to and reloaded from a .3mf") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/ng_roundtrip.3mf";

            StoreParams store_params;
            store_params.path    = test_file.c_str();
            store_params.model   = &model;
            store_params.config  = &config;
            store_params.plate_data_list.push_back(plate);
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                       &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
            boost::filesystem::remove(test_file);

            THEN("the <nozzle> tags round-trip into the loaded plate's nozzles_info") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() >= 1);
                PlateData* rt = dst_plates.front();

                REQUIRE(rt->nozzles_info.size() == 2);
                // reader stores extruder_id 0-based (tag is 1-based), diameter/volume_type preserved.
                std::sort(rt->nozzles_info.begin(), rt->nozzles_info.end());
                REQUIRE(rt->nozzles_info[0].group_id == 0);
                REQUIRE(rt->nozzles_info[0].extruder_id == 0);
                REQUIRE(rt->nozzles_info[0].diameter == "0.4");
                REQUIRE(rt->nozzles_info[0].volume_type == NozzleVolumeType::nvtStandard);
                REQUIRE(rt->nozzles_info[1].group_id == 1);
                REQUIRE(rt->nozzles_info[1].extruder_id == 1);
                REQUIRE(rt->nozzles_info[1].volume_type == NozzleVolumeType::nvtHighFlow);

                // A static (non-selector) result must persist enable_filament_dynamic_map = false.
                auto* dyn = rt->config.option<ConfigOptionBool>("enable_filament_dynamic_map");
                const bool persisted_true = (dyn != nullptr && dyn->value);
                REQUIRE_FALSE(persisted_true);
            }

            release_PlateData_list(dst_plates);
        }
        delete plate;
        boost::filesystem::remove_all(backup_dir);
    }
}

SCENARIO("2D convex hull of sinking object", "[3mf][.]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects[0];
            object->center_around_origin(false);

	    // This outputs the same exact data as the Prusaslicer test
	    object->volumes[0]->mesh().write_ascii("/tmp/orca.ascii");

            // set instance's attitude so that it is rotated, scaled (and sinking? how is it sinking? the rotation? does it matter if it's sinking?)
            ModelInstance* instance = object->instances[0];
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
	    auto trafo = instance->get_transformation().get_matrix();

	    // This matrix is the same exact matrix as the Prusaslicer test
	    CAPTURE(trafo);
            Polygon hull_2d = object->convex_hull_2d(trafo);

	    // But we get different hull_2d.points here (and somehow decimal numbers despite being int64_t values, but that's probabaly printing configuration somewhere -- Prusaslicer's prints out with newlines between the X&Y and not one between coordinates, which is about the worse possible output).
	    // I think it's something to do with PrusaSlicer ignoring everything under the Z plane, which makes sense from the results.
	    // See the comments added to ModelObject::convex_hull_2d for more information.

            // verify result
            Points result = {
                { -91501496, -15914144 },
                { 91501496, -15914144 },
                { 91501496, 4243 },
                { 78229680, 4246883 },
                { 56898100, 4246883 },
                { -85501496, 4242641 },
                { -91501496, 4243 }
            };

            THEN("2D convex hull should match with reference") {
                // Allow 1um error due to floating point rounding.
                bool res = hull_2d.points.size() == result.size();
                if (res) {
                    for (size_t i = 0; i < result.size(); ++ i) {
                        const Point &p1 = result[i];
                        const Point &p2 = hull_2d.points[i];
                        CHECK((std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1));
                    }
                }

                CAPTURE(hull_2d.points);
                REQUIRE(res);
            }
        }
    }
}
