#include "PrinterAgentPluginCapability.hpp"
#include "PrinterAgentPluginCapabilityTrampoline.hpp"

#include "IPrinterAgent.hpp"
#include <pybind11/cast.h>
#include "slic3r/Utils/bambu_networking.hpp"

#include <memory>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include <utility>

namespace py = pybind11;

namespace Slic3r {

void PrinterAgentPluginCapability::RegisterBindings(pybind11::module_& module)
{
    auto printer_agent_module = module.def_submodule("printer_agent", "Printer Agent API");

    py::enum_<FilamentSyncMode>(printer_agent_module, "FilamentSyncMode",
                                "How the host gets a printer's filament (AMS) data; returned by\n"
                                "PrinterAgentBase.get_filament_sync_mode().")
        .value("None_", FilamentSyncMode::none, "The agent provides no filament data.")
        .value("Subscription", FilamentSyncMode::subscription,
               "The printer's status messages carry the filament data as it changes.")
        .value("Pull", FilamentSyncMode::pull, "The host calls fetch_filament_info() whenever it needs the data.")
        .export_values();

    py::enum_<CameraStreamMode>(printer_agent_module, "CameraStreamMode",
                                "How the host shows a printer's camera; returned by\n"
                                "PrinterAgentBase.get_camera_stream_mode(). Every mode but None_ plays the URL from\n"
                                "get_camera_url().")
        .value("None_", CameraStreamMode::none, "The printer has no camera view.")
        .value("HTTP", CameraStreamMode::http, "An http:// video stream.")
        .value("HTTPS", CameraStreamMode::https, "An https:// video stream.")
        .value("HTTP_SNAPSHOT", CameraStreamMode::http_snapshot, "An HTTP endpoint that returns one image per request.")
        .value("RTSP", CameraStreamMode::rtsp, "An rtsp:// stream on the LAN.")
        .export_values();

    py::class_<AgentInfo>(printer_agent_module, "AgentInfo",
                          "Identity of a printer agent, returned by PrinterAgentBase.get_agent_info().")
        .def(py::init<>())
        .def(py::init([](std::string id, std::string name, std::string version, std::string description) {
                 return AgentInfo{std::move(id), std::move(name), std::move(version), std::move(description)};
             }),
             py::arg("id"), py::arg("name"), py::arg("version"), py::arg("description"),
             "An AgentInfo with these fields, each as its property describes; AgentInfo() leaves them all empty.")
        .def_readwrite("id", &AgentInfo::id,
                       "Agent id, e.g. \"moonraker\". Printer presets select their agent by this id, so it must be\n"
                       "non-empty and must not change; a capability whose id another agent already uses is disabled.")
        .def_readwrite("name", &AgentInfo::name, "Name shown to the user, e.g. \"Moonraker\".")
        .def_readwrite("version", &AgentInfo::version, "Agent version string, e.g. \"1.0.0\".")
        .def_readwrite("description", &AgentInfo::description, "Short description of the agent.");

    py::class_<detectResult>(printer_agent_module, "DetectResult",
                             "What PrinterAgentBase.bind_detect() reports about a printer found at an IP address.")
        .def(py::init<>())
        .def_readwrite("result_msg", &detectResult::result_msg, "Detail text. The host does not read it.")
        .def_readwrite("command", &detectResult::command, "The host does not read it.")
        .def_readwrite("dev_id", &detectResult::dev_id, "The printer's device id (serial number).")
        .def_readwrite("model_id", &detectResult::model_id,
                       "The printer's model id, stored as the device's printer type.")
        .def_readwrite("dev_name", &detectResult::dev_name, "The printer's display name.")
        .def_readwrite("version", &detectResult::version, "The printer's firmware version.")
        .def_readwrite("bind_state", &detectResult::bind_state,
                       "\"free\" when the printer can be added, \"occupied\" when it is bound to another account (the\n"
                       "host then refuses it).")
        .def_readwrite("connect_type", &detectResult::connect_type,
                       "\"lan\" for a printer in LAN-only mode, \"cloud\" for one in cloud mode (the host then asks\n"
                       "the user to switch it to LAN only).");

    py::class_<PrintParams>(printer_agent_module, "PrintParams",
                            "A print or send job, passed to the PrinterAgentBase start_* methods. A send job sets\n"
                            "fewer fields than a print job; the print options hold no meaningful value there.")
        .def(py::init<>())
        .def_readwrite("dev_id", &PrintParams::dev_id, "Device id of the target printer.")
        .def_readwrite("task_name", &PrintParams::task_name, "Task name. The host leaves it empty.")
        .def_readwrite("project_name", &PrintParams::project_name,
                       "Job name for the printer: the project or model name for a print, \"<project>.gcode.3mf\"\n"
                       "for a send, and \"verify_job\" for the connection check (see start_send_gcode_to_sdcard()).")
        .def_readwrite("preset_name", &PrintParams::preset_name,
                       "Name of the print profile for the job record, or \"<project>_plate_<n>\" when none is known.")
        .def_readwrite("filename", &PrintParams::filename,
                       "Local path of the file to send: a .3mf holding the sliced plate's G-code, or a small\n"
                       "temporary file for the connection check.")
        .def_readwrite("config_filename", &PrintParams::config_filename,
                       "Local path of a .3mf holding only the project's configuration.")
        .def_readwrite("plate_index", &PrintParams::plate_index,
                       "1-based number of the plate to print, whose G-code is inside filename.")
        .def_readwrite("ftp_folder", &PrintParams::ftp_folder,
                       "Folder on the printer's storage to upload into, from the printer model's profile; empty when\n"
                       "the model defines none.")
        .def_readwrite("ftp_file", &PrintParams::ftp_file, "The host leaves it empty.")
        .def_readwrite("ftp_file_md5", &PrintParams::ftp_file_md5, "The host leaves it empty.")
        .def_readwrite("nozzle_mapping", &PrintParams::nozzle_mapping,
                       "JSON nozzle mapping for printers with several nozzles; empty otherwise.")
        .def_readwrite("ams_mapping", &PrintParams::ams_mapping,
                       "JSON array with one entry per project filament: the printer tray index it prints from, or -1\n"
                       "when it has none or uses an external spool.")
        .def_readwrite("ams_mapping2", &PrintParams::ams_mapping2,
                       "JSON array with one {\"ams_id\": int, \"slot_id\": int} object per project filament; both are\n"
                       "255 when the filament is not mapped.")
        .def_readwrite("ams_mapping_info", &PrintParams::ams_mapping_info,
                       "JSON array with one object per project filament, holding ams (tray index or -1),\n"
                       "sourceColor, targetColor, filamentId, filamentType and nozzleId.")
        .def_readwrite("nozzles_info", &PrintParams::nozzles_info,
                       "JSON array describing the printer's nozzles; filled only for two-nozzle printers.")
        .def_readwrite("connection_type", &PrintParams::connection_type,
                       "\"lan\" for a printer in LAN-only mode, \"cloud\" otherwise.")
        .def_readwrite("comments", &PrintParams::comments,
                       "Note the host sets on the cloud print path saying why a LAN send was not used or failed,\n"
                       "e.g. \"no_ip\", \"low_version\", \"no_sdcard\", \"no_password\", \"upload_failed\" or\n"
                       "\"failed(<code>)\".")
        .def_readwrite("origin_profile_id", &PrintParams::origin_profile_id,
                       "Designer profile id from the project's 3MF metadata, or 0.")
        .def_readwrite("stl_design_id", &PrintParams::stl_design_id,
                       "Online model library design id of the project's model, or 0.")
        .def_readwrite("origin_model_id", &PrintParams::origin_model_id,
                       "Designer model id from the project's 3MF metadata, or empty.")
        .def_readwrite("print_type", &PrintParams::print_type,
                       "\"from_normal\" for a sliced project, \"from_sdcard_view\" for a file already on the\n"
                       "printer's storage.")
        .def_readwrite("dst_file", &PrintParams::dst_file,
                       "Path of the file on the printer's storage when print_type is \"from_sdcard_view\".")
        .def_readwrite("dev_name", &PrintParams::dev_name,
                       "Printer display name. Set only for jobs sent to several printers at once.")
        .def_readwrite("dev_ip", &PrintParams::dev_ip, "IP address of the printer.")
        .def_readwrite("use_ssl_for_ftp", &PrintParams::use_ssl_for_ftp, "Whether the file upload should use TLS.")
        .def_readwrite("use_ssl_for_mqtt", &PrintParams::use_ssl_for_mqtt,
                       "Whether the printer's LAN message connection should use TLS.")
        .def_readwrite("username", &PrintParams::username,
                       "LAN user name. The host takes it from the agent's default LAN user name, which is empty for a\n"
                       "plugin agent.")
        .def_readwrite("password", &PrintParams::password, "The access code stored for the printer.")
        .def_readwrite("task_bed_leveling", &PrintParams::task_bed_leveling, "Level the bed before printing.")
        .def_readwrite("task_flow_cali", &PrintParams::task_flow_cali, "Run flow dynamics calibration before printing.")
        .def_readwrite("task_vibration_cali", &PrintParams::task_vibration_cali,
                       "Run vibration compensation calibration before printing.")
        .def_readwrite("task_layer_inspect", &PrintParams::task_layer_inspect, "Inspect the first layer.")
        .def_readwrite("task_record_timelapse", &PrintParams::task_record_timelapse, "Record a timelapse.")
        .def_readwrite("task_use_ams", &PrintParams::task_use_ams,
                       "Feed filament from the AMS rather than an external spool.")
        .def_readwrite("task_bed_type", &PrintParams::task_bed_type,
                       "Plate type: \"cool_plate\", \"textured_cool_plate\", \"eng_plate\", \"hot_plate\",\n"
                       "\"textured_plate\", \"supertack_plate\" or \"unknown\".")
        .def_readwrite("extra_options", &PrintParams::extra_options, "The host leaves it empty.")
        .def_readwrite("auto_bed_leveling", &PrintParams::auto_bed_leveling,
                       "Bed leveling choice: 0 off, 1 on, 2 auto.")
        .def_readwrite("auto_flow_cali", &PrintParams::auto_flow_cali,
                       "Flow dynamics calibration choice: 0 off, 1 on, 2 auto.")
        .def_readwrite("auto_offset_cali", &PrintParams::auto_offset_cali,
                       "Nozzle offset calibration choice: 0 off, 1 on, 2 auto.")
        .def_readwrite("task_ext_change_assist", &PrintParams::task_ext_change_assist,
                       "The user changes the external spool by hand for a multi-color print.")
        .def_readwrite("try_emmc_print", &PrintParams::try_emmc_print,
                       "Send the job to the printer's internal (eMMC) storage rather than its SD card.");

    py::class_<PrinterConnectionParams>(printer_agent_module, "PrinterConnectionParams",
                                        "Where and how to reach a LAN printer; passed to\n"
                                        "PrinterAgentBase.connect_printer().")
        .def(py::init<>())
        .def_readwrite("dev_id", &PrinterConnectionParams::dev_id, "The printer's device id.")
        .def_readwrite("host", &PrinterConnectionParams::host,
                       "Host name or IP address, without the http:// or https:// prefix.")
        .def_readwrite("port", &PrinterConnectionParams::port,
                       "Port as a string, from the printer address or the preset's printhost_port; empty when unset.")
        .def_readwrite("username", &PrinterConnectionParams::username,
                       "LAN user name: the agent's default LAN user name, which is empty for a plugin agent.")
        .def_readwrite("password", &PrinterConnectionParams::password,
                       "The access code or API key stored for the printer.")
        .def_readwrite("use_ssl", &PrinterConnectionParams::use_ssl, "True when the printer address starts with https.")
        .def_readwrite("ca_file", &PrinterConnectionParams::ca_file,
                       "Path of a CA certificate file from the preset's printhost_cafile, or empty.");

    py::class_<PrinterAgentPluginCapability, PluginCapabilityInterface, PyPrinterAgentPluginCapabilityTrampoline, std::shared_ptr<PrinterAgentPluginCapability>>(
        printer_agent_module, "PrinterAgentBase",
        "Base for a printer agent capability: the protocol OrcaSlicer uses to find printers, connect to them,\n"
        "send them commands and start print jobs. A printer preset picks its agent by get_agent_info().id.\n"
        "\n"
        "Methods marked Required must be overridden: without one the host logs an error and gets the failure\n"
        "value. Methods returning an int return 0 on success and a negative error code on failure. A command\n"
        "the agent cannot translate returns -7010, one this printer lacks returns -7020, and the host shows a\n"
        "dialog for both. An exception raised by any method is logged and the host gets -1, or False, \"\" or\n"
        "the default value for other return types. In the command_* methods, sequence_id is the command id the\n"
        "host tracks and lan_mode is True for a printer in LAN-only mode.\n"
        "\n"
        "The agent reports back through callables the host hands over with these methods, which the plugin\n"
        "also implements by name, storing the callable (None clears it) and returning 0:\n"
        "set_on_local_connect_fn(fn) with fn(status, dev_id, msg), status 0 connected, 1 failed, 2 lost;\n"
        "set_on_local_message_fn(fn) and set_on_message_fn(fn) with fn(dev_id, json_msg) for messages from\n"
        "LAN and cloud printers; set_on_user_message_fn(fn) with fn(dev_id, json_msg);\n"
        "set_on_printer_connected_fn(fn) with fn(dev_id); set_on_subscribe_failure_fn(fn) with fn(dev_id);\n"
        "set_on_ssdp_msg_fn(fn) with fn(device_json) for discovered printers (see start_discovery());\n"
        "set_queue_on_main_fn(fn) with fn(callable), which runs callable on the UI thread; and\n"
        "set_server_callback(fn) with fn(url, http_status).")
        .def(py::init<>())
        .def("get_type", &PrinterAgentPluginCapability::get_type, "Return PluginType.PrinterConnection.")
        .def("get_agent_info", &PrinterAgentPluginCapability::get_agent_info,
             "Required. Return the AgentInfo identifying this agent. The host reads it when it registers the\n"
             "capability and whenever it checks which agent is active, so return the same values every time.")
        .def("connect_printer", &PrinterAgentPluginCapability::connect_printer, py::arg("params"),
             "Required. Called to connect to a LAN printer described by params, a PrinterConnectionParams. Start\n"
             "connecting, in the background if it takes time, and report the outcome through the\n"
             "set_on_local_connect_fn() callable. Return 0 when the attempt started, a negative code otherwise.")
        .def("disconnect_printer", &PrinterAgentPluginCapability::disconnect_printer,
             "Required. Close the connection connect_printer() opened. Called when the user switches printers or\n"
             "agents and when OrcaSlicer closes, which waits at most 3 seconds for it. Return 0 on success.")
        .def("send_message", &PrinterAgentPluginCapability::send_message, py::arg("dev_id"), py::arg("json_str"),
             py::arg("qos"), py::arg("flag"),
             "Required. Send a command to a printer that is not in LAN-only mode, through the cloud.\n"
             "json_str is a Bambu-style JSON command such as\n"
             "{\"print\": {\"command\": \"gcode_line\", \"param\": \"G28\\n\", \"sequence_id\": \"1\"}};\n"
             "qos is the MQTT QoS level, 0 or 1; the host always passes flag=0. Return 0 when sent.")
        .def("send_message_to_printer", &PrinterAgentPluginCapability::send_message_to_printer, py::arg("dev_id"),
             py::arg("json_str"), py::arg("qos"), py::arg("flag"),
             "Required. Send a command straight to a printer in LAN-only mode. The arguments and return value\n"
             "are those of send_message().")
        .def("command_ams_refresh_rfid", &PrinterAgentPluginCapability::command_ams_refresh_rfid, py::arg("dev_id"),
             py::arg("ams_id"), py::arg("slot_id"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Ask the printer to re-read the RFID tag of slot slot_id in AMS unit ams_id. When ams_id is -1,\n"
             "slot_id is a tray index counted across all units (unit * 4 + slot). The default returns -7010.")
        .def("command_ams_calibrate", &PrinterAgentPluginCapability::command_ams_calibrate, py::arg("dev_id"),
             py::arg("ams_id"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Ask the printer to calibrate AMS unit ams_id. The default returns -7010.")
        .def("command_ams_select_tray", &PrinterAgentPluginCapability::command_ams_select_tray, py::arg("dev_id"),
             py::arg("tray_id"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Ask the printer to load filament from a tray; tray_id is the tray index counted across all AMS units\n"
             "(unit * 4 + slot), as a string. The default returns -7010.")
        .def("command_start_camera", &PrinterAgentPluginCapability::command_start_camera, py::arg("dev_id"),
             "Ask the printer to start its camera. The default returns -7010.")
        .def("command_xyz_abs", &PrinterAgentPluginCapability::command_xyz_abs, py::arg("dev_id"),
             py::arg("sequence_id"), py::arg("lan_mode"),
             "Switch the printer to absolute positioning. The default sends G90 as a gcode_line command through\n"
             "send_message_to_printer() when lan_mode is True, send_message() otherwise.")
        .def("command_auto_leveling", &PrinterAgentPluginCapability::command_auto_leveling, py::arg("dev_id"),
             py::arg("sequence_id"), py::arg("lan_mode"),
             "Level the bed. The default sends G29 as a gcode_line command, as command_xyz_abs() does.")
        .def("command_go_home", &PrinterAgentPluginCapability::command_go_home, py::arg("dev_id"),
             py::arg("is_printing"), py::arg("supports_mqtt_homing"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Home the axes. is_printing is True while the printer prints. The default sends the\n"
             "back_to_center command when supports_mqtt_homing is True, otherwise G28 (G28 X while printing).")
        .def("command_set_bed", &PrinterAgentPluginCapability::command_set_bed, py::arg("dev_id"), py::arg("temp"),
             py::arg("supports_mqtt_bed_ctrl"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Set the bed temperature to temp, in degrees Celsius. The default sends the set_bed_temp command when\n"
             "supports_mqtt_bed_ctrl is True, otherwise M140.")
        .def("command_set_nozzle", &PrinterAgentPluginCapability::command_set_nozzle, py::arg("dev_id"),
             py::arg("temp"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Set the nozzle temperature to temp, in degrees Celsius. The default sends M104.")
        .def("command_axis_control", &PrinterAgentPluginCapability::command_axis_control, py::arg("dev_id"),
             py::arg("axis"), py::arg("unit"), py::arg("input_val"), py::arg("speed"), py::arg("is_core_xy"),
             py::arg("supports_mqtt_axis_control"), py::arg("sequence_id"), py::arg("lan_mode"),
             "Jog one axis. axis is \"X\", \"Y\", \"Z\" or \"E\"; the move is input_val * unit millimeters,\n"
             "negative to go back; speed is the feed rate in mm/min; is_core_xy says whether the printer is\n"
             "CoreXY. The default sends a relative G1 move (G0 after M83 for E), inverting Y and Z moves when\n"
             "is_core_xy is False, and returns -1 for any other axis.")
        .def("start_discovery", &PrinterAgentPluginCapability::start_discovery, py::arg("start"), py::arg("sending"),
             "Required. Called with start=True when OrcaSlicer starts looking for printers and start=False when\n"
             "it stops; the host always passes sending=False. Report each printer found by calling the\n"
             "set_on_ssdp_msg_fn() callable with a JSON string holding dev_id, dev_name, dev_ip, dev_type,\n"
             "dev_signal, connect_type and bind_state, plus optional sec_link, ssdp_version and connection_name.\n"
             "Return True on success.")
        .def("bind_detect", &PrinterAgentPluginCapability::bind_detect, py::arg("dev_ip"), py::arg("sec_link"),
             py::arg("detect"),
             "Required. Called when the user adds a printer by IP address dev_ip; the host passes\n"
             "sec_link=\"secure\". Set the fields of detect, a DetectResult the host reads afterwards, in place.\n"
             "Return 0 on success; on failure -1 tells the user the connection failed and -2 that the login\n"
             "request could not be sent.")
        .def("get_user_selected_machine", &PrinterAgentPluginCapability::get_user_selected_machine,
             "Required. Return the device id last stored by set_user_selected_machine(), or \"\".")
        .def("set_user_selected_machine", &PrinterAgentPluginCapability::set_user_selected_machine, py::arg("dev_id"),
             "Required. Called with the device id of the printer the user selects, or \"\" to clear the\n"
             "selection. Store it for get_user_selected_machine() and return 0.")
        .def("start_send_gcode_to_sdcard", &PrinterAgentPluginCapability::start_send_gcode_to_sdcard,
             py::arg("params"), py::arg("update_fn"), py::arg("cancel_fn"), py::arg("wait_fn"),
             "Required. Upload params.filename to the printer's storage without starting it. Before a LAN\n"
             "print or send, the host also calls it as a connection check, with project_name \"verify_job\", a\n"
             "small temporary file and None for all three callables; return 0 when that upload works. See\n"
             "start_print() for the callables and the return value.")
        .def("start_local_print", &PrinterAgentPluginCapability::start_local_print, py::arg("params"),
             py::arg("update_fn"), py::arg("cancel_fn"),
             "Required. Upload the job to a printer in LAN-only mode and start it. params is a PrintParams; see\n"
             "start_print() for the callables and the return value.")
        .def("get_filament_sync_mode", &PrinterAgentPluginCapability::get_filament_sync_mode,
             "Return the FilamentSyncMode saying how the host gets the printer's filament data. The default is\n"
             "FilamentSyncMode.None_.")
        .def("get_camera_stream_mode", &PrinterAgentPluginCapability::get_camera_stream_mode,
             "Return the CameraStreamMode saying how the host shows the printer's camera. The default is\n"
             "CameraStreamMode.None_.")
        .def("fetch_filament_info", &PrinterAgentPluginCapability::fetch_filament_info, py::arg("dev_id"),
             py::arg("sync_mode"),
             "Called when get_filament_sync_mode() is Pull, before the host reads the printer's filament slots;\n"
             "sync_mode is FilamentSyncMode.Pull. Refresh the data and return True, or False when none is\n"
             "available. The default returns False.")
        .def("get_camera_url", &PrinterAgentPluginCapability::get_camera_url,
             "Return the camera URL of the connected printer, played as get_camera_stream_mode() says. The\n"
             "default returns \"\".")
        .def("check_cert", &PrinterAgentPluginCapability::check_cert,
             "Return 0 when the user's certificate for the printer is valid. The host does not call it.")
        .def("install_device_cert", &PrinterAgentPluginCapability::install_device_cert, py::arg("dev_id"),
             py::arg("lan_only"),
             "Called for the selected printer after it connects and again periodically, to install or refresh\n"
             "its certificate for TLS; lan_only is True for a printer in LAN-only mode. Returns nothing. The\n"
             "default does nothing.")
        .def("ping_bind", &PrinterAgentPluginCapability::ping_bind, py::arg("ping_code"),
             "Required. Called with the PIN code the user typed to bind a printer to the signed-in account.\n"
             "Return 0 when it is accepted, a negative value otherwise.")
        .def("bind", &PrinterAgentPluginCapability::bind, py::arg("dev_ip"), py::arg("dev_id"), py::arg("dev_model"),
             py::arg("sec_link"), py::arg("timezone"), py::arg("improved"), py::arg("update_fn"),
             "Required. Bind the printer to the signed-in account. dev_model is the model name shown to the\n"
             "user, sec_link the value the printer announced at discovery, timezone the local offset such as\n"
             "\"UTC+08:00\", and improved whether the user joined the user experience improvement program.\n"
             "Report progress by calling update_fn(stage, code, msg), stage 0 to 5 from connecting to\n"
             "finished, code 0 or a negative error code. Return 0 on success.")
        .def("unbind", &PrinterAgentPluginCapability::unbind, py::arg("dev_id"),
             "Required. Remove the printer from the signed-in account. Return 0 on success.")
        .def("start_print", &PrinterAgentPluginCapability::start_print, py::arg("params"), py::arg("update_fn"),
             py::arg("cancel_fn"), py::arg("wait_fn"),
             "Required. Start a print through the cloud, for a printer that is not in LAN-only mode. params is a\n"
             "PrintParams. Report progress by calling update_fn(stage, code, msg): stage 0 create, 1 upload,\n"
             "2 waiting, 3 sending, 4 record, 5 wait for printer, 6 finished, 7 error; code is a percentage\n"
             "during upload and record, or a negative error code. cancel_fn() returns True once the user\n"
             "cancelled; return -18 then. wait_fn(stage, job_info_json) waits until the printer has taken the\n"
             "job and returns True. Any callable may be None. Return 0 on success or a negative error code,\n"
             "e.g. -14 file not found, -4020 LAN upload failed, -5010 upload to storage failed.")
        .def("start_local_print_with_record", &PrinterAgentPluginCapability::start_local_print_with_record,
             py::arg("params"), py::arg("update_fn"), py::arg("cancel_fn"), py::arg("wait_fn"),
             "Required. Upload the job over the LAN to a printer that is not in LAN-only mode and start it, also\n"
             "recording it with the cloud. After a negative result the host normally falls back to start_print().\n"
             "See start_print() for the arguments and the return value.")
        .def("start_sdcard_print", &PrinterAgentPluginCapability::start_sdcard_print, py::arg("params"),
             py::arg("update_fn"), py::arg("cancel_fn"),
             "Required. Print a file already on the printer's storage, named by params.dst_file. See\n"
             "start_print() for the callables and the return value.");
}

} // namespace Slic3r
