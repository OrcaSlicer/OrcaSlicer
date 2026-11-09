#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/GeometryImportDialog.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Zipper.hpp"
#include "test_utils.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("CAD 3MF routing recognizes project markers without reading geometry", "[GeometryImport]")
{
    const std::string marker = GENERATE(std::string(), std::string("Metadata/model_settings.config"),
        std::string("Metadata/project_settings.config"), std::string("Metadata/Slic3r_PE.config"),
        std::string("Metadata/Slic3r_PE_model.config"));
    CAPTURE(marker);
    ScopedTemporaryFile file(".3mf");
    Zipper archive(file.string());
    // Directory-only classification must not triangulate or parse this placeholder mesh.
    archive.add_entry("3D/3dmodel.model");
    archive << "<model/>";
    if (!marker.empty()) {
        archive.add_entry(marker);
        archive << "{}";
    }
    archive.finalize();
    CHECK(is_geometry_only_3mf(file.string()) == marker.empty());
}

TEST_CASE("Geometry import choices distinguish remembering from suppressing questions", "[GeometryImport]")
{
    const std::string format = GENERATE(std::string("step"), std::string("amf"), std::string("3mf"));
    const bool remember = GENERATE(false, true);
    const bool dont_ask = GENERATE(false, true);
    CAPTURE(format, remember, dont_ask);
    AppConfig config;
    const std::string prefix = "import_names_" + format + "_";
    const std::string show = format == "step" ? "enable_step_mesh_setting" : prefix + "show_dialog";
    config.set_bool(prefix + "keep", false);
    config.set_bool(prefix + "sort", false);
    config.set_bool(prefix + "assembly", false);
    config.set_bool("is_split_compound", false);
    config.set_bool(show, true);
    GeometryImportOptions options;
    options.keep_names = true;
    options.sort_names = true;
    options.load_as_assembly = true;
    options.split_compound = true;
    options.chosen = true;
    options.remember = remember;
    options.dont_ask = dont_ask;
    save_geometry_import_options(config, format, options);
    // Suppression must persist the action even with Remember my choice unchecked.
    const bool persisted = remember || dont_ask;
    CHECK(config.get_bool(prefix + "keep") == persisted);
    CHECK(config.get_bool(prefix + "sort") == persisted);
    // A 3MF keeps its own structure, so it has no presentation choice to remember.
    CHECK(config.get_bool(prefix + "assembly") == (persisted && format != "3mf"));
    // Reading side: a 3MF keeps the structure written in it whatever the stored key says.
    CHECK(geometry_import_options(config, format).load_as_assembly == (format == "3mf" || persisted));
    CHECK(config.get_bool("is_split_compound") == (persisted && format == "step"));
    CHECK(config.get_bool(show) == !dont_ask);
}

TEST_CASE("Sorting disabled by Keep original names keeps its remembered value", "[GeometryImport]")
{
    const std::string format = GENERATE(std::string("step"), std::string("amf"), std::string("3mf"));
    CAPTURE(format);
    AppConfig config;
    const std::string prefix = "import_names_" + format + "_";
    config.set_bool(prefix + "sort", true);
    GeometryImportOptions options;
    options.keep_names = false;
    options.sort_names = false;
    options.chosen = true;
    options.remember = true;
    save_geometry_import_options(config, format, options);
    CHECK_FALSE(config.get_bool(prefix + "keep"));
    CHECK(config.get_bool(prefix + "sort"));
}

TEST_CASE("Name cleanup remembers the complete replacement string", "[GeometryImport]")
{
    const bool remember = GENERATE(false, true);
    const bool dont_ask = GENERATE(false, true);
    AppConfig config;
    config.set("import_names_cleanup_replacement", "_");
    config.set_bool("import_names_cleanup_remove", true);
    config.set_bool("import_names_cleanup_show_dialog", true);
    GeometryImportNamePolicy policy;
    policy.used = true;
    policy.remove = false;
    policy.replacement = "_slash_";
    policy.remember = remember;
    policy.dont_ask = dont_ask;
    save_geometry_import_name_policy(config, policy);
    const bool persisted = remember || dont_ask;
    CHECK(config.get("import_names_cleanup_replacement") == (persisted ? "_slash_" : "_"));
    CHECK(config.get_bool("import_names_cleanup_remove") == !persisted);
    CHECK(config.get_bool("import_names_cleanup_show_dialog") == !dont_ask);
}

TEST_CASE("Geometry import treats STEP and STP extensions as the same format", "[GeometryImport]")
{
    CHECK(geometry_import_format("model.STEP") == "step");
    CHECK(geometry_import_format("model.sTp") == "step");
    CHECK(geometry_import_format("model.AMF") == "amf");
    CHECK(geometry_import_format("model.3MF") == "3mf");
    CHECK(geometry_import_format("model.stl").empty());
}

TEST_CASE("AMF loads separate objects unless an assembly mode was chosen", "[GeometryImport]")
{
    AppConfig config;
    // Files from other slicers hold a whole plate in one group, so the default keeps them apart.
    CHECK(geometry_import_options(config, "amf").separate_objects);
    CHECK_FALSE(geometry_import_options(config, "amf").single_object());
    CHECK_FALSE(geometry_import_options(config, "step").separate_objects);
    CHECK_FALSE(geometry_import_options(config, "3mf").separate_objects);

    const bool separate = GENERATE(false, true);
    const bool assembly = GENERATE(false, true);
    CAPTURE(separate, assembly);
    config.set_bool("import_names_amf_assembly", !assembly);
    GeometryImportOptions options;
    options.separate_objects = separate;
    options.load_as_assembly = assembly;
    options.chosen = true;
    options.remember = true;
    save_geometry_import_options(config, "amf", options);
    const GeometryImportOptions restored = geometry_import_options(config, "amf");
    CHECK(restored.separate_objects == separate);
    CHECK(restored.load_as_assembly == assembly);
    CHECK(restored.single_object() == (!separate && !assembly));
    // The STEP dialog has no separate mode and keeps its own key.
    CHECK_FALSE(config.has("import_names_step_separate"));
}
