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

    py::enum_<FilamentSyncMode>(printer_agent_module, "FilamentSyncMode")
        .value("None_", FilamentSyncMode::none)
        .value("Subscription", FilamentSyncMode::subscription)
        .value("Pull", FilamentSyncMode::pull)
        .export_values();

    py::enum_<CameraStreamMode>(printer_agent_module, "CameraStreamMode")
        .value("None_", CameraStreamMode::none)
        .value("HTTP", CameraStreamMode::http)
        .value("HTTPS", CameraStreamMode::https)
        .value("HTTP_SNAPSHOT", CameraStreamMode::http_snapshot)
        .value("RTSP", CameraStreamMode::rtsp)
        .export_values();

    py::enum_<PrintStartMode>(printer_agent_module, "PrintStartMode")
        .value("Cloud", PrintStartMode::Cloud)
        .value("LocalWithRecord", PrintStartMode::LocalWithRecord)
        .value("Local", PrintStartMode::Local)
        .value("Sdcard", PrintStartMode::Sdcard);

    py::enum_<PrintMapping::DestinationKind>(printer_agent_module, "MappingDestinationKind")
        .value("Unmapped", PrintMapping::DestinationKind::unmapped)
        .value("ExternalSpool", PrintMapping::DestinationKind::external_spool)
        .value("AmsSlot", PrintMapping::DestinationKind::ams_slot);

    py::enum_<PrintMapping::MappingVersion>(printer_agent_module, "MappingVersion")
        .value("None_", PrintMapping::MappingVersion::none)
        .value("V0", PrintMapping::MappingVersion::v0)
        .value("V1", PrintMapping::MappingVersion::v1);

    py::class_<PrintMapping::Destination>(printer_agent_module, "MappingDestination")
        .def(py::init<>())
        .def_readwrite("kind", &PrintMapping::Destination::kind)
        .def_readwrite("ams_id", &PrintMapping::Destination::ams_id)
        .def_readwrite("slot_id", &PrintMapping::Destination::slot_id);

    py::class_<PrintMapping::FilamentMetadata>(printer_agent_module, "MappingFilamentMetadata")
        .def(py::init<>())
        .def_readwrite("ams", &PrintMapping::FilamentMetadata::ams)
        .def_readwrite("target_color", &PrintMapping::FilamentMetadata::target_color)
        .def_readwrite("filament_id", &PrintMapping::FilamentMetadata::filament_id)
        .def_readwrite("filament_type", &PrintMapping::FilamentMetadata::filament_type)
        .def_readwrite("nozzle_id", &PrintMapping::FilamentMetadata::nozzle_id)
        .def_readwrite("source_color", &PrintMapping::FilamentMetadata::source_color);

    py::class_<PrintMapping::Filament>(printer_agent_module, "MappingFilament")
        .def(py::init<>())
        .def_readwrite("filament_index", &PrintMapping::Filament::filament_index)
        .def_readwrite("destination", &PrintMapping::Filament::destination)
        .def_readwrite("metadata", &PrintMapping::Filament::metadata);

    py::class_<PrintMapping::NozzleMapping>(printer_agent_module, "NozzleMapping")
        .def(py::init<>())
        .def_readwrite("index", &PrintMapping::NozzleMapping::index)
        .def_readwrite("nozzle_id", &PrintMapping::NozzleMapping::nozzle_id);

    py::class_<PrintMapping::NozzleInfo>(printer_agent_module, "NozzleInfo")
        .def(py::init<>())
        .def_readwrite("id", &PrintMapping::NozzleInfo::id)
        .def_readwrite("type", &PrintMapping::NozzleInfo::type)
        .def_readwrite("flow_size", &PrintMapping::NozzleInfo::flow_size)
        .def_readwrite("diameter", &PrintMapping::NozzleInfo::diameter);

    py::class_<PrintMapping>(printer_agent_module, "PrintMapping")
        .def(py::init<>())
        .def_readwrite("use_ams", &PrintMapping::use_ams)
        .def_readwrite("ams_mapping_version", &PrintMapping::ams_mapping_version)
        .def_readwrite("filaments", &PrintMapping::filaments)
        .def_readwrite("nozzle_mapping", &PrintMapping::nozzle_mapping)
        .def_readwrite("nozzles", &PrintMapping::nozzles);

    py::class_<AgentInfo>(printer_agent_module, "AgentInfo")
        .def(py::init<>())
        .def(py::init([](std::string id, std::string name, std::string version, std::string description) {
                 return AgentInfo{std::move(id), std::move(name), std::move(version), std::move(description)};
             }),
             py::arg("id"), py::arg("name"), py::arg("version"), py::arg("description"))
        .def_readwrite("id", &AgentInfo::id)
        .def_readwrite("name", &AgentInfo::name)
        .def_readwrite("version", &AgentInfo::version)
        .def_readwrite("description", &AgentInfo::description);

    py::class_<detectResult>(printer_agent_module, "DetectResult")
        .def(py::init<>())
        .def_readwrite("result_msg", &detectResult::result_msg)
        .def_readwrite("command", &detectResult::command)
        .def_readwrite("dev_id", &detectResult::dev_id)
        .def_readwrite("model_id", &detectResult::model_id)
        .def_readwrite("dev_name", &detectResult::dev_name)
        .def_readwrite("version", &detectResult::version)
        .def_readwrite("bind_state", &detectResult::bind_state)
        .def_readwrite("connect_type", &detectResult::connect_type);

    py::class_<PrintParams>(printer_agent_module, "PrintParams")
        .def(py::init<>())
        .def_readwrite("dev_id", &PrintParams::dev_id)
        .def_readwrite("task_name", &PrintParams::task_name)
        .def_readwrite("project_name", &PrintParams::project_name)
        .def_readwrite("preset_name", &PrintParams::preset_name)
        .def_readwrite("filename", &PrintParams::filename)
        .def_readwrite("config_filename", &PrintParams::config_filename)
        .def_readwrite("plate_index", &PrintParams::plate_index)
        .def_readwrite("ftp_folder", &PrintParams::ftp_folder)
        .def_readwrite("ftp_file", &PrintParams::ftp_file)
        .def_readwrite("ftp_file_md5", &PrintParams::ftp_file_md5)
        .def_readwrite("nozzle_mapping", &PrintParams::nozzle_mapping)
        .def_readwrite("ams_mapping", &PrintParams::ams_mapping)
        .def_readwrite("ams_mapping2", &PrintParams::ams_mapping2)
        .def_readwrite("ams_mapping_info", &PrintParams::ams_mapping_info)
        .def_readwrite("nozzles_info", &PrintParams::nozzles_info)
        .def_readwrite("connection_type", &PrintParams::connection_type)
        .def_readwrite("comments", &PrintParams::comments)
        .def_readwrite("origin_profile_id", &PrintParams::origin_profile_id)
        .def_readwrite("stl_design_id", &PrintParams::stl_design_id)
        .def_readwrite("origin_model_id", &PrintParams::origin_model_id)
        .def_readwrite("print_type", &PrintParams::print_type)
        .def_readwrite("dst_file", &PrintParams::dst_file)
        .def_readwrite("dev_name", &PrintParams::dev_name)
        .def_readwrite("dev_ip", &PrintParams::dev_ip)
        .def_readwrite("use_ssl_for_ftp", &PrintParams::use_ssl_for_ftp)
        .def_readwrite("use_ssl_for_mqtt", &PrintParams::use_ssl_for_mqtt)
        .def_readwrite("username", &PrintParams::username)
        .def_readwrite("password", &PrintParams::password)
        .def_readwrite("task_bed_leveling", &PrintParams::task_bed_leveling)
        .def_readwrite("task_flow_cali", &PrintParams::task_flow_cali)
        .def_readwrite("task_vibration_cali", &PrintParams::task_vibration_cali)
        .def_readwrite("task_layer_inspect", &PrintParams::task_layer_inspect)
        .def_readwrite("task_record_timelapse", &PrintParams::task_record_timelapse)
        .def_readwrite("task_use_ams", &PrintParams::task_use_ams)
        .def_readwrite("task_bed_type", &PrintParams::task_bed_type)
        .def_readwrite("extra_options", &PrintParams::extra_options)
        .def_readwrite("auto_bed_leveling", &PrintParams::auto_bed_leveling)
        .def_readwrite("auto_flow_cali", &PrintParams::auto_flow_cali)
        .def_readwrite("auto_offset_cali", &PrintParams::auto_offset_cali)
        .def_readwrite("task_ext_change_assist", &PrintParams::task_ext_change_assist)
        .def_readwrite("try_emmc_print", &PrintParams::try_emmc_print);

    py::class_<PrinterConnectionParams>(printer_agent_module, "PrinterConnectionParams")
        .def(py::init<>())
        .def_readwrite("dev_id", &PrinterConnectionParams::dev_id)
        .def_readwrite("host", &PrinterConnectionParams::host)
        .def_readwrite("port", &PrinterConnectionParams::port)
        .def_readwrite("username", &PrinterConnectionParams::username)
        .def_readwrite("password", &PrinterConnectionParams::password)
        .def_readwrite("use_ssl", &PrinterConnectionParams::use_ssl)
        .def_readwrite("ca_file", &PrinterConnectionParams::ca_file);

    py::class_<PrinterAgentPluginCapability, PluginCapabilityInterface, PyPrinterAgentPluginCapabilityTrampoline, std::shared_ptr<PrinterAgentPluginCapability>>(
        printer_agent_module, "PrinterAgentBase")
        .def(py::init<>())
        .def("get_type", &PrinterAgentPluginCapability::get_type)
        .def("get_agent_info", &PrinterAgentPluginCapability::get_agent_info)
        .def("connect_printer", &PrinterAgentPluginCapability::connect_printer)
        .def("disconnect_printer", &PrinterAgentPluginCapability::disconnect_printer)
        .def("send_message", &PrinterAgentPluginCapability::send_message)
        .def("send_message_to_printer", &PrinterAgentPluginCapability::send_message_to_printer)
        .def("command_ams_refresh_rfid", &PrinterAgentPluginCapability::command_ams_refresh_rfid)
        .def("command_ams_calibrate", &PrinterAgentPluginCapability::command_ams_calibrate)
        .def("command_ams_select_tray", &PrinterAgentPluginCapability::command_ams_select_tray)
        .def("command_start_camera", &PrinterAgentPluginCapability::command_start_camera)
        .def("command_xyz_abs", &PrinterAgentPluginCapability::command_xyz_abs)
        .def("command_auto_leveling", &PrinterAgentPluginCapability::command_auto_leveling)
        .def("command_go_home", &PrinterAgentPluginCapability::command_go_home)
        .def("command_set_bed", &PrinterAgentPluginCapability::command_set_bed)
        .def("command_set_nozzle", &PrinterAgentPluginCapability::command_set_nozzle)
        .def("command_axis_control", &PrinterAgentPluginCapability::command_axis_control)
        .def("start_discovery", &PrinterAgentPluginCapability::start_discovery)
        .def("bind_detect", &PrinterAgentPluginCapability::bind_detect)
        .def("get_user_selected_machine", &PrinterAgentPluginCapability::get_user_selected_machine)
        .def("set_user_selected_machine", &PrinterAgentPluginCapability::set_user_selected_machine)
        .def("start_send_gcode_to_sdcard", &PrinterAgentPluginCapability::start_send_gcode_to_sdcard)
        .def("start_local_print", &PrinterAgentPluginCapability::start_local_print)
        .def("get_filament_sync_mode", &PrinterAgentPluginCapability::get_filament_sync_mode)
        .def("get_camera_stream_mode", &PrinterAgentPluginCapability::get_camera_stream_mode)
        .def("fetch_filament_info", &PrinterAgentPluginCapability::fetch_filament_info)
        .def("get_camera_url", &PrinterAgentPluginCapability::get_camera_url)
        .def("check_cert", &PrinterAgentPluginCapability::check_cert)
        .def("install_device_cert", &PrinterAgentPluginCapability::install_device_cert)
        .def("ping_bind", &PrinterAgentPluginCapability::ping_bind)
        .def("bind", &PrinterAgentPluginCapability::bind)
        .def("unbind", &PrinterAgentPluginCapability::unbind)
        .def("start_print", &PrinterAgentPluginCapability::start_print)
        .def("start_local_print_with_record", &PrinterAgentPluginCapability::start_local_print_with_record)
        .def("start_sdcard_print", &PrinterAgentPluginCapability::start_sdcard_print)
        .def("start_print_with_mapping", &IPrinterAgent::start_print_with_mapping);
}

} // namespace Slic3r
