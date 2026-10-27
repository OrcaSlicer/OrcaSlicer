#include <catch2/catch_all.hpp>

#include "slic3r/Utils/PrintMapping.hpp"

using Slic3r::PrintMapping;

namespace {

struct ParseOutcome
{
    PrintMapping mapping;
    std::string  error;
    bool         ok = false;
};

ParseOutcome parse(const Slic3r::PrintParams& params)
{
    ParseOutcome outcome;
    outcome.ok = PrintMapping::from_legacy(params, outcome.mapping, outcome.error);
    return outcome;
}

} // namespace

TEST_CASE("ams_mapping2 is authoritative when both AMS mappings are present", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.ams_mapping  = "[9,-1]";
    params.ams_mapping2 = R"([{"ams_id":0,"slot_id":1},{"ams_id":254,"slot_id":254}])";

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    REQUIRE(outcome.mapping.ams_mapping_version == PrintMapping::MappingVersion::v1);
    REQUIRE(outcome.mapping.filaments.size() == 2);

    const PrintMapping::Filament& first = outcome.mapping.filaments[0];
    CHECK(first.destination.kind == PrintMapping::DestinationKind::ams_slot);
    CHECK(first.destination.ams_id == 0);
    CHECK(first.destination.slot_id == 1);

    const PrintMapping::Filament& second = outcome.mapping.filaments[1];
    CHECK(second.destination.kind == PrintMapping::DestinationKind::external_spool);
    CHECK(second.destination.ams_id == 254);
}

TEST_CASE("ams_mapping2 is not bypassed by a malformed or empty ams_mapping", "[PrintMapping]")
{
    SECTION("a malformed v0 array does not matter when v1 is valid") {
        Slic3r::PrintParams params{};
        params.ams_mapping  = "not json";
        params.ams_mapping2 = R"([{"ams_id":2,"slot_id":3}])";

        const ParseOutcome outcome = parse(params);

        REQUIRE(outcome.ok);
        REQUIRE(outcome.mapping.ams_mapping_version == PrintMapping::MappingVersion::v1);
        REQUIRE(outcome.mapping.filaments.size() == 1);
        CHECK(outcome.mapping.filaments[0].destination.ams_id == 2);
    }

    SECTION("a malformed v1 array is an error rather than a v0 fallback") {
        Slic3r::PrintParams params{};
        params.ams_mapping  = "[0]";
        params.ams_mapping2 = R"([{"ams_id":0}])";

        const ParseOutcome outcome = parse(params);

        REQUIRE_FALSE(outcome.ok);
        REQUIRE_FALSE(outcome.error.empty());
        CHECK(outcome.mapping.filaments.empty());
    }

    SECTION("an empty v1 array is authoritative") {
        Slic3r::PrintParams params{};
        params.ams_mapping  = "[0,1]";
        params.ams_mapping2 = "[]";

        const ParseOutcome outcome = parse(params);

        REQUIRE(outcome.ok);
        REQUIRE(outcome.mapping.ams_mapping_version == PrintMapping::MappingVersion::v1);
        CHECK(outcome.mapping.filaments.empty());
    }
}

TEST_CASE("ams_mapping v0 falls back with negative entries left unmapped", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.ams_mapping = "[3,-1,255,254]";

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    REQUIRE(outcome.mapping.ams_mapping_version == PrintMapping::MappingVersion::v0);
    REQUIRE(outcome.mapping.filaments.size() == 4);

    CHECK(outcome.mapping.filaments[0].destination.kind == PrintMapping::DestinationKind::ams_slot);
    CHECK(outcome.mapping.filaments[0].destination.ams_id == PrintMapping::unmapped_id);
    CHECK(outcome.mapping.filaments[0].destination.slot_id == 3);

    CHECK(outcome.mapping.filaments[1].destination.kind == PrintMapping::DestinationKind::unmapped);
    CHECK(outcome.mapping.filaments[2].destination.kind == PrintMapping::DestinationKind::external_spool);
    CHECK(outcome.mapping.filaments[2].destination.ams_id == PrintMapping::virtual_main_ams_id);
    CHECK(outcome.mapping.filaments[3].destination.ams_id == PrintMapping::virtual_deputy_ams_id);
}

TEST_CASE("ams_mapping_info resolves the v1 external/unmapped sentinel", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.ams_mapping2     = R"([{"ams_id":255,"slot_id":255},{"ams_id":255,"slot_id":255}])";
    params.ams_mapping_info = R"([{"ams":-1},{"ams":255}])";

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    REQUIRE(outcome.mapping.filaments.size() == 2);
    CHECK(outcome.mapping.filaments[0].destination.kind == PrintMapping::DestinationKind::unmapped);
    CHECK(outcome.mapping.filaments[1].destination.kind == PrintMapping::DestinationKind::external_spool);
    CHECK(outcome.mapping.filaments[1].destination.ams_id == PrintMapping::virtual_main_ams_id);
}

TEST_CASE("ams_mapping_info restores an external spool that v0 encoded as unmapped", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.ams_mapping      = "[-1]";
    params.ams_mapping_info = R"([{"ams":254}])";

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    REQUIRE(outcome.mapping.filaments.size() == 1);
    CHECK(outcome.mapping.filaments[0].destination.kind == PrintMapping::DestinationKind::external_spool);
    CHECK(outcome.mapping.filaments[0].destination.ams_id == PrintMapping::virtual_deputy_ams_id);
}

TEST_CASE("ams_mapping_info metadata is attached to the index-matched filament", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.ams_mapping2 = R"([{"ams_id":0,"slot_id":2}])";
    params.ams_mapping_info =
        R"([{"ams":0,"targetColor":"FF0000FF","filamentId":"GFA00","filamentType":"PLA","nozzleId":1,"sourceColor":"00FF00FF"}])";

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    REQUIRE(outcome.mapping.filaments.size() == 1);
    const PrintMapping::Filament& filament = outcome.mapping.filaments[0];

    REQUIRE(filament.metadata.has_value());
    const PrintMapping::FilamentMetadata& metadata = *filament.metadata;
    CHECK(metadata.ams == 0);
    CHECK(metadata.target_color == "FF0000FF");
    CHECK(metadata.filament_id == "GFA00");
    CHECK(metadata.filament_type == "PLA");
    REQUIRE(metadata.nozzle_id.has_value());
    CHECK(*metadata.nozzle_id == 1);
    CHECK(metadata.source_color == "00FF00FF");
}

TEST_CASE("nozzle_mapping and nozzles_info are parsed as typed data", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.nozzle_mapping = "[1,0]";
    params.nozzles_info =
        R"([{"id":1,"type":null,"flowSize":"high","diameter":0.6},{"id":0,"type":"standard","flowSize":null,"diameter":0.4}])";

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    REQUIRE(outcome.mapping.nozzle_mapping.size() == 2);
    CHECK(outcome.mapping.nozzle_mapping[0].index == 0);
    CHECK(outcome.mapping.nozzle_mapping[0].nozzle_id == 1);
    CHECK(outcome.mapping.nozzle_mapping[1].nozzle_id == 0);

    REQUIRE(outcome.mapping.nozzles.size() == 2);
    CHECK(outcome.mapping.nozzles[0].id == 1);
    CHECK_FALSE(outcome.mapping.nozzles[0].type.has_value());
    REQUIRE(outcome.mapping.nozzles[0].flow_size.has_value());
    CHECK(*outcome.mapping.nozzles[0].flow_size == "high");
    REQUIRE(outcome.mapping.nozzles[0].diameter.has_value());
    REQUIRE_THAT(*outcome.mapping.nozzles[0].diameter, Catch::Matchers::WithinAbs(0.6, 1e-9));

    CHECK(outcome.mapping.nozzles[1].id == 0);
    REQUIRE(outcome.mapping.nozzles[1].type.has_value());
    CHECK(*outcome.mapping.nozzles[1].type == "standard");
    CHECK_FALSE(outcome.mapping.nozzles[1].flow_size.has_value());
    REQUIRE(outcome.mapping.nozzles[1].diameter.has_value());
    REQUIRE_THAT(*outcome.mapping.nozzles[1].diameter, Catch::Matchers::WithinAbs(0.4, 1e-9));
}

TEST_CASE("an absent mapping converts to an empty typed mapping", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.task_use_ams = true;

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    CHECK(outcome.mapping.use_ams);
    CHECK(outcome.mapping.ams_mapping_version == PrintMapping::MappingVersion::none);
    CHECK(outcome.mapping.filaments.empty());
    CHECK(outcome.mapping.nozzle_mapping.empty());
    CHECK(outcome.mapping.nozzles.empty());
}

TEST_CASE("PrintParams conversion reads the mapping fields and use_ams", "[PrintMapping]")
{
    Slic3r::PrintParams params{};
    params.ams_mapping2   = R"([{"ams_id":1,"slot_id":3}])";
    params.nozzle_mapping = "[0]";
    params.task_use_ams   = true;

    const ParseOutcome outcome = parse(params);

    REQUIRE(outcome.ok);
    CHECK(outcome.mapping.use_ams);
    REQUIRE(outcome.mapping.filaments.size() == 1);
    CHECK(outcome.mapping.filaments[0].destination.ams_id == 1);
    CHECK(outcome.mapping.filaments[0].destination.slot_id == 3);
    REQUIRE(outcome.mapping.nozzle_mapping.size() == 1);
    CHECK(outcome.mapping.nozzle_mapping[0].nozzle_id == 0);
}

TEST_CASE("malformed or inconsistent mapping input returns an error", "[PrintMapping]")
{
    Slic3r::PrintParams params{};

    SECTION("v1 is not valid JSON") {
        params.ams_mapping2 = "{";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("v1 entry is not an object") {
        params.ams_mapping2 = "[7]";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("v1 ams_id is out of range") {
        params.ams_mapping2 = R"([{"ams_id":256,"slot_id":0}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("v1 external sentinel carries a physical slot") {
        params.ams_mapping2 = R"([{"ams_id":255,"slot_id":3}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("v1 physical ams_id carries the slot sentinel") {
        params.ams_mapping2 = R"([{"ams_id":2,"slot_id":255}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("v0 is not an array") {
        params.ams_mapping = R"({"tray":0})";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("metadata is present without an AMS mapping") {
        params.ams_mapping_info = R"([{"ams":0}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("metadata has more entries than the mapping") {
        params.ams_mapping      = "[0]";
        params.ams_mapping_info = R"([{"ams":0},{"ams":0}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("metadata names a virtual tray for an AMS slot") {
        params.ams_mapping      = "[0]";
        params.ams_mapping_info = R"([{"ams":255}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("metadata names an AMS tray for a virtual tray") {
        params.ams_mapping2     = R"([{"ams_id":255,"slot_id":255}])";
        params.ams_mapping_info = R"([{"ams":0}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("nozzle_mapping contains a non-integer") {
        params.nozzle_mapping = R"(["left"])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("nozzles_info has a non-numeric diameter") {
        params.nozzles_info = R"([{"id":0,"diameter":"wide"}])";
        CHECK_FALSE(parse(params).ok);
    }
    SECTION("nozzles_info entry has no id") {
        params.nozzles_info = R"([{"type":"standard"}])";
        CHECK_FALSE(parse(params).ok);
    }
}
