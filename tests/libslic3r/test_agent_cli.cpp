#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AgentCli.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("Agent help starts at model, svg, slice, and configuration", "[AgentCli]") {
    const nlohmann::json help = agent_help_json();
    REQUIRE(help["status"] == "ok");
    REQUIRE(help["data"]["groups"].size() == 4);
    REQUIRE(help["data"]["groups"][0]["id"] == "model");
    REQUIRE(help["data"]["groups"][1]["id"] == "svg");
    REQUIRE(help["data"]["groups"][2]["id"] == "slice");
    REQUIRE(help["data"]["groups"][3]["id"] == "configuration");
    REQUIRE(help.dump().find("sparse_infill_pattern") == std::string::npos);
    REQUIRE(help["data"]["recipe"]["steps"][0]["id"] == "model.edit.scale");
}

TEST_CASE("Agent search finds split and merge without listing settings", "[AgentCli]") {
    const nlohmann::json found = agent_search_json("model", "split", "", 12, 0);
    REQUIRE(found["status"] == "ok");
    std::vector<std::string> ids;
    for (const nlohmann::json &hit : found["data"]["matches"])
        ids.push_back(hit["id"].get<std::string>());
    REQUIRE(std::find(ids.begin(), ids.end(), "model.split.objects") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "model.split.parts") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "layer_height") == ids.end());

    const nlohmann::json merged = agent_search_json("model", "merge", "", 12, 0);
    REQUIRE(merged["data"]["matches"][0]["id"] == "model.merge");
    REQUIRE(merged["data"]["matches"][0]["availability"] == "live_gui");
}

TEST_CASE("Agent configuration search returns categories until a query or category is given", "[AgentCli]") {
    const nlohmann::json categories = agent_search_json("configuration", "", "", 12, 0);
    REQUIRE(categories["data"]["kind"] == "categories");
    REQUIRE(categories["data"]["categories"].size() > 5);
    REQUIRE_FALSE(categories["data"].contains("matches"));

    bool quality = false;
    for (const nlohmann::json &category : categories["data"]["categories"]) {
        if (category["id"] == "Quality")
            quality = true;
    }
    REQUIRE(quality);

    const nlohmann::json quality_keys = agent_search_json("configuration", "", "Quality", 12, 0);
    REQUIRE(quality_keys["data"]["kind"] == "settings");
    const int quality_total = quality_keys["data"]["total"].get<int>();
    REQUIRE(quality_total > 0);
    const int quality_shown = quality_total < 12 ? quality_total : 12;
    REQUIRE(quality_keys["data"]["matches"].size() == quality_shown);
    REQUIRE(quality_keys["data"]["truncated"] == (quality_total > 12));

    const nlohmann::json layers = agent_search_json("configuration", "layer_height", "", 30, 0);
    bool saw_layer_height = false;
    for (const nlohmann::json &hit : layers["data"]["matches"]) {
        if (hit["id"] == "layer_height")
            saw_layer_height = true;
    }
    REQUIRE(saw_layer_height);
    REQUIRE(layers["data"]["matches"].size() <= 30);
}

TEST_CASE("Agent describe of scale uses the existing transform definition", "[AgentCli]") {
    const nlohmann::json described = agent_describe_json("model.edit.scale");
    REQUIRE(described["status"] == "ok");
    const ConfigOptionDef *def = cli_transform_config_def.get("scale");
    REQUIRE(def != nullptr);
    REQUIRE(described["data"]["legacy_flag"] == "--scale");
    REQUIRE(described["data"]["value"]["type"] == "float");
    REQUIRE(described["data"]["value"]["default"] == def->default_value->serialize());
    REQUIRE(described["data"]["owner"] == "CLI transform scale");
}

TEST_CASE("Agent describe of split names the model kernel", "[AgentCli]") {
    const nlohmann::json described = agent_describe_json("model.split.objects");
    REQUIRE(described["status"] == "ok");
    REQUIRE(described["data"]["availability"] == "live_gui");
    REQUIRE(described["data"]["owner"] == "ModelObject::split");
    REQUIRE(described["data"]["unavailable"]["code"] == "live_gui_required");
}

TEST_CASE("Agent describe of layer height uses the print setting definition", "[AgentCli]") {
    const nlohmann::json described = agent_describe_json("layer_height");
    const ConfigOptionDef *def = print_config_def.get("layer_height");
    REQUIRE(def != nullptr);
    REQUIRE(described["data"]["group"] == "configuration");
    REQUIRE(described["data"]["value"]["type"] == "float");
    REQUIRE(described["data"]["value"]["category"] == "Quality");
    REQUIRE(described["data"]["legacy_flag"] == "--layer-height");
    REQUIRE(described["data"]["value"]["default"] == def->default_value->serialize());
}

TEST_CASE("Agent invoke plans a scale and export through the legacy CLI", "[AgentCli]") {
    nlohmann::json recipe;
    recipe["files"] = nlohmann::json::array();
    recipe["files"].push_back("part.stl");
    recipe["steps"] = nlohmann::json::array();
    recipe["steps"].push_back({{"id", "model.edit.scale"}, {"value", 2}});
    recipe["steps"].push_back({{"id", "slice.export_stl"}, {"value", true}});
    const AgentCliResult planned = agent_invoke_json(recipe);
    REQUIRE(planned.forward_to_legacy);
    REQUIRE(planned.exit_code == 0);
    REQUIRE(planned.legacy_args.size() == 3);
    REQUIRE(planned.legacy_args[0] == "part.stl");
    REQUIRE(planned.legacy_args[1] == "--scale=2");
    REQUIRE(planned.legacy_args[2] == "--export-stl=1");
}

TEST_CASE("Agent invoke refuses split and a transform that would open the window", "[AgentCli]") {
    nlohmann::json split;
    split["files"] = nlohmann::json::array();
    split["files"].push_back("part.stl");
    split["steps"] = nlohmann::json::array();
    split["steps"].push_back({{"id", "model.split.objects"}});
    const AgentCliResult refused = agent_invoke_json(split);
    REQUIRE_FALSE(refused.forward_to_legacy);
    const nlohmann::json body = nlohmann::json::parse(refused.stdout_text);
    REQUIRE(body["errors"][0]["code"] == "live_gui_required");

    nlohmann::json scale_only;
    scale_only["files"] = nlohmann::json::array();
    scale_only["files"].push_back("part.stl");
    scale_only["steps"] = nlohmann::json::array();
    scale_only["steps"].push_back({{"id", "model.edit.scale"}, {"value", 2}});
    const AgentCliResult window = agent_invoke_json(scale_only);
    REQUIRE_FALSE(window.forward_to_legacy);
    REQUIRE(nlohmann::json::parse(window.stdout_text)["errors"][0]["code"] == "needs_output_action");
}

TEST_CASE("Agent invoke rejects an option-like file name", "[AgentCli]") {
    nlohmann::json recipe;
    recipe["files"] = nlohmann::json::array();
    recipe["files"].push_back("-part.stl");
    recipe["steps"] = nlohmann::json::array();
    recipe["steps"].push_back({{"id", "slice.export_stl"}, {"value", true}});
    const AgentCliResult planned = agent_invoke_json(recipe);
    REQUIRE_FALSE(planned.forward_to_legacy);
    REQUIRE(nlohmann::json::parse(planned.stdout_text)["errors"][0]["code"] == "invalid_input");
}

TEST_CASE("Agent help command returns the index and does not forward", "[AgentCli]") {
    char program[] = "orca-slicer";
    char agent[] = "agent";
    char *argv[] = {program, agent, nullptr};
    const AgentCliResult result = agent_cli_run(2, argv);
    REQUIRE_FALSE(result.forward_to_legacy);
    REQUIRE(result.exit_code == 0);
    const nlohmann::json body = nlohmann::json::parse(result.stdout_text);
    REQUIRE(body["operation"] == "help");
    REQUIRE(body["data"]["groups"][0]["id"] == "model");
}
