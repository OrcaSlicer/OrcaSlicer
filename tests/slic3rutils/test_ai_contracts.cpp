#include <catch2/catch_all.hpp>

#include "slic3r/AI/Contracts/ColorIntent.hpp"
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/ModelGeneration/GeneratedModelArtifact.hpp"
#include "slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp"

#include <type_traits>
#include <limits>
#include <utility>

using namespace Slic3r::AI;

namespace {

template<class T, class = void> struct HasAutoSliceAfterImport : std::false_type {};
template<class T>
struct HasAutoSliceAfterImport<T, std::void_t<decltype(std::declval<T>().auto_slice_after_import)>> : std::true_type {};

template<class T, class = void> struct HasSliceAfterImport : std::false_type {};
template<class T>
struct HasSliceAfterImport<T, std::void_t<decltype(std::declval<T>().slice_after_import)>> : std::true_type {};

class RecordingConsumer final : public IModelArtifactConsumer
{
public:
    ModelImportResult import_artifact(const ModelImportRequest& request) override
    {
        ModelImportResult result;
        if (request.artifact.job_id.empty())
            result.outcome = ModelImportOutcome::InvalidArtifact;
        else
            result.outcome = ModelImportOutcome::Imported;
        result.color_mode = request.color_mode;
        return result;
    }
};

class FixedPaletteProvider final : public IPrintablePaletteProvider
{
public:
    PrintablePaletteSnapshot printable_palette() const override
    {
        return {{"#112233"}, {0}, {0}, {"#112233"}};
    }
};

} // namespace

TEST_CASE("neutral AI contracts preserve accepted defaults and legacy includes", "[AIContracts]")
{
    static_assert(std::is_abstract_v<IModelArtifactConsumer>);
    static_assert(std::is_abstract_v<IPrintablePaletteProvider>);
    static_assert(!HasAutoSliceAfterImport<ModelImportRequest>::value);
    static_assert(!HasSliceAfterImport<ModelImportResult>::value);

    ModelImportRequest request;
    CHECK(request.color_mode == ImportColorMode::NativeMatch);
    CHECK_FALSE(request.color_trial.has_value());
    CHECK_FALSE(request.artifact.used_printable_colors);

    request.artifact.job_id = "accepted-job";
    request.color_mode      = ImportColorMode::AutoMap;
    RecordingConsumer consumer;
    const ModelImportResult result = consumer.import_artifact(request);
    CHECK(result.imported());
    CHECK(result.color_mode == ImportColorMode::AutoMap);

    const PrintablePaletteSnapshot palette = FixedPaletteProvider().printable_palette();
    REQUIRE(palette.compatible_colors.size() == 1);
    CHECK(palette.compatible_colors.front() == "#112233");
}

TEST_CASE("an import color trial accepts one through thirty two paired normalized colors", "[AIContracts][ModelColorTrial]")
{
    for (size_t count = 1; count <= kMaxModelColorTrialColors; ++count) {
        DYNAMIC_SECTION(count << " paired colors") {
            ModelColorTrial trial;
            trial.mapping_colors.assign(count, {0.f, 0.5f, 1.f});
            trial.target_colors.assign(count, {1.f, 0.f, 0.25f});
            CHECK(trial.valid());
        }
    }
}

TEST_CASE("an import color trial rejects empty mismatched and oversized palettes", "[AIContracts][ModelColorTrial]")
{
    for (const auto& sizes : {std::pair<size_t, size_t>{0, 0}, {0, 1}, {1, 0}, {1, 2}, {2, 1},
                              {kMaxModelColorTrialColors + 1, kMaxModelColorTrialColors + 1}}) {
        DYNAMIC_SECTION(sizes.first << " mapping colors and " << sizes.second << " target colors") {
            ModelColorTrial trial;
            trial.mapping_colors.assign(sizes.first, {0.f, 0.5f, 1.f});
            trial.target_colors.assign(sizes.second, {1.f, 0.f, 0.25f});
            CHECK_FALSE(trial.valid());
        }
    }
}

TEST_CASE("an import color trial rejects nonfinite and out of range channels in either palette", "[AIContracts][ModelColorTrial]")
{
    const std::array<float, 5> invalid_channels {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -0.001f, 1.001f,
    };
    for (size_t palette = 0; palette < 2; ++palette)
        for (size_t channel = 0; channel < 3; ++channel)
            for (size_t invalid = 0; invalid < invalid_channels.size(); ++invalid) {
                DYNAMIC_SECTION("palette " << palette << " channel " << channel << " invalid value " << invalid) {
                    ModelColorTrial trial {{{0.f, 0.5f, 1.f}}, {{1.f, 0.f, 0.25f}}};
                    auto& colors = palette == 0 ? trial.mapping_colors : trial.target_colors;
                    colors.front()[channel] = invalid_channels[invalid];
                    CHECK_FALSE(trial.valid());
                }
            }
}

TEST_CASE("a per import color trial preserves the generated artifact and its color intent", "[AIContracts][ModelColorTrial]")
{
    GeneratedModelArtifact artifact;
    artifact.local_path = "original-model.obj";
    artifact.job_id = "original-job";
    artifact.format = "obj";
    artifact.color_encoding = "vertex-rgb";
    artifact.generation_palette = {"#112233", "#445566"};
    artifact.used_printable_colors = true;
    artifact.color_intent_manifest = ColorIntentManifestRef {
        "color-intent.v1.json", kColorIntentSchemaV1,
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    };
    ModelImportRequest request {artifact};
    CHECK_FALSE(request.color_trial.has_value());
    request.color_trial = ModelColorTrial {{{0.f, 0.5f, 1.f}}, {{1.f, 0.f, 0.25f}}};
    REQUIRE(request.color_trial->valid());
    request.color_trial->target_colors.front() = {0.f, 1.f, 0.f};
    CHECK(request.artifact.local_path == artifact.local_path);
    CHECK(request.artifact.job_id == artifact.job_id);
    CHECK(request.artifact.format == artifact.format);
    CHECK(request.artifact.color_encoding == artifact.color_encoding);
    CHECK(request.artifact.generation_palette == artifact.generation_palette);
    CHECK(request.artifact.used_printable_colors == artifact.used_printable_colors);
    REQUIRE(request.artifact.color_intent_manifest.has_value());
    CHECK(request.artifact.color_intent_manifest->local_path == artifact.color_intent_manifest->local_path);
    CHECK(request.artifact.color_intent_manifest->schema == artifact.color_intent_manifest->schema);
    CHECK(request.artifact.color_intent_manifest->sha256 == artifact.color_intent_manifest->sha256);
    request.color_trial.reset();
    CHECK(request.artifact.generation_palette == artifact.generation_palette);
    CHECK(request.color_mode == ImportColorMode::NativeMatch);
}

TEST_CASE("physical color capability accepts one through six unique channels", "[AIContracts][ColorIntent]")
{
    for (size_t count = 0; count <= 7; ++count)
        CHECK(is_supported_physical_channel_count(count) == (count >= 1 && count <= 6));

    std::vector<PhysicalFilamentChannel> channels;
    for (size_t slot = 0; slot < 6; ++slot)
        channels.push_back({slot, "#112233", "PLA", true});
    CHECK(is_valid_physical_channel_set(channels));

    channels.push_back({6, "#445566", "PLA", true});
    CHECK_FALSE(is_valid_physical_channel_set(channels));
    channels.pop_back();
    channels.back().slot = channels.front().slot;
    CHECK_FALSE(is_valid_physical_channel_set(channels));
    channels.back().slot = 5;
    channels.back().display_color = "not-a-color";
    CHECK_FALSE(is_valid_physical_channel_set(channels));
}

TEST_CASE("printable palette roles use a stable one-to-six prefix", "[AIContracts][ColorIntent]")
{
    CHECK(kMinTargetPaletteColors == 1);
    CHECK(kMaxTargetPaletteColors == 6);
    CHECK(kLegacyDefaultTargetPaletteColors == 4);
    REQUIRE(kPaletteRoleIds.size() == kMaxTargetPaletteColors);
    CHECK(std::string(kPaletteRoleIds[0]) == "primary");
    CHECK(std::string(kPaletteRoleIds[3]) == "accent");
    CHECK(std::string(kPaletteRoleIds[4]) == "secondary");
    CHECK(std::string(kPaletteRoleIds[5]) == "detail");

    for (size_t count = kMinTargetPaletteColors; count <= kMaxTargetPaletteColors; ++count) {
        DYNAMIC_SECTION(count << " printable colors activate the same number of roles") {
            for (size_t index = 0; index < kPaletteRoleIds.size(); ++index)
                CHECK(is_active_palette_role(kPaletteRoleIds[index], count) == (index < count));
        }
    }
    CHECK_FALSE(is_supported_target_palette_color_count(0));
    CHECK_FALSE(is_supported_target_palette_color_count(7));
}

TEST_CASE("process mix recipes require one to three normalized unique components", "[AIContracts][ColorIntent]")
{
    MixedColorRecipe recipe {
        "#778899",
        {{0, 0.25}, {2, 0.75}},
        std::nullopt,
    };
    CHECK(is_valid_mixed_color_recipe(recipe));

    recipe.components.clear();
    CHECK_FALSE(is_valid_mixed_color_recipe(recipe));
    recipe.components = {{0, 0.25}, {1, 0.25}, {2, 0.25}, {3, 0.25}};
    CHECK_FALSE(is_valid_mixed_color_recipe(recipe));
    recipe.components = {{0, 0.5}, {0, 0.5}};
    CHECK_FALSE(is_valid_mixed_color_recipe(recipe));
    recipe.components = {{0, 0.0}, {1, 1.0}};
    CHECK_FALSE(is_valid_mixed_color_recipe(recipe));
    recipe.components = {{0, 0.4}, {1, 0.4}};
    CHECK_FALSE(is_valid_mixed_color_recipe(recipe));
    recipe.components = {{0, 0.5}, {1, 0.5}};
    recipe.target_color = "#XYZXYZ";
    CHECK_FALSE(is_valid_mixed_color_recipe(recipe));
}

TEST_CASE("typed printable palette rebuilds the legacy flat projection", "[AIContracts][ColorIntent]")
{
    PrintablePaletteSnapshot palette;
    palette.physical_channels = {
        {0, "#112233", "PLA", true},
        {2, "#445566", "PLA", false},
        {5, "#778899", "PLA", true},
    };
    palette.supported_output_modes = {ColorOutputMode::DiscreteFilament, ColorOutputMode::ProcessMix};
    palette.mixed_recipes.push_back({"#AABBCC", {{0, 0.5}, {5, 0.5}}, std::nullopt});
    CHECK(palette.rebuild_legacy_projection());

    REQUIRE(palette.project_colors.size() == 6);
    CHECK(palette.project_colors[0] == "#112233");
    CHECK(palette.project_colors[1].empty());
    CHECK(palette.project_colors[5] == "#778899");
    CHECK(palette.valid_slots == std::vector<size_t> {0, 2, 5});
    CHECK(palette.compatible_slots == std::vector<size_t> {0, 5});
    CHECK(palette.compatible_colors == std::vector<std::string> {"#112233", "#778899"});
    CHECK(palette.supports(ColorOutputMode::DiscreteFilament));
    CHECK(palette.supports(ColorOutputMode::ProcessMix));

    GeneratedModelArtifact artifact;
    CHECK_FALSE(artifact.color_intent_manifest.has_value());
    artifact.color_intent_manifest = ColorIntentManifestRef {
        "color-intent.v1.json",
        "orcaslicer.color-intent.v1",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    };
    CHECK(artifact.color_intent_manifest->schema == "orcaslicer.color-intent.v1");
    CHECK(is_valid_color_intent_manifest_ref(*artifact.color_intent_manifest));
    artifact.color_intent_manifest->schema = "orcaslicer.color-intent.v2";
    CHECK_FALSE(is_valid_color_intent_manifest_ref(*artifact.color_intent_manifest));
    artifact.color_intent_manifest->schema = kColorIntentSchemaV1;
    artifact.color_intent_manifest->sha256[0] = 'A';
    CHECK_FALSE(is_valid_color_intent_manifest_ref(*artifact.color_intent_manifest));
}
