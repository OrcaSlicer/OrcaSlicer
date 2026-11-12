#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

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

TEST_CASE("A process gets columns for tools added to the printer, copied from tool 1", "[ParameterUtils]")
{
    const std::string standard = "Direct Drive Standard", high_flow = "Direct Drive High Flow";
    // Two tools with a Standard and a High Flow column each, as a toolchanger profile ships them.
    DynamicPrintConfig process;
    process.option<ConfigOptionInts>("print_extruder_id", true)->values           = {1, 1, 2, 2};
    process.option<ConfigOptionStrings>("print_extruder_variant", true)->values   = {standard, high_flow, standard, high_flow};
    process.option<ConfigOptionFloats>("outer_wall_speed", true)->values          = {100., 200., 110., 210.};
    process.option<ConfigOptionFloats>("inner_wall_speed", true)->values          = {50.};
    // The printer's extruder count raised to three.
    DynamicPrintConfig printer;
    printer.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {standard + "," + high_flow, standard + "," + high_flow,
                                                                                  standard + "," + high_flow};

    REQUIRE(add_process_columns_for_new_tools(process, printer));
    CHECK(process.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1, 1, 2, 2, 3, 3});
    CHECK(process.option<ConfigOptionStrings>("print_extruder_variant")->values ==
          std::vector<std::string>{standard, high_flow, standard, high_flow, standard, high_flow});
    // Each new column copies tool 1's column of its variant.
    CHECK(process.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{100., 200., 110., 210., 100., 200.});
    // A single value already serves every column.
    CHECK(process.option<ConfigOptionFloats>("inner_wall_speed")->values == std::vector<double>{50.});
    // Every tool has its columns now.
    CHECK_FALSE(add_process_columns_for_new_tools(process, printer));
    CHECK(process.option<ConfigOptionInts>("print_extruder_id")->values.size() == 6);
}

TEST_CASE("A process with one column keeps it shared by every tool", "[ParameterUtils]")
{
    DynamicPrintConfig process;
    process.option<ConfigOptionInts>("print_extruder_id", true)->values         = {1};
    process.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {"Direct Drive Standard"};
    process.option<ConfigOptionFloats>("outer_wall_speed", true)->values        = {30.};
    DynamicPrintConfig printer;
    printer.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {"Direct Drive Standard", "Direct Drive Standard"};

    CHECK_FALSE(add_process_columns_for_new_tools(process, printer));
    CHECK(process.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{1});
    CHECK(process.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{30.});
}
