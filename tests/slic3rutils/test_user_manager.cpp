#include <catch2/catch_all.hpp>

#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/UserManager.hpp"

using namespace Slic3r;

TEST_CASE("User message that is not JSON is rejected without throwing", "[UserManager]")
{
    const std::string payload = GENERATE(as<std::string>{}, "not json", "", "<html></html>", "{\"bind\":");
    UserManager manager;
    int result = 0;
    REQUIRE_NOTHROW(result = manager.parse_json(payload));
    CHECK(result == -1);
}

TEST_CASE("User message without a successful bind is ignored", "[UserManager]")
{
    const std::string payload = GENERATE(as<std::string>{}, "{}", R"({"bind":{"command":"unbind"}})", R"({"bind":"bind"})", "[1]");
    UserManager manager;
    CHECK(manager.parse_json(payload) == -1);
}
