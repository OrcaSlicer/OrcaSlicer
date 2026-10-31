#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ColorDecomposeRecipe.hpp"

#include <initializer_list>
#include <numeric>
#include <string>

using namespace Slic3r;

namespace {

int total_ratio(const ColorDecomposeRecipeResult& recipe)
{
    return std::accumulate(recipe.components.begin(), recipe.components.end(), 0,
                           [](int sum, const ColorDecomposeRecipeComponent& comp) { return sum + comp.ratio; });
}

} // namespace

TEST_CASE("CMYK+W decomposition uses the fixed channel slots", "[ColorDecompose]")
{
    SECTION("Black is a single black component")
    {
        const auto recipe = lookup_standard_recipe({0, 0, 0}, ColorDecomposeRecipeMode::CMYKW, std::string());
        REQUIRE(recipe.valid);
        REQUIRE(recipe.components.size() == 1);
        CHECK(recipe.components.front().base_color == "Black");
        CHECK(recipe.components.front().filament_index == 4);
        CHECK(recipe.components.front().ratio == 100);
    }

    SECTION("White is a single white component")
    {
        const auto recipe = lookup_standard_recipe({255, 255, 255}, ColorDecomposeRecipeMode::CMYKW, std::string());
        REQUIRE(recipe.valid);
        REQUIRE(recipe.components.size() == 1);
        CHECK(recipe.components.front().base_color == "White");
        CHECK(recipe.components.front().filament_index == 5);
    }

    SECTION("Red is an even mix of magenta and yellow")
    {
        const auto recipe = lookup_standard_recipe({255, 0, 0}, ColorDecomposeRecipeMode::CMYKW, std::string());
        REQUIRE(recipe.valid);
        REQUIRE(recipe.components.size() == 2);
        CHECK(recipe.components[0].base_color == "Magenta");
        CHECK(recipe.components[0].filament_index == 2);
        CHECK(recipe.components[0].ratio == 50);
        CHECK(recipe.components[1].base_color == "Yellow");
        CHECK(recipe.components[1].filament_index == 3);
        CHECK(recipe.components[1].ratio == 50);
    }

    SECTION("Grey mixes black and white")
    {
        const auto recipe = lookup_standard_recipe({128, 128, 128}, ColorDecomposeRecipeMode::CMYKW, std::string());
        REQUIRE(recipe.valid);
        REQUIRE(recipe.components.size() == 2);
        CHECK(recipe.components[0].base_color == "Black");
        CHECK(recipe.components[1].base_color == "White");
    }

    SECTION("The shares always add up to 100")
    {
        for (unsigned char r : {0, 40, 128, 200, 255})
            for (unsigned char g : {0, 90, 128, 255})
                for (unsigned char b : {0, 17, 128, 230}) {
                    const auto recipe = lookup_standard_recipe({r, g, b}, ColorDecomposeRecipeMode::CMYKW, std::string());
                    REQUIRE(recipe.valid);
                    CHECK(total_ratio(recipe) == 100);
                }
    }
}
