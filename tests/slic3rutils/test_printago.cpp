#include <catch2/catch_all.hpp>

#include "slic3r/Utils/Printago.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("Printago parse_callback_token", "[Printago]")
{
    SECTION("extracts the token") {
        auto t = Printago::parse_callback_token("orcaslicer://auth/callback?token=abc123");
        REQUIRE(t.has_value());
        REQUIRE(*t == "abc123");
    }

    SECTION("token among other params, any order") {
        auto t = Printago::parse_callback_token("orcaslicer://auth/callback?state=x&token=tok-9&foo=bar");
        REQUIRE(t.has_value());
        REQUIRE(*t == "tok-9");
    }

    SECTION("url-decodes the token") {
        auto t = Printago::parse_callback_token("orcaslicer://auth/callback?token=a%2Bb%2Fc");
        REQUIRE(t.has_value());
        REQUIRE(*t == "a+b/c");
    }

    SECTION("not a printago callback") {
        REQUIRE_FALSE(Printago::parse_callback_token("https://app.printago.io/orca?token=abc").has_value());
        REQUIRE_FALSE(Printago::parse_callback_token("orcaslicer://other?token=abc").has_value());
    }

    SECTION("callback without a token") {
        REQUIRE_FALSE(Printago::parse_callback_token("orcaslicer://auth/callback").has_value());
        REQUIRE_FALSE(Printago::parse_callback_token("orcaslicer://auth/callback?state=x").has_value());
        REQUIRE_FALSE(Printago::parse_callback_token("orcaslicer://auth/callback?token=").has_value());
    }
}

TEST_CASE("Printago endpoint URLs", "[Printago]")
{
    const auto ep = default_printago_endpoints();

    SECTION("login url is the orca entry route") {
        REQUIRE_THAT(ep.login_url(), ContainsSubstring("/orca"));
    }

    SECTION("launch url carries the token in the fragment") {
        const auto url = ep.launch_url("sess-token-42");
        REQUIRE_THAT(url, ContainsSubstring("/orca/launch"));
        REQUIRE_THAT(url, ContainsSubstring("#token=sess-token-42"));
        // token must be in the fragment, never the query string
        REQUIRE(url.find('?') == std::string::npos);
        REQUIRE(url.find("#token=") != std::string::npos);
    }
}
