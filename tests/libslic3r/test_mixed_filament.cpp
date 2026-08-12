#include <catch2/catch_all.hpp>

#include "libslic3r/MixedFilament.hpp"

using namespace Slic3r;

TEST_CASE("MixedFilament ratio 1:1 resolve", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1");
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.is_mixed(3, 2));
    REQUIRE_FALSE(mgr.is_mixed(1, 2));
    REQUIRE(mgr.total_filaments(2) == 3);
    // Layers alternate A, B
    REQUIRE(mgr.resolve(3, 2, 0) == 1);
    REQUIRE(mgr.resolve(3, 2, 1) == 2);
    REQUIRE(mgr.resolve(3, 2, 2) == 1);
    REQUIRE(mgr.resolve(3, 2, 3) == 2);
}

TEST_CASE("MixedFilament ratio 2:1 resolve", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,2,1");
    // cycle = 3: A,A,B
    REQUIRE(mgr.resolve(3, 2, 0) == 1);
    REQUIRE(mgr.resolve(3, 2, 1) == 1);
    REQUIRE(mgr.resolve(3, 2, 2) == 2);
    REQUIRE(mgr.resolve(3, 2, 3) == 1);
    REQUIRE(mgr.resolve(3, 2, 4) == 1);
    REQUIRE(mgr.resolve(3, 2, 5) == 2);
}

TEST_CASE("MixedFilament pattern 112 sequence", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    // A=1,B=2, pattern 112 → T0,T0,T1 (physical 1,1,2)
    mgr.load_definitions("1,2,1,1,1,112");
    REQUIRE(mgr.resolve(3, 2, 0) == 1);
    REQUIRE(mgr.resolve(3, 2, 1) == 1);
    REQUIRE(mgr.resolve(3, 2, 2) == 2);
    REQUIRE(mgr.resolve(3, 2, 3) == 1);
    REQUIRE(mgr.resolve(3, 2, 4) == 1);
    REQUIRE(mgr.resolve(3, 2, 5) == 2);
}

TEST_CASE("MixedFilament serialize load round-trip with pattern", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,2,1,112");
    const std::string ser = mgr.serialize_definitions();
    REQUIRE(ser.find("112") != std::string::npos);

    MixedFilamentManager round;
    round.load_definitions(ser);
    REQUIRE(round.enabled_count() == 1);
    REQUIRE(round.mixed_filaments().front().manual_pattern == "112");
    REQUIRE(round.mixed_filaments().front().ratio_a == 2);
    REQUIRE(round.mixed_filaments().front().ratio_b == 1);
    // Pattern wins over ratio
    REQUIRE(round.resolve(3, 2, 0) == 1);
    REQUIRE(round.resolve(3, 2, 1) == 1);
    REQUIRE(round.resolve(3, 2, 2) == 2);
}

TEST_CASE("MixedFilament total_filaments and is_mixed", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1;1,3,1,1,1");
    // Two enabled mixes on 4 physicals
    REQUIRE(mgr.enabled_count() == 2);
    REQUIRE(mgr.total_filaments(4) == 6);
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE(mgr.is_mixed(6, 4));
    REQUIRE_FALSE(mgr.is_mixed(4, 4));
    REQUIRE_FALSE(mgr.is_mixed(7, 4));
}

TEST_CASE("MixedFilament pattern separators normalize", "[MixedFilament]")
{
    REQUIRE(MixedFilamentManager::normalize_manual_pattern("1-1-2") == "112");
    REQUIRE(MixedFilamentManager::normalize_manual_pattern("1 1 2") == "112");
    // First group only for comma-grouped patterns
    REQUIRE(MixedFilamentManager::normalize_manual_pattern("12,21") == "12");
}

TEST_CASE("MixedFilament append_physical_0based", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1");
    std::vector<unsigned int> out;
    mgr.append_physical_0based(3, 2, out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == 0);
    REQUIRE(out[1] == 1);
}
