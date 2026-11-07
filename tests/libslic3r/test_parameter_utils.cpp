#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "libslic3r/ParameterUtils.hpp"

using namespace Slic3r;

TEST_CASE("A print sequence splits into layer ranges with their extruder orders", "[ParameterUtils]")
{
    // Two groups of equal length: layers 1-5 print extruders 2 then 1, layers 6-9 print 1 then 2.
    const std::vector<int> sequence{1, 5, 2, 1, 6, 9, 1, 2};
    const auto             seqs = get_other_layers_print_sequence(2, sequence);
    REQUIRE(seqs.size() == 2);
    CHECK(seqs[0].first.first == 1);
    CHECK(seqs[0].first.second == 5);
    CHECK(seqs[0].second == std::vector<int>{2, 1});
    CHECK(seqs[1].first.first == 6);
    CHECK(seqs[1].first.second == 9);
    CHECK(seqs[1].second == std::vector<int>{1, 2});
}

TEST_CASE("A print sequence round-trips through its flat form", "[ParameterUtils]")
{
    std::vector<LayerPrintSequence> seqs{{{1, 3}, {2, 1}}, {{4, 8}, {1, 2}}};
    int              nums = 0;
    std::vector<int> flat;
    get_other_layers_print_sequence(seqs, nums, flat);
    CHECK(nums == 2);
    CHECK(flat == std::vector<int>{1, 3, 2, 1, 4, 8, 1, 2});
    CHECK(get_other_layers_print_sequence(nums, flat) == seqs);
}

TEST_CASE("A sequence count without matching values gives no custom sequence", "[ParameterUtils]")
{
    // The stored default: one placeholder value and a count of zero.
    const std::vector<int> placeholder{0};
    CHECK(get_other_layers_print_sequence(0, placeholder).empty());
    // The count raised on its own, as a command line override does.
    CHECK(get_other_layers_print_sequence(1, placeholder).empty());
    // A group with a layer range but no extruder, alone and as two groups that divide evenly.
    CHECK(get_other_layers_print_sequence(1, std::vector<int>{1, 5}).empty());
    CHECK(get_other_layers_print_sequence(2, std::vector<int>{1, 5, 6, 9}).empty());
    // A count that does not divide the values.
    CHECK(get_other_layers_print_sequence(2, std::vector<int>{1, 5, 2, 1, 6}).empty());
    // A negative count, and no values at all.
    CHECK(get_other_layers_print_sequence(-1, std::vector<int>{1, 5, 2}).empty());
    CHECK(get_other_layers_print_sequence(1, {}).empty());
}
