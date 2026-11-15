// What the server says of itself in initialize and server/discover (server_info()): its name, version and the
// instructions text, which lives here.

#include "McpProtocol.hpp"

#include "libslic3r_version.h"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// Server instructions shown to the model; clients truncate them at 4096 characters.
const char* kInstructions =
    "OrcaSlicer, a 3D-printing slicer, driven through its GUI.\n"
    "Workflow: get_app_state (is it ready? which presets, plates, dialogs?) -> act -> check it with the reading "
    "tools (list_objects, list_plates, get_settings, slice's reply, get_slice_stats); screenshot only to see what they "
    "do not report.\n"
    "Running the session, dialogs, replays and scripts:\n"
    "- A call that takes long or opens a dialog returns {status:\"running\"|\"blocked_by_dialog\", job}. It keeps "
    "running: never retry it. Answer dialogs with answer_dialog (see get_app_state.dialogs), then wait {for:\"job\"}. "
    "A native message box (native: true) is answered with answer_dialog by one of the buttons listed for it; a file or "
    "folder picker, or a native dialog whose buttons list is empty (any on macOS), can't be: register auto_answer_dialogs "
    "before the step that opens one.\n"
    "- While an operation (slice, export, arrange or another background job) runs, other changes are refused; use wait "
    "or cancel_job.\n"
    "- export_replay saves this session's calls as a replay that run_replay runs.\n"
    "- run_script runs Python against orca.host, the plugin API (search_python_api searches it), computing over meshes and "
    "settings and calling tools through orca.host.tools; its own file, program, thread and network use is restricted "
    "unless get_app_state.python.scripts_unrestricted.\n"
    "- Not sure what does something? search_tools {query} searches every catalog at once: the commands, with those the "
    "tool list does not show (Speed Dial commands, the Design tab's, and operations plugins offer as "
    "plugin.<plugin>.<name>; call_tool runs them), the Speed Dial's other actions, settings and the Python API, each "
    "match with how to use it.\n"
    "Acting on project data by index and name, JSON in and out; prefer these:\n"
    "- Typed tools: open_files, select_preset, set_settings, slice, export and the rest.\n"
    "- What a setting does, or which one controls an effect: get_settings with keys, or query with a few words, and no "
    "scope, which looks through the process, filament and printer settings; each item names its scope, the one "
    "set_settings needs (its default is the process), and its meta has its description and where (its settings page and "
    "group).\n"
    "- Objects, parts and plates are 0-based indices, as list_objects and list_plates give "
    "them; filament slots and Preview layers are 1-based.\n"
    "- Lists come in pages: {items, offset, total}, with next_offset while more remain; call again with that offset to "
    "read on. list_objects has plates and outside in place of items.\n"
    "- Paths are absolute. Outputs never overwrite a file unless overwrite is true.\n"
    "Driving the GUI itself, for what those do not reach or to see what a person sees:\n"
    "- run_action runs every Speed Dial command (search_actions finds them): prefer it to GUI clicks.\n"
    "- list_widgets lists widgets, menus, 3D toolbar items, gizmos, ImGui items and web page elements with refs; click "
    "/ set_widget_value / press_keys / mouse_gesture act on its refs, good across later listings, or on locators, which "
    "its description lists.\n"
    "- list_windows lists the open windows, modal or not, with refs: a window's ref as screenshot's target captures "
    "that window, as list_widgets's target lists it alone, and as set_window's target sizes, moves or raises it.\n"
    "- Gizmos (Cut, Text, SVG, the painting tools...): click {target: {gizmo: \"Cut\"}} opens one; its panel's "
    "fields and buttons are ImGui items for set_widget_value / click, and painting is a mouse_gesture drag starting on the "
    "object. Move the camera with set_view, never a drag: what a drag does follows the user's mouse settings. pick_gizmo_file only stands in for a file picker inside an open gizmo.\n"
    "- screenshot returns a downscaled image plus the full-size PNG path; target \"canvas\" is the 3D view.\n"
    "- start_recording / stop_recording record a video (MJPEG .mkv), optionally with the tool calls as captions and the "
    "pointer highlighted; with encode, stop_recording then encodes it to MP4 with the system ffmpeg as a job to wait "
    "for, and encode_recording encodes any recording later. An encode deletes the .mkv this session recorded once the "
    "MP4 is written, unless keep_raw_video_file.";

} // namespace

ServerInfo server_info()
{
    ServerInfo info;
    info.version      = SLIC3R_VERSION;
    info.instructions = kInstructions;
    return info;
}

}}} // namespace Slic3r::GUI::Mcp
