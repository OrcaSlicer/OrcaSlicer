#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"

#include <algorithm>
#include <set>

using Slic3r::GUI::AIModelGenerationClient;
using namespace Slic3r::GUI::ModelGenerationPresentation;

TEST_CASE("model-generation progress maps service phases to stable UI milestones",
          "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::JobStatus status;
    REQUIRE(status.palette_color_count == Slic3r::AI::kLegacyDefaultTargetPaletteColors);

    status.state = "recommending_palette";
    status.progress = 5;
    REQUIRE(display_progress(status) == 3);
    status.progress = 10;
    REQUIRE(display_progress(status) == 10);

    status.state = "awaiting_palette_confirmation";
    REQUIRE(display_progress(status) == 10);
    status.state = "preprocessing";
    status.progress = 15;
    REQUIRE(display_progress(status) == 25);
    status.state = "awaiting_confirmation";
    REQUIRE(display_progress(status) == 35);

    status.state.clear();
    status.phase = "generating";
    status.progress = 70;
    REQUIRE(display_progress(status) == 78);
    status.phase = "converting";
    status.progress = 95;
    REQUIRE(display_progress(status) == 90);
    status.phase = "downloading_artifact";
    REQUIRE(display_progress(status) == 92);
    status.phase = "checking_model";
    status.progress = 99;
    REQUIRE(display_progress(status) == 97);
    status.phase = "checking_visual";
    REQUIRE(display_progress(status) == 98);

    status.phase.clear();
    status.state = "ready";
    REQUIRE(display_progress(status) == 100);
}

TEST_CASE("Server palette corrections preserve previews without overwriting user edits", "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette = {"#F4F4F0", "#1F1B1C", "#F2C9AE"};
    const AIModelGenerationClient::PaletteRoles submitted = {
        {"primary", palette[2]}, {"structure", palette[1]}, {"light", palette[0]}};
    const AIModelGenerationClient::PaletteRoles corrected = {
        {"primary", palette[0]}, {"structure", palette[1]}, {"light", palette[2]}};
    auto current = submitted;
    synchronize_palette_roles(palette, current, palette, submitted, palette, corrected);
    CHECK(current == corrected);
    synchronize_palette_roles(palette, current, palette, corrected, palette, corrected);
    CHECK(current == corrected);

    auto edited_palette = palette;
    edited_palette[0] = "#FFFFFF";
    current = submitted;
    synchronize_palette_roles(edited_palette, current, palette, submitted, palette, corrected);
    CHECK(current == submitted);
    synchronize_palette_roles(palette, current, palette, submitted, edited_palette, corrected);
    CHECK(current == submitted);

    auto edited_roles = submitted;
    std::swap(edited_roles["structure"], edited_roles["primary"]);
    current = edited_roles;
    synchronize_palette_roles(palette, current, palette, submitted, palette, corrected);
    CHECK(current == edited_roles);
    synchronize_palette_roles(palette, current, palette, edited_roles, palette, {});
    CHECK(current == edited_roles);
}

TEST_CASE("automatic printable palette roles remain deterministic and distinct",
          "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette {
        "#000000", "#FFFFFF", "#FF0000", "#00FF00", "#0066FF", "#9933CC"
    };
    for (size_t count = 1; count <= palette.size(); ++count) {
        DYNAMIC_SECTION(count << " colors receive a complete stable role prefix") {
            const std::vector<std::string> active(palette.begin(), palette.begin() + count);
            const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(active);
            REQUIRE(roles.size() == count);
            for (size_t index = 0; index < PALETTE_ROLE_IDS.size(); ++index)
                CHECK(roles.count(PALETTE_ROLE_IDS[index]) == (index < count ? 1 : 0));
            std::set<std::string> assigned;
            for (const auto& [role, color] : roles) {
                CHECK(std::find(active.begin(), active.end(), color) != active.end());
                assigned.insert(color);
            }
            CHECK(assigned.size() == count);
            CHECK(automatic_palette_roles(active) == roles);
        }
    }

    const std::vector<std::string> legacy_palette(palette.begin(), palette.begin() + 4);
    const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(legacy_palette);
    REQUIRE(roles.at("structure") == "#000000");
    REQUIRE(roles.at("light") == "#FFFFFF");
    REQUIRE(roles.at("primary") == "#00FF00");
    REQUIRE(roles.at("accent") == "#FF0000");
    REQUIRE(same_palette_color("#aBc123", "#AbC123"));
    REQUIRE_FALSE(same_palette_color("#ABC123", "#ABC124"));
    CHECK(automatic_palette_roles({"#000000", "#FFFFFF", "#FF0000", "#00FF00",
                                   "#0066FF", "#9933CC", "#00FFFF"}).empty());
    CHECK(automatic_palette_roles({"#000000", "invalid"}).empty());
    CHECK(automatic_palette_roles({"#000000", "#000000"}).empty());
}

TEST_CASE("style families retain legacy styles in a compact secondary choice",
          "[ModelGenerationPresentation]")
{
    CHECK(style_selection("portrait_sketch") == 2);
    CHECK(style_selection("ink_relief") == 2);
    CHECK(style_selection("sculpture") == 0);
    CHECK(style_selection("realistic") == 1);
    for (const std::string style : {"portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom"}) {
        CHECK(style_selection(style) == 2);
        CHECK(selected_style(2, stylized_style_selection(style)) == style);
    }
    CHECK(selected_style(0, 5) == "sculpture");
    CHECK(selected_style(1, 5) == "realistic");
    CHECK(selected_style(2, -1) == "cartoon");
    CHECK(style_uses_printable_colors("portrait_sketch"));
    CHECK(style_uses_printable_colors("ink_relief"));
}
