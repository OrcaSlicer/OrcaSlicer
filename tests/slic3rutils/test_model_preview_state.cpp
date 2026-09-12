#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include "../test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem/fstream.hpp>

using Slic3r::GUI::ModelPreview3D;
namespace selection = Slic3r::AI::SurfaceSelectionPersistence;
namespace trial = Slic3r::AI::ColorTrialPersistence;

namespace {
struct SavedPreview {
    ScopedTemporaryDir directory {"orca-preview-state"};
    boost::filesystem::path model = directory.path() / "models" / "saved.glb";
    boost::filesystem::path record = directory.path() / "downloads" / "saved.json";
    boost::filesystem::path adjacent = directory.path() / "models" / "saved.json";
    ModelPreview3D::PreparedModel original;
    selection::SelectionState masks;
    selection::FaceColorOverrides intent {{0, {0.2f, 0.7f, 0.1f}}};
    trial::State colors;

    SavedPreview()
    {
        boost::filesystem::create_directories(model.parent_path());
        boost::filesystem::create_directories(record.parent_path());
        boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR)) /
            "model_artifact" / "baseline.glb", model);
        std::string error;
        REQUIRE(ModelPreview3D::prepare_model(model, original, error));
        REQUIRE(original.triangles >= 2);
        masks.selected.assign(original.triangles, 0);
        masks.protected_faces.assign(original.triangles, 0);
        masks.foreground.assign(original.triangles, 0);
        masks.domain.assign(original.triangles, 0);
        masks.selected[0] = masks.foreground[0] = masks.domain[0] = 1;
        masks.protected_faces[1] = 1;
        colors.mapping_colors = {{0.1f, 0.2f, 0.3f}};
        colors.colors = {{0.8f, 0.6f, 0.4f}};
        colors.source = 2;
        colors.count = 1;
        colors.enabled = true;
        colors.locks[0] = true;
    }

    nlohmann::json metadata(const std::string& geometry) const
    {
        return {
            {"local_selection", selection::encode(masks, original.triangles, geometry)},
            {"face_color_intent", selection::encode_colors(intent, original.triangles, geometry)},
            {"color_trial", trial::encode(colors, original.triangles, geometry)}
        };
    }

    void write(const boost::filesystem::path& path, const nlohmann::json& value) const
    {
        boost::filesystem::ofstream output(path);
        REQUIRE(output.good());
        output << value.dump();
        output.close();
        REQUIRE(output.good());
    }

    void check_restored(const ModelPreview3D::PreparedModel& prepared) const
    {
        REQUIRE(prepared.selection.has_value());
        CHECK(prepared.selection->selected == masks.selected);
        CHECK(prepared.selection->protected_faces == masks.protected_faces);
        CHECK(prepared.selection->foreground == masks.foreground);
        CHECK(prepared.selection->domain == masks.domain);
        CHECK(prepared.face_color_overrides == intent);
        REQUIRE(prepared.color_trial.has_value());
        CHECK(prepared.color_trial->mapping_colors == colors.mapping_colors);
        CHECK(prepared.color_trial->colors == colors.colors);
        CHECK(prepared.color_trial->locks == colors.locks);
        CHECK(prepared.color_trial->enabled == colors.enabled);
        CHECK(prepared.color_trial->source == 2);
    }
};
}

TEST_CASE("History preview restores editing state stored outside the model directory", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    REQUIRE_FALSE(boost::filesystem::exists(saved.adjacent));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    saved.check_restored(prepared);
}

TEST_CASE("The chosen history record takes precedence over adjacent preview state", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    saved.write(saved.adjacent, nlohmann::json::object());
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    saved.check_restored(prepared);
}

TEST_CASE("Saved preview state is rejected when geometry differs despite matching face counts", "[ModelPreviewState]")
{
    SavedPreview saved;
    const std::string other_geometry(64, '0');
    REQUIRE(other_geometry != saved.original.geometry_id);
    saved.write(saved.record, saved.metadata(other_geometry));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
    CHECK(prepared.triangles == saved.original.triangles);
}

TEST_CASE("Preview state beside a model remains available without an explicit history record", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error));
    saved.check_restored(prepared);
}

TEST_CASE("A missing chosen history record does not borrow another adjacent snapshot", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    REQUIRE_FALSE(boost::filesystem::exists(saved.record));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
}
