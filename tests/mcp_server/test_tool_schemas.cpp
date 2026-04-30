#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>
#include "MCP/ToolSchemas.h"

#include <set>

using json = nlohmann::json;
using namespace Slic3r::GUI;

TEST_CASE("Tool schema definitions", "[MCP][ToolSchemas]") {
    auto tools = get_all_tool_definitions();

    SECTION("get_all_tool_definitions returns 21 tools") {
        REQUIRE(tools.size() == 21);
    }

    SECTION("Each tool has non-empty name and description") {
        for (const auto& tool : tools) {
            INFO("Tool: " + tool.name);
            REQUIRE_FALSE(tool.name.empty());
            REQUIRE_FALSE(tool.description.empty());
        }
    }

    SECTION("Each inputSchema has type=object") {
        for (const auto& tool : tools) {
            INFO("Tool: " + tool.name);
            REQUIRE(tool.input_schema.contains("type"));
            REQUIRE(tool.input_schema["type"] == "object");
        }
    }

    SECTION("No duplicate tool names") {
        std::set<std::string> names;
        for (const auto& tool : tools) {
            INFO("Duplicate tool name: " + tool.name);
            REQUIRE(names.insert(tool.name).second);
        }
    }

    SECTION("Screenshot tool exists with is_screenshot=true") {
        const McpToolDef* screenshot = nullptr;
        for (const auto& tool : tools) {
            if (tool.name == "screenshot") {
                screenshot = &tool;
                break;
            }
        }
        REQUIRE(screenshot != nullptr);
        REQUIRE(screenshot->is_screenshot);
    }

    SECTION("Screenshot tool has width, height, mode properties") {
        const McpToolDef* screenshot = nullptr;
        for (const auto& tool : tools) {
            if (tool.name == "screenshot") {
                screenshot = &tool;
                break;
            }
        }
        REQUIRE(screenshot != nullptr);

        auto props = screenshot->input_schema["properties"];
        REQUIRE(props.contains("width"));
        REQUIRE(props.contains("height"));
        REQUIRE(props.contains("mode"));
    }

    SECTION("model_add_primitive has required field with type") {
        const McpToolDef* tool = nullptr;
        for (const auto& t : tools) {
            if (t.name == "model_add_primitive") {
                tool = &t;
                break;
            }
        }
        REQUIRE(tool != nullptr);
        REQUIRE(tool->input_schema.contains("required"));

        auto required = tool->input_schema["required"];
        REQUIRE(required.is_array());
        bool has_type = false;
        for (const auto& r : required) {
            if (r == "type") has_type = true;
        }
        REQUIRE(has_type);
    }

    SECTION("config_get has required field with keys") {
        const McpToolDef* tool = nullptr;
        for (const auto& t : tools) {
            if (t.name == "config_get") {
                tool = &t;
                break;
            }
        }
        REQUIRE(tool != nullptr);
        REQUIRE(tool->input_schema.contains("required"));

        auto required = tool->input_schema["required"];
        REQUIRE(required.is_array());
        bool has_keys = false;
        for (const auto& r : required) {
            if (r == "keys") has_keys = true;
        }
        REQUIRE(has_keys);
    }

    SECTION("Each non-screenshot tool has a non-empty dispatch_action") {
        for (const auto& tool : tools) {
            if (tool.is_screenshot) continue;
            INFO("Tool: " + tool.name);
            REQUIRE_FALSE(tool.dispatch_action.empty());
        }
    }

    SECTION("Only screenshot tool has is_screenshot=true") {
        for (const auto& tool : tools) {
            INFO("Tool: " + tool.name);
            if (tool.name == "screenshot") {
                REQUIRE(tool.is_screenshot);
            } else {
                REQUIRE_FALSE(tool.is_screenshot);
            }
        }
    }

    SECTION("object_transform has translate, scale, rotate properties") {
        const McpToolDef* tool = nullptr;
        for (const auto& t : tools) {
            if (t.name == "object_transform") {
                tool = &t;
                break;
            }
        }
        REQUIRE(tool != nullptr);
        auto props = tool->input_schema["properties"];
        REQUIRE(props.contains("index"));
        REQUIRE(props.contains("translate"));
        REQUIRE(props.contains("scale"));
        REQUIRE(props.contains("rotate"));
    }

    SECTION("Tools with required fields have them listed correctly") {
        std::map<std::string, std::vector<std::string>> expected_required = {
            {"model_add_primitive", {"type"}},
            {"model_load_file", {"path"}},
            {"model_delete_object", {"index"}},
            {"object_transform", {"index"}},
            {"model_export", {"path"}},
            {"config_get", {"keys"}},
            {"config_set", {"settings"}},
            {"mesh_stats", {"index"}},
            {"viewport_select_view", {"view"}},
            {"viewport_zoom", {"delta"}},
        };
        for (const auto& [tool_name, req_fields] : expected_required) {
            const McpToolDef* tool = nullptr;
            for (const auto& t : tools) {
                if (t.name == tool_name) { tool = &t; break; }
            }
            INFO("Tool: " + tool_name);
            REQUIRE(tool != nullptr);
            REQUIRE(tool->input_schema.contains("required"));
            auto required = tool->input_schema["required"];
            REQUIRE(required.size() == req_fields.size());
            for (const auto& field : req_fields) {
                bool found = false;
                for (const auto& r : required) {
                    if (r == field) found = true;
                }
                INFO("Missing required field: " + field);
                REQUIRE(found);
            }
        }
    }
}
