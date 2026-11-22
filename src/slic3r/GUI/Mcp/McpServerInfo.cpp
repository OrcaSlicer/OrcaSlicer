// What the server says of itself in initialize and server/discover (server_info()): its name, version and the
// instructions text, which lives here.

#include "McpProtocol.hpp"

#include "libslic3r_version.h"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// Server instructions shown to the model; clients truncate them at 4096 characters.
const char* kInstructions =
    "OrcaSlicer, a 3D-printing slicer, driven through its GUI.\n"
    "Workflow: app_state (ready? which presets, plates, dialogs?) -> act -> check with object_list, plate_list, "
    "settings_get, plate_slice's reply or plate_slice_stats; window_screenshot only for what they do not report.\n"
    "Running the session, dialogs, replays and scripts:\n"
    "- A call that takes long or opens a dialog returns {status:\"running\"|\"blocked_by_dialog\", job}. It keeps "
    "running: never retry it. Answer dialogs with dialog_answer (see app_state.dialogs), then app_wait {for:\"job\"}. A "
    "native message box (native: true) takes dialog_answer with one of its listed buttons; a file or folder picker, "
    "or a native dialog with no buttons listed (any on macOS), can't: register dialog_auto_answer before the step "
    "that opens one.\n"
    "- During an operation (slicing, exporting, arranging, another job) other changes are refused: app_wait or "
    "job_cancel.\n"
    "- replay_export saves this session's calls as a replay that replay_run runs.\n"
    "- script_run runs Python against orca.host, the plugin API (script_list_api lists it), to compute over "
    "meshes and settings and call tools (orca.host.tools); its file, program, thread and network use is restricted "
    "unless app_state.python.scripts_unrestricted.\n"
    "- tool_list lists the commands the tool list does not show (the Design tab's, menu items with no arguments, "
    "plugin operations as plugin.<plugin>.<name>); tool_call runs them. tool_describe {tool} gives any command's "
    "manual.\n"
    "Acting on project data by index and name, JSON in and out; prefer these:\n"
    "- Commands are named place first, then verb (object_mirror); tools/list gives them place by place.\n"
    "- A call with exactly a Speed Dial action's arguments (project_export {format:\"gcode\"}) may ask in a dialog or "
    "act on the selection, as a person's pick does. Any other call, with silent: true where a "
    "schema takes it, runs from its arguments; a dialog it still opens (preset_export's always) is in its "
    "description.\n"
    "- Which setting does what: settings_list_pages {type} lists a settings tab's pages, settings_list {type, where} a "
    "page's settings with their descriptions, settings_get {keys} named ones (with no type, each names the type "
    "settings_set needs).\n"
    "- Objects, parts and plates are 0-based, as object_list and plate_list number them; filament slots and Preview "
    "layers are 1-based.\n"
    "- Lists come in pages: {items, offset, total}, next_offset while more remain (call again with it); object_list "
    "has plates and outside for items.\n"
    "- Paths are absolute; an output replaces a file only with overwrite: true.\n"
    "Driving the GUI, for what those do not reach or to see what a person sees:\n"
    "- action_run runs a Speed Dial action by id from action_list: a command's as a call with its arguments does; a "
    "plate, a recent project, a plugin's script or page as a person's pick does.\n"
    "- widget_list lists widgets, menus, 3D toolbar items, gizmos, ImGui items and web page elements with refs; "
    "widget_click / widget_set_value / keyboard_press / mouse_gesture act on its refs (good across later listings) or "
    "on the locators its description lists.\n"
    "- window_list lists the open windows with refs: a window's ref as the target of window_screenshot captures it, "
    "of widget_list lists it alone, and of window_set sizes, moves or raises it.\n"
    "- Gizmos (Cut, Text, SVG, painting...): widget_click {target: {gizmo: \"Cut\"}} opens one; its panel's fields and "
    "buttons are ImGui items for widget_set_value / widget_click, and painting is a mouse_gesture drag from the "
    "object. Move the camera with view_set, never a drag, which follows the user's mouse settings. gizmo_pick_file "
    "stands in for a file picker inside an open gizmo.\n"
    "- window_screenshot returns a downscaled image plus the full-size PNG path; target \"canvas\" is the 3D view.\n"
    "- recording_start / recording_stop record a video (MJPEG .mkv), optionally captioned with the tool calls, the "
    "pointer highlighted; with encode, recording_stop encodes it to MP4 (system ffmpeg) as a job, and "
    "recording_encode encodes any later; an encode deletes this session's .mkv once the MP4 is written, unless "
    "keep_raw_video_file.";

} // namespace

ServerInfo server_info()
{
    ServerInfo info;
    info.version      = SLIC3R_VERSION;
    info.instructions = kInstructions;
    return info;
}

}}} // namespace Slic3r::GUI::Mcp
