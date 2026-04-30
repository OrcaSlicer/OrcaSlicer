#include "MCP/ToolSchemas.h"

using json = nlohmann::json;

namespace Slic3r { namespace GUI {

std::vector<McpToolDef> get_all_tool_definitions()
{
    std::vector<McpToolDef> tools;

    // -----------------------------------------------------------------------
    // screenshot (special handler, not dispatched via CommandDispatch)
    // -----------------------------------------------------------------------
    tools.push_back({
        "screenshot",
        "Capture the current OrcaSlicer viewport as a PNG image. Returns a base64-encoded PNG image. By default captures exactly what the user sees (viewport mode), including UI elements. Use mode=thumbnail for a clean orthographic render without UI overlays via FBO off-screen rendering.",
        {
            {"type", "object"},
            {"properties", {
                {"width",  {{"type", "integer"}, {"description", "Image width in pixels (only used in thumbnail mode)"}}},
                {"height", {{"type", "integer"}, {"description", "Image height in pixels (only used in thumbnail mode)"}}},
                {"mode",   {{"type", "string"}, {"enum", json::array({"viewport", "thumbnail"})}, {"description", "Capture mode: 'viewport' (default) captures the actual screen, 'thumbnail' uses FBO orthographic rendering"}}}
            }}
        },
        "", // no dispatch action
        true // is_screenshot
    });

    // -----------------------------------------------------------------------
    // Model tools
    // -----------------------------------------------------------------------
    tools.push_back({
        "model_list_objects",
        "List all objects currently on the build plate. Returns an array of objects with their name, index, volume count, and axis-aligned bounding box (min/max XYZ coordinates in mm). Use the returned index to reference objects in other tools like object_transform, model_delete_object, or mesh_stats.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "model.list"
    });

    tools.push_back({
        "model_add_primitive",
        "Add a primitive shape (cube, cylinder, or sphere) to the build plate. Default size is 20mm per axis. Dimensions x/y/z override the default. The object is placed at the center of the build plate. Use object_transform afterwards to position, scale, or rotate it. For example, a sphere can be scaled non-uniformly to create an ellipsoid.",
        {
            {"type", "object"},
            {"properties", {
                {"type", {{"type", "string"}, {"enum", json::array({"cube", "cylinder", "sphere"})}, {"description", "Primitive shape type"}}},
                {"x",    {{"type", "number"}, {"description", "Size in mm along the X axis (default: 20mm)"}}},
                {"y",    {{"type", "number"}, {"description", "Size in mm along the Y axis (default: 20mm)"}}},
                {"z",    {{"type", "number"}, {"description", "Size in mm along the Z axis (default: 20mm)"}}}
            }},
            {"required", json::array({"type"})}
        },
        "model.add_primitive"
    });

    tools.push_back({
        "model_load_file",
        "Load a 3D model file onto the build plate. Supports STL, 3MF, OBJ, and STEP formats. The path must be absolute. Large models (1M+ triangles) may cause slower processing. The object is auto-centered on the build plate after loading.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "Absolute path to the model file"}}}
            }},
            {"required", json::array({"path"})}
        },
        "model.load_file"
    });

    tools.push_back({
        "model_delete_object",
        "Delete an object from the build plate by its index. Get the object index from model_list_objects first. Indices may shift after deletion -- re-query model_list_objects if deleting multiple objects.",
        {
            {"type", "object"},
            {"properties", {
                {"index", {{"type", "integer"}, {"description", "Index of the object to delete"}}}
            }},
            {"required", json::array({"index"})}
        },
        "model.delete"
    });

    tools.push_back({
        "object_transform",
        "Apply translate, scale, and/or rotate transforms to an object on the build plate. Translate sets absolute position [x, y, z] in mm (not a relative offset). Scale multiplies the original dimensions -- e.g., [2,1,1] doubles width. Rotate sets angles in degrees around each axis. Multiple transforms can be combined in one call. Note: OrcaSlicer may auto-drop objects to the bed (Z=0); objects cannot be positioned below Z=0 for printing.",
        {
            {"type", "object"},
            {"properties", {
                {"index",     {{"type", "integer"}, {"description", "Index of the object to transform"}}},
                {"translate", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", "Absolute position [x, y, z] in mm. The object's center will be placed at these coordinates."}}},
                {"scale",     {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", "Scale multipliers [x, y, z] relative to original size. E.g., [2, 1, 0.5] doubles width, keeps depth, halves height."}}},
                {"rotate",    {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", "Rotation angles [x, y, z] in degrees. Applied around each respective axis."}}}
            }},
            {"required", json::array({"index"})}
        },
        "model.transform"
    });

    tools.push_back({
        "model_export",
        "Export the current plate contents to a file. 3MF format preserves all metadata and multi-object structure; STL exports only geometry. The output path must be absolute with the appropriate file extension (.3mf or .stl).",
        {
            {"type", "object"},
            {"properties", {
                {"path",   {{"type", "string"}, {"description", "Absolute path for the exported file"}}},
                {"format", {{"type", "string"}, {"enum", json::array({"3mf", "stl"})}, {"description", "Export format (default: 3mf)"}}}
            }},
            {"required", json::array({"path"})}
        },
        "model.export"
    });

    // -----------------------------------------------------------------------
    // Config tools
    // -----------------------------------------------------------------------
    tools.push_back({
        "config_get",
        "Retrieve current values of slicer configuration keys. Keys include print settings (layer_height, infill_density, wall_loops, etc.), printer settings, and filament settings. Use config_list_options to discover available keys. Returns a JSON object mapping each key to its current value.",
        {
            {"type", "object"},
            {"properties", {
                {"keys", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "List of config keys to retrieve"}}}
            }},
            {"required", json::array({"keys"})}
        },
        "config.get"
    });

    tools.push_back({
        "config_set",
        "Update slicer configuration settings. Pass a JSON object with key-value pairs. Changes take effect immediately and affect subsequent slicing. Example: {\"settings\": {\"layer_height\": \"0.2\", \"infill_density\": \"15%\"}}. Use config_list_options to discover valid keys and value ranges.",
        {
            {"type", "object"},
            {"properties", {
                {"settings", {{"type", "object"}, {"additionalProperties", true}, {"description", "Key-value pairs of config settings to update"}}}
            }},
            {"required", json::array({"settings"})}
        },
        "config.set"
    });

    tools.push_back({
        "config_list_options",
        "List available configuration options with their types, default values, and valid ranges. Use the optional filter parameter to search by keyword -- e.g., filter='infill' returns all infill-related settings. Useful for discovering what settings are available before using config_get or config_set.",
        {
            {"type", "object"},
            {"properties", {
                {"filter", {{"type", "string"}, {"description", "Filter string to narrow results"}}}
            }}
        },
        "config.list"
    });

    tools.push_back({
        "config_load_profile",
        "Load a complete printer, material, or process profile by name or file path. Profile names should match those shown in the OrcaSlicer UI (e.g., '0.20mm Standard @BBL A1'). This replaces the current settings for that profile type. Provide either name or path, not both.",
        {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}, {"description", "Profile name to load"}}},
                {"path", {{"type", "string"}, {"description", "File path to a profile"}}}
            }}
        },
        "config.load_profile"
    });

    // -----------------------------------------------------------------------
    // Diagnostics tools
    // -----------------------------------------------------------------------
    tools.push_back({
        "mesh_stats",
        "Get detailed mesh statistics for an object: vertex count, face count, bounding box dimensions, volume (cm3), surface area, and mesh repair status. Useful for checking model complexity before slicing. Get the object index from model_list_objects.",
        {
            {"type", "object"},
            {"properties", {
                {"index", {{"type", "integer"}, {"description", "Index of the object"}}}
            }},
            {"required", json::array({"index"})}
        },
        "diagnostics.mesh_stats"
    });

    tools.push_back({
        "validate_print",
        "Check the current print setup for issues without slicing. Reports warnings (e.g., objects outside printable area, unsupported overhangs) and errors (e.g., no objects on plate, invalid settings). Run this before slice_and_stats to catch problems early.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "diagnostics.validate"
    });

    tools.push_back({
        "slice_and_stats",
        "Slice the current plate and return statistics including estimated print time, filament usage (length and weight), total layers, and per-object details. This triggers a full slice operation which may take several seconds for complex models. Check slice_status for progress on long-running slices.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "diagnostics.slice"
    });

    tools.push_back({
        "slice_status",
        "Check whether slicing is currently in progress, completed, or not started. When complete, returns print time estimates, filament usage, layer count, and any warnings. Use this to poll for completion after calling slice_and_stats, or to get cached results from the last slice.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "diagnostics.slice_status"
    });

    tools.push_back({
        "get_state",
        "Get a comprehensive snapshot of OrcaSlicer's current state: all objects on the plate (with bounding boxes), active printer/filament/process profiles, plate dimensions and printable area, current camera position/zoom, and whether slicing is in progress. This is the best starting point to understand what is currently loaded.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "diagnostics.state"
    });

    // -----------------------------------------------------------------------
    // Viewport tools
    // -----------------------------------------------------------------------
    tools.push_back({
        "viewport_select_view",
        "Set the 3D viewport to a standard camera angle. Available presets: front, rear, top, bottom, left, right, iso (isometric 3/4 view), topfront (angled top-down). Useful for taking consistent screenshots from known angles.",
        {
            {"type", "object"},
            {"properties", {
                {"view", {{"type", "string"}, {"enum", json::array({"front", "rear", "top", "bottom", "left", "right", "iso", "topfront"})}, {"description", "The view direction to select"}}}
            }},
            {"required", json::array({"view"})}
        },
        "viewport.select_view"
    });

    tools.push_back({
        "viewport_zoom_to_bed",
        "Reset the viewport to show the entire build plate. Useful after zooming in on details, or to get an overview of all objects on the plate.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "viewport.zoom_to_bed"
    });

    tools.push_back({
        "viewport_zoom_to_volumes",
        "Auto-zoom the viewport to fit all objects tightly in view. Provides a tighter view than zoom_to_bed when objects don't fill the entire plate. Best used after adding or repositioning objects.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "viewport.zoom_to_volumes"
    });

    tools.push_back({
        "viewport_zoom",
        "Adjust the viewport zoom level incrementally. Positive delta zooms in, negative zooms out. Typical range: -5.0 to 5.0 for noticeable changes. Combine with viewport_select_view for precise camera positioning before taking screenshots.",
        {
            {"type", "object"},
            {"properties", {
                {"delta", {{"type", "number"}, {"description", "Zoom delta (positive = zoom in, negative = zoom out)"}}}
            }},
            {"required", json::array({"delta"})}
        },
        "viewport.zoom"
    });

    tools.push_back({
        "viewport_camera_info",
        "Get the current camera state: projection type (perspective/orthographic), position [x,y,z], look-at target [x,y,z], zoom level, and viewport pixel dimensions. Useful for saving/restoring camera positions or calculating object visibility.",
        {
            {"type", "object"},
            {"properties", json::object()}
        },
        "viewport.camera_info"
    });

    return tools;
}

}} // namespace Slic3r::GUI
