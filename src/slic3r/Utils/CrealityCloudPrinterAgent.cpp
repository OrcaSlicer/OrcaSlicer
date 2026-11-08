#include "CrealityCloudPrinterAgent.hpp"

#include "ICloudServiceAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

using nlohmann::json;
namespace CC = CrealityCloud;

namespace {

constexpr const char* AGENT_VERSION = "1.0.0";
constexpr auto        DISCOVERY_INTERVAL = std::chrono::seconds(15);
constexpr auto        STATUS_INTERVAL    = std::chrono::seconds(5);

// Sleeps in short steps so a stop request does not wait out the interval.
template<class Stop> void wait_for(std::chrono::milliseconds total, Stop stop)
{
    const auto step = std::chrono::milliseconds(100);
    for (auto waited = std::chrono::milliseconds(0); waited < total && !stop(); waited += step)
        std::this_thread::sleep_for(step);
}

// The task name a print is listed under in Creality Cloud and on the printer.
std::string task_name(const PrintParams& params, const std::string& gcode_path)
{
    std::string name = !params.task_name.empty() ? params.task_name :
                       !params.project_name.empty() ? params.project_name :
                                                      boost::filesystem::path(gcode_path).stem().string();
    for (const char* extension : {".3mf", ".gcode"})
        if (boost::iends_with(name, extension))
            name.erase(name.size() - std::strlen(extension));
    return name.empty() ? "print" : name;
}

} // namespace

CrealityCloudPrinterAgent::CrealityCloudPrinterAgent(std::string log_dir) : m_log_dir(std::move(log_dir)) {}

CrealityCloudPrinterAgent::~CrealityCloudPrinterAgent()
{
    stop_discovery();
    stop_status();
}

AgentInfo CrealityCloudPrinterAgent::get_agent_info_static()
{
    return AgentInfo{AGENT_ID, "Creality Cloud", AGENT_VERSION, "Print to Creality printers through a Creality Cloud account"};
}

void CrealityCloudPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_cloud_agent = std::move(cloud);
}

// --- Callbacks ----------------------------------------------------------------------------------

void CrealityCloudPrinterAgent::post(std::function<void()> fn)
{
    QueueOnMainFn queue;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        queue = m_queue_on_main;
    }
    if (queue)
        queue(std::move(fn));
    else
        fn();
}

void CrealityCloudPrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn local, cloud;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        local = m_on_local_message;
        cloud = m_on_message;
    }
    if (!local && !cloud)
        return;
    post([dev_id, payload, local, cloud]() {
        if (local)
            local(dev_id, payload);
        else
            cloud(dev_id, payload);
    });
}

int CrealityCloudPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_ssdp = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_printer_connected = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_subscribe_failure = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_user_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_local_connect = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_local_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_queue_on_main = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}
int CrealityCloudPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_server_err = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

std::string CrealityCloudPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_selected;
}

int CrealityCloudPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_selected = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// --- Discovery ----------------------------------------------------------------------------------

bool CrealityCloudPrinterAgent::start_discovery(bool start, bool /*sending*/)
{
    if (!start) {
        stop_discovery();
        return true;
    }
    if (!m_discovery_stop.exchange(false))
        return true; // Already running
    if (m_discovery_thread.joinable())
        m_discovery_thread.join();
    m_discovery_thread = std::thread([this] { discovery_loop(); });
    return true;
}

void CrealityCloudPrinterAgent::stop_discovery()
{
    m_discovery_stop.store(true);
    if (m_discovery_thread.joinable() && m_discovery_thread.get_id() != std::this_thread::get_id())
        m_discovery_thread.join();
}

void CrealityCloudPrinterAgent::discovery_loop()
{
    while (!m_discovery_stop.load()) {
        const CC::Session session = CC::SessionStore::get();
        if (session.valid()) {
            std::vector<CC::Device> devices;
            std::string             error;
            const int               code = CC::Client(session).list_devices(devices, error);
            if (code == CC::CODE_OK) {
                OnMsgArrivedFn ssdp;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    m_devices.clear();
                    for (const CC::Device& d : devices)
                        m_devices[d.dn] = d;
                    ssdp = m_on_ssdp;
                }
                if (ssdp)
                    for (const CC::Device& d : devices) {
                        const std::string payload = CC::ssdp_payload(d).dump();
                        post([ssdp, payload] { ssdp(payload); });
                    }
            } else {
                BOOST_LOG_TRIVIAL(warning) << "CrealityCloud: listing printers failed (" << code << "): " << error;
            }
        }
        wait_for(DISCOVERY_INTERVAL, [this] { return m_discovery_stop.load(); });
    }
}

bool CrealityCloudPrinterAgent::find_device(const std::string& dn, CC::Device& device)
{
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_devices.find(dn);
        if (it != m_devices.end()) {
            device = it->second;
            return true;
        }
    }
    std::vector<CC::Device> devices;
    std::string             error;
    if (CC::Client(CC::SessionStore::get()).list_devices(devices, error) != CC::CODE_OK)
        return false;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const CC::Device& d : devices)
        m_devices[d.dn] = d;
    auto it = m_devices.find(dn);
    if (it == m_devices.end())
        return false;
    device = it->second;
    return true;
}

// --- Connection and status ----------------------------------------------------------------------

int CrealityCloudPrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    if (params.dev_id.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    stop_status();
    set_user_selected_machine(params.dev_id);
    const uint64_t generation = ++m_status_generation;
    m_status_thread           = std::thread([this, dev_id = params.dev_id, generation] { status_loop(dev_id, generation); });
    return BAMBU_NETWORK_SUCCESS;
}

int CrealityCloudPrinterAgent::disconnect_printer()
{
    stop_status();
    return BAMBU_NETWORK_SUCCESS;
}

void CrealityCloudPrinterAgent::stop_status()
{
    ++m_status_generation;
    if (m_status_thread.joinable() && m_status_thread.get_id() != std::this_thread::get_id())
        m_status_thread.join();
}

void CrealityCloudPrinterAgent::status_loop(std::string dev_id, uint64_t generation)
{
    auto stale     = [this, generation] { return generation != m_status_generation.load(); };
    bool connected = false;
    while (!stale()) {
        const CC::Session session = CC::SessionStore::get();
        CC::Device        device;
        // Refresh the device record each round, since it carries the online flag and the job's progress.
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_devices.erase(dev_id);
        }
        if (session.valid() && find_device(dev_id, device)) {
            json        telemetry = json::object();
            std::string error;
            if (device.online && !device.tb_id.empty())
                CC::Client(session).telemetry(device.tb_id, telemetry, error);
            if (stale())
                break;
            if (!connected) {
                connected = true;
                OnLocalConnectedFn   local;
                OnPrinterConnectedFn printer;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    local   = m_on_local_connect;
                    printer = m_on_printer_connected;
                }
                if (local)
                    post([local, dev_id] { local(ConnectStatusOk, dev_id, "0"); });
                if (printer)
                    post([printer, dev_id] { printer(dev_id); });
            }
            dispatch_message(dev_id, CC::status_payload(device, telemetry).dump());
        }
        wait_for(STATUS_INTERVAL, stale);
    }
}

int CrealityCloudPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

int CrealityCloudPrinterAgent::control(const std::string& dev_id, const json& params)
{
    CC::Device device;
    if (!find_device(dev_id, device) || device.tb_id.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    std::string error;
    const int   code = CC::Client(CC::SessionStore::get()).rpc(device.tb_id, params, true, error);
    if (code != CC::CODE_OK)
        BOOST_LOG_TRIVIAL(warning) << "CrealityCloud: command failed (" << code << "): " << error;
    return code == CC::CODE_OK ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

int CrealityCloudPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{
    const json request = json::parse(json_str, nullptr, false);
    if (!request.is_object())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    if (request.contains("info") && request["info"].value("command", "") == "get_version") {
        json reply;
        reply["info"]["command"] = "get_version";
        reply["info"]["result"]  = "success";
        reply["info"]["module"]  = json::array({json{{"name", "ota"}, {"sw_ver", ""}, {"product_name", "Creality"}}});
        dispatch_message(dev_id, reply.dump());
        return BAMBU_NETWORK_SUCCESS;
    }
    if (!request.contains("print") || !request["print"].is_object())
        return BAMBU_NETWORK_SUCCESS;

    const json&       print   = request["print"];
    const std::string command = print.value("command", "");
    if (command == "pause")
        return control(dev_id, {{"pause", 1}});
    if (command == "resume")
        return control(dev_id, {{"pause", 0}});
    if (command == "stop")
        return control(dev_id, {{"stop", 1}});
    if (command == "gcode_line" && print.contains("param") && print["param"].is_string())
        return control(dev_id, {{"gcodeCmd", print["param"].get<std::string>()}});
    if (command == "set_bed_temp" && print.contains("temp") && print["temp"].is_number())
        return control(dev_id, {{"bedTempControl", {{"num", 0}, {"val", print["temp"].get<int>()}}}});
    if (command == "set_nozzle_temp" && print.contains("target_temp") && print["target_temp"].is_number())
        return control(dev_id, {{"nozzleTempControl", print["target_temp"].get<int>()}});
    return BAMBU_NETWORK_SUCCESS;
}

// --- Printing -----------------------------------------------------------------------------------

int CrealityCloudPrinterAgent::cloud_print(const PrintParams& params, bool start, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    auto update = [&](int stage, int code, const std::string& msg) {
        if (update_fn)
            update_fn(stage, code, msg);
    };
    update(PrintingStageCreate, 0, _u8L("Preparing..."));

    const CC::Session session = CC::SessionStore::get();
    if (!session.valid()) {
        update(PrintingStageERROR, 0, _u8L("Sign in to Creality Cloud first."));
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // params.filename may be the .gcode.3mf bundle; dst_file, when set, is the plate's plain G-code.
    std::string gcode_path = !params.dst_file.empty() ? params.dst_file : params.filename;
    boost::filesystem::path extracted;
    if (boost::iends_with(gcode_path, ".3mf")) {
        boost::system::error_code ec;
        extracted = boost::filesystem::temp_directory_path(ec) / boost::filesystem::unique_path("creality-plate-%%%%-%%%%.gcode", ec);
        if (ec || !MoonrakerPrinterAgent::extract_plate_gcode(gcode_path, params.plate_index, extracted)) {
            BOOST_LOG_TRIVIAL(error) << "CrealityCloud: no G-code for plate " << params.plate_index << " in " << gcode_path;
            return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
        }
        gcode_path = extracted.string();
    }
    struct Cleanup
    {
        boost::filesystem::path path;
        ~Cleanup()
        {
            boost::system::error_code ec;
            if (!path.empty())
                boost::filesystem::remove(path, ec);
        }
    } cleanup{extracted};

    if (!boost::filesystem::exists(gcode_path))
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;

    CC::Device device;
    if (start && !find_device(params.dev_id, device)) {
        update(PrintingStageERROR, 0, _u8L("The printer is not on this Creality Cloud account."));
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    const std::string name = task_name(params, gcode_path);
    CC::Client        client(session);
    CC::UploadedGcode uploaded;
    std::string       error;
    update(PrintingStageUpload, 0, _u8L("Uploading to Creality Cloud..."));
    int code = client.upload_gcode(
        gcode_path, name, [&](int percent) { update(PrintingStageUpload, percent, _u8L("Uploading to Creality Cloud...")); },
        [&] { return cancel_fn && cancel_fn(); }, uploaded, error);
    if (code == CC::CODE_CANCELLED)
        return BAMBU_NETWORK_ERR_CANCELED;
    if (code != CC::CODE_OK) {
        BOOST_LOG_TRIVIAL(error) << "CrealityCloud: upload failed (" << code << "): " << error;
        update(PrintingStageERROR, code, code == CC::CODE_TOKEN_INVALID ? _u8L("Creality Cloud session expired. Sign in again.") : error);
        return BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED;
    }
    if (!start) {
        update(PrintingStageFinished, 100, _u8L("Uploaded to your Creality Cloud files"));
        return BAMBU_NETWORK_SUCCESS;
    }
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    update(PrintingStageSending, 0, _u8L("Starting print..."));
    if (!device.tb_id.empty()) {
        // Creality Print sets the self-test flag right before the task; a failure here should not block the print.
        std::string ignored;
        client.set_self_test(device.tb_id, params.task_bed_leveling, ignored);
    }

    std::vector<std::string> types, colors;
    if (const PresetBundle* bundle = GUI::wxGetApp().preset_bundle) {
        if (auto* opt = bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
            colors = opt->values;
        if (auto* opt = bundle->full_config().option<ConfigOptionStrings>("filament_type"))
            types = opt->values;
    }
    const std::vector<CC::FilamentSlot> slots   = CC::filament_slots(params.ams_mapping, types, colors);
    const bool                          use_cfs = device.multi_color && params.task_use_ams && !slots.empty();

    code = client.add_print_task(device.dn, uploaded, name, slots, device.multi_color, use_cfs, error);
    if (code != CC::CODE_OK) {
        BOOST_LOG_TRIVIAL(error) << "CrealityCloud: starting the print failed (" << code << "): " << error;
        update(PrintingStageERROR, code, error);
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }
    update(PrintingStageFinished, 100, _u8L("Print started"));
    return BAMBU_NETWORK_SUCCESS;
}

int CrealityCloudPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return cloud_print(params, true, std::move(update_fn), std::move(cancel_fn));
}

int CrealityCloudPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return cloud_print(params, true, std::move(update_fn), std::move(cancel_fn));
}

int CrealityCloudPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return cloud_print(params, true, std::move(update_fn), std::move(cancel_fn));
}

int CrealityCloudPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    // "Send" without printing keeps the file in the account's cloud G-code library, where the printer can start it later.
    return cloud_print(params, false, std::move(update_fn), std::move(cancel_fn));
}

int CrealityCloudPrinterAgent::start_sdcard_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }

} // namespace Slic3r
