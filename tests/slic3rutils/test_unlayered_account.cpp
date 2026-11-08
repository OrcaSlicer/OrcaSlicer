#include <catch2/catch_test_macros.hpp>

#include <string>

#include "slic3r/Utils/UnlayeredAccount.hpp"

using namespace Slic3r;

// Sign-in when Unlayered can't be reached: a handoff that gets no HTTP answer at all is marked unreachable, so the
// sign-in dialog says "Couldn't reach Unlayered" instead of showing libcurl's text. Nothing listens on port 1 of this
// machine, so the request fails here and never leaves it.
TEST_CASE("A sign-in that cannot connect is marked unreachable", "[UnlayeredAccount]")
{
    UnlayeredAccount          account("http://127.0.0.1:1");
    UnlayeredAccount::Handoff handoff;
    std::string               error;
    REQUIRE_FALSE(account.start_handoff(handoff, error));
    CHECK(handoff.unreachable);
    CHECK_FALSE(error.empty()); // the raw text is kept for the log
}
