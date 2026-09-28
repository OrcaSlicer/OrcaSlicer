#include <catch2/catch_all.hpp>

#include "libslic3r/Format/AssembleList.hpp"

#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

static constexpr int max_plates = 36;

static AssembleListResult load_text(const std::string &text, std::vector<assemble_plate_info_t> &plates)
{
    ScopedTemporaryFile file(".json");
    {
        boost::nowide::ofstream out(file.string());
        out << text;
    }
    return load_assemble_plate_list(file.string(), plates, max_plates);
}

static AssembleListResult load_json(const json &root)
{
    std::vector<assemble_plate_info_t> plates;
    return load_text(root.dump(), plates);
}

// One plate with one object of three clones, which every optional field accepts.
static json valid_list()
{
    return json::parse(R"({
        "plates": [{
            "plate_name": "plate",
            "need_arrange": false,
            "objects": [{
                "path": "cube.stl",
                "count": 3,
                "filaments": [1],
                "height_ranges": [{ "min_z": 0, "max_z": 5, "range_params": { "layer_height": "0.1" } }]
            }],
            "assembled_params": [{
                "assemble_index": 1,
                "height_ranges": [{ "min_z": 0, "max_z": 5, "range_params": { "layer_height": "0.1" } }]
            }]
        }]
    })");
}

TEST_CASE("A valid assemble list parses into its plates and objects", "[AssembleList]")
{
    const std::string text = R"({
        "plates": [
            {
                "plate_name": "first",
                "need_arrange": true,
                "plate_params": { "curr_bed_type": "Textured PEI Plate" },
                "objects": [
                    {
                        "path": "a.stl",
                        "count": 2,
                        "filaments": [1, 3],
                        "assemble_index": [1],
                        "pos_x": [10.5, 20.5],
                        "pos_y": [30],
                        "pos_z": [0, 1],
                        "print_params": { "sparse_infill_density": "30%" },
                        "height_ranges": [{ "min_z": 1.5, "max_z": 4, "range_params": { "layer_height": "0.12" } }]
                    },
                    { "path": "b.stl", "count": 1, "filaments": [0] }
                ],
                "assembled_params": [{ "assemble_index": 1, "print_params": { "wall_loops": "4" } }]
            },
            {
                "plate_name": "second",
                "need_arrange": false,
                "objects": [{ "path": "c.stl", "count": 1, "filaments": [2] }]
            }
        ]
    })";
    std::vector<assemble_plate_info_t> plates;
    REQUIRE(load_text(text, plates) == AssembleListResult::Success);
    REQUIRE(plates.size() == 2);

    const assemble_plate_info_t &first = plates[0];
    CHECK(first.plate_name == "first");
    CHECK(first.need_arrange);
    CHECK(first.plate_params.at("curr_bed_type") == "Textured PEI Plate");
    REQUIRE(first.assemble_obj_list.size() == 2);

    const assemble_object_info_t &a = first.assemble_obj_list[0];
    CHECK(a.path == "a.stl");
    CHECK(a.count == 2);
    CHECK(a.filaments == std::vector<int>{1, 3});
    CHECK(a.assemble_index == std::vector<int>{1});
    REQUIRE(a.pos_x.size() == 2);
    CHECK_THAT(a.pos_x[0], WithinAbs(10.5, 1e-6));
    CHECK_THAT(a.pos_x[1], WithinAbs(20.5, 1e-6));
    REQUIRE(a.pos_y.size() == 1);
    CHECK_THAT(a.pos_y[0], WithinAbs(30., 1e-6));
    REQUIRE(a.pos_z.size() == 2);
    CHECK_THAT(a.pos_z[1], WithinAbs(1., 1e-6));
    CHECK(a.print_params.at("sparse_infill_density") == "30%");
    REQUIRE(a.height_ranges.size() == 1);
    CHECK_THAT(a.height_ranges[0].min_z, WithinAbs(1.5, 1e-6));
    CHECK_THAT(a.height_ranges[0].max_z, WithinAbs(4., 1e-6));
    CHECK(a.height_ranges[0].range_params.at("layer_height") == "0.12");

    const assemble_object_info_t &b = first.assemble_obj_list[1];
    CHECK(b.path == "b.stl");
    CHECK(b.count == 1);
    CHECK(b.filaments == std::vector<int>{0});
    CHECK(b.pos_x.empty());
    CHECK(b.assemble_index.empty());

    REQUIRE(first.assembled_param_list.count(1) == 1);
    CHECK(first.assembled_param_list.at(1).print_params.at("wall_loops") == "4");

    const assemble_plate_info_t &second = plates[1];
    CHECK(second.plate_name == "second");
    CHECK_FALSE(second.need_arrange);
    REQUIRE(second.assemble_obj_list.size() == 1);
    CHECK(second.assemble_obj_list[0].path == "c.stl");
    CHECK(second.assemble_obj_list[0].filaments == std::vector<int>{2});
}

TEST_CASE("The unmodified fixture used by the rule tests is accepted", "[AssembleList]")
{
    CHECK(load_json(valid_list()) == AssembleListResult::Success);
}

TEST_CASE("An object with an empty filament list is rejected", "[AssembleList]")
{
    json root = valid_list();
    root["plates"][0]["objects"][0]["filaments"] = json::array();
    CHECK(load_json(root) == AssembleListResult::ConfigError);
}

TEST_CASE("An object with a negative filament id is rejected", "[AssembleList]")
{
    json root = valid_list();
    root["plates"][0]["objects"][0]["filaments"] = GENERATE(json::array({-1}), json::array({1, -2, 1}));
    CAPTURE(root["plates"][0]["objects"][0]["filaments"].dump());
    CHECK(load_json(root) == AssembleListResult::ConfigError);
}

TEST_CASE("Filament id 0 is accepted", "[AssembleList]")
{
    json root = valid_list();
    root["plates"][0]["objects"][0]["filaments"] = GENERATE(json::array({0}), json::array({0, 1, 0}));
    CAPTURE(root["plates"][0]["objects"][0]["filaments"].dump());
    CHECK(load_json(root) == AssembleListResult::Success);
}

TEST_CASE("Per-clone lists need one entry or one per clone", "[AssembleList]")
{
    // The fixture object has 3 clones.
    const std::string key  = GENERATE("filaments", "assemble_index", "pos_x", "pos_y", "pos_z");
    const size_t      size = GENERATE(1, 2, 3, 4);
    CAPTURE(key, size);

    json root = valid_list();
    root["plates"][0]["objects"][0][key] = json(std::vector<int>(size, 1));
    const AssembleListResult expected = (size == 1 || size == 3) ? AssembleListResult::Success : AssembleListResult::ConfigError;
    CHECK(load_json(root) == expected);
}

TEST_CASE("An empty optional per-clone list is accepted", "[AssembleList]")
{
    const std::string key = GENERATE("assemble_index", "pos_x", "pos_y", "pos_z");
    CAPTURE(key);

    json root = valid_list();
    root["plates"][0]["objects"][0][key] = json::array();
    CHECK(load_json(root) == AssembleListResult::Success);
}

// Fields read through a const reference (plate_name, need_arrange, objects, path, count) are
// looked up without a presence check, so only their wrong-type case is covered here.
TEST_CASE("A missing required field is rejected", "[AssembleList]")
{
    const std::string pointer = GENERATE("/plates",
                                         "/plates/0/objects/0/filaments",
                                         "/plates/0/objects/0/height_ranges/0/min_z",
                                         "/plates/0/objects/0/height_ranges/0/max_z",
                                         "/plates/0/objects/0/height_ranges/0/range_params",
                                         "/plates/0/assembled_params/0/assemble_index",
                                         "/plates/0/assembled_params/0/height_ranges/0/min_z",
                                         "/plates/0/assembled_params/0/height_ranges/0/max_z",
                                         "/plates/0/assembled_params/0/height_ranges/0/range_params");
    CAPTURE(pointer);

    json root = valid_list();
    const json::json_pointer ptr(pointer);
    root[ptr.parent_pointer()].erase(ptr.back());
    CHECK(load_json(root) == AssembleListResult::ConfigError);
}

TEST_CASE("A field of the wrong type is rejected", "[AssembleList]")
{
    const std::string pointer = GENERATE("/plates/0/plate_name",
                                         "/plates/0/need_arrange",
                                         "/plates/0/objects/0/path",
                                         "/plates/0/objects/0/count",
                                         "/plates/0/objects/0/filaments",
                                         "/plates/0/objects/0/pos_x");
    CAPTURE(pointer);

    json root = valid_list();
    root[json::json_pointer(pointer)] = json::object();
    CHECK(load_json(root) == AssembleListResult::ConfigError);
}

TEST_CASE("A plate or clone count out of range is rejected", "[AssembleList]")
{
    SECTION("no plates")
    {
        json root = valid_list();
        root["plates"] = json::array();
        CHECK(load_json(root) == AssembleListResult::ConfigError);
    }
    SECTION("more plates than the limit")
    {
        json root = valid_list();
        const json plate = root["plates"][0];
        for (int i = 1; i < max_plates; ++i)
            root["plates"].push_back(plate);
        CHECK(load_json(root) == AssembleListResult::Success);
        root["plates"].push_back(plate);
        CHECK(load_json(root) == AssembleListResult::ConfigError);
    }
    SECTION("a plate with no objects")
    {
        json root = valid_list();
        root["plates"][0]["objects"] = json::array();
        CHECK(load_json(root) == AssembleListResult::ConfigError);
    }
    SECTION("a clone count below 1")
    {
        json root = valid_list();
        root["plates"][0]["objects"][0]["count"] = GENERATE(0, -1);
        CAPTURE(root["plates"][0]["objects"][0]["count"].dump());
        CHECK(load_json(root) == AssembleListResult::ConfigError);
    }
}

TEST_CASE("Malformed JSON is rejected", "[AssembleList]")
{
    const std::string text = GENERATE(std::string(), std::string("{\"plates\": ["), std::string("not json"));
    CAPTURE(text);
    std::vector<assemble_plate_info_t> plates;
    CHECK(load_text(text, plates) == AssembleListResult::ConfigError);
}

TEST_CASE("A missing file is reported as not found", "[AssembleList]")
{
    ScopedTemporaryFile        file(".json");
    std::vector<assemble_plate_info_t> plates;
    CHECK(load_assemble_plate_list(file.string(), plates, max_plates) == AssembleListResult::FileNotFound);
}
