#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AIModelGenerationClient.hpp"

using Slic3r::GUI::AIModelGenerationClient;

TEST_CASE("Legacy Tripo choices restore the same standard face cap without editing saved history",
          "[ModelGenerationClient][Regression]")
{
    for (const int faces : {300000, 1000000, 2000000}) {
        for (const bool null_geometry : {false, true}) {
            DYNAMIC_SECTION("faces=" << faces << " null=" << null_geometry) {
                nlohmann::json saved = {{"face_limit", faces}, {"output_format", "obj"}};
                if (null_geometry) saved["geometry_quality"] = nullptr;
                const auto original = saved;
                for (int restore = 0; restore < 2; ++restore) {
                    const auto options = AIModelGenerationClient::restore_generation_options(saved);
                    CHECK(options.provider == "tripo");
                    CHECK(options.face_limit == (faces == 2000000 ? 1000000 : faces));
                    CHECK(options.geometry_quality == "standard");
                    CHECK(options.texture_quality == "standard");
                    CHECK(options.output_format == "obj");
                    CHECK(saved == original);
                }
            }
        }
    }
}

TEST_CASE("Explicit model choices survive restoration for review without changing cost or face target",
          "[ModelGenerationClient][Regression]")
{
    for (const std::string provider : {"tripo", "hunyuan"}) {
        for (const std::string geometry : {"standard", "detailed"}) {
            DYNAMIC_SECTION(provider << " " << geometry) {
                const nlohmann::json saved = {{"provider", provider}, {"face_limit", 2000000},
                    {"geometry_quality", geometry}, {"texture_quality", "extreme"}, {"output_format", "obj"}};
                const auto options = AIModelGenerationClient::restore_generation_options(saved);
                CHECK(options.provider == provider);
                CHECK(options.face_limit == 2000000);
                CHECK(options.geometry_quality == geometry);
                CHECK(options.texture_quality == "extreme");
                CHECK(options.output_format == "obj");
            }
        }
    }
}

TEST_CASE("Tripo legacy restoration does not change another provider's choices",
          "[ModelGenerationClient][Regression]")
{
    const auto options = AIModelGenerationClient::restore_generation_options(
        {{"provider", "hunyuan"}, {"face_limit", 2000000}, {"geometry_quality", nullptr}});
    CHECK(options.provider == "hunyuan");
    CHECK(options.face_limit == 2000000);
    CHECK(options.geometry_quality == "standard");
}
