#include "KobraXPrinterAgent.hpp"

#include "IPrinterAgent.hpp"
#include "KobraX.hpp"
#include "KobraXSession.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <wx/app.h>

using json = nlohmann::json;
namespace fs = boost::filesystem;

namespace Slic3r {

namespace {

constexpr const char* VERSION       = "0.1.0";
constexpr const char* PRODUCT       = "Anycubic Kobra X";
const auto            POLL_EVERY    = std::chrono::seconds(5);
const auto            RETRY_FIRST   = std::chrono::seconds(2);
const auto            RETRY_MAX     = std::chrono::seconds(60);
const auto            DISPATCH_GAP  = std::chrono::milliseconds(500);
const auto            START_TIMEOUT = std::chrono::seconds(15);
const auto            START_WATCH   = std::chrono::seconds(5);

std::string text(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string command_of(const json& message, const char* section)
{
    const auto it = message.find(section);
    if (it == message.end() || !it->is_object() || !it->contains("command") || !(*it)["command"].is_string())
        return {};
    return (*it)["command"].get<std::string>();
}

} // namespace

struct KobraXPrinterAgent::Connection
{
    std::string dev_id;
    std::string host;
    std::string access_code;
    int         http_port = KobraX::HTTP_PORT;

    std::mutex                       mutex;
    std::condition_variable          wake;
    bool                             stop    = false;
    bool                             dropped = false;
    std::shared_ptr<KobraX::Session> session;
    std::chrono::steady_clock::time_point last_dispatch;
    std::thread                      thread;

    // Waits until `delay` passed or the connection is stopped or dropped; false once stopped
    bool wait(std::chrono::steady_clock::duration delay)
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, delay, [this] { return stop || dropped; });
        return !stop;
    }
};

KobraXPrinterAgent::KobraXPrinterAgent(std::string log_dir) { (void) log_dir; }

KobraXPrinterAgent::~KobraXPrinterAgent() { disconnect_printer(); }

AgentInfo KobraXPrinterAgent::get_agent_info_static()
{
    return AgentInfo{KOBRAX_PRINTER_AGENT_ID, "Anycubic Kobra X", VERSION, "Anycubic Kobra X LAN printer agent (MQTT over TLS)"};
}

int KobraXPrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    if (params.dev_id.empty() || params.host.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    disconnect_printer();

    auto connection         = std::make_shared<Connection>();
    connection->dev_id      = params.dev_id;
    connection->host        = params.host;
    connection->access_code = params.password;
    if (!params.port.empty() && std::all_of(params.port.begin(), params.port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && params.port.size() <= 5)
        connection->http_port = std::stoi(params.port);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connection = connection;
        m_state      = KobraX::State{};
    }
    // The destructor joins the thread, so it may use this agent directly
    connection->thread = std::thread([this, connection] { run(connection); });
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::disconnect_printer()
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connection.swap(m_connection);
    }
    if (!connection)
        return BAMBU_NETWORK_SUCCESS;
    {
        std::lock_guard<std::mutex> lock(connection->mutex);
        connection->stop = true;
    }
    connection->wake.notify_all();
    if (connection->thread.joinable())
        connection->thread.join();
    return BAMBU_NETWORK_SUCCESS;
}

void KobraXPrinterAgent::run(std::shared_ptr<Connection> c)
{
    auto delay      = std::chrono::steady_clock::duration(RETRY_FIRST);
    bool ever_open  = false;
    std::weak_ptr<KobraXPrinterAgent> self = weak_from_this();
    std::weak_ptr<Connection>         weak_connection = c;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(c->mutex);
            if (c->stop)
                return;
            c->dropped = false;
        }
        std::string             error;
        KobraX::Session::Options options{c->host, c->http_port, KobraX::MQTT_PORT, {}};
        std::shared_ptr<KobraX::Session> session;
        if (KobraX::find_client_identity(KobraX::identity_folder(), KobraX::bundled_identity_folder(), options.identity, error)) {
            session = std::make_shared<KobraX::Session>(
                options,
                [self](const std::string& suffix, const json& payload) {
                    auto agent = self.lock();
                    if (!agent)
                        return;
                    agent->on_report(suffix, payload);
                    // This runs on the session's io thread, which ~KobraXPrinterAgent ends and joins: if the last
                    // owner let go meanwhile, the agent must not be destroyed here, so the reference ends on the main
                    // thread (held in one place, whatever copies the queue makes of the task)
                    QueueOnMainFn queue;
                    {
                        std::lock_guard<std::mutex> lock(agent->m_mutex);
                        queue = agent->m_queue_on_main;
                    }
                    if (queue) {
                        auto held = std::make_shared<std::shared_ptr<KobraXPrinterAgent>>(std::move(agent));
                        queue([held] { held->reset(); });
                    }
                },
                [weak_connection](const std::string& reason) {
                    BOOST_LOG_TRIVIAL(warning) << "KobraXPrinterAgent: connection lost: " << reason;
                    if (auto connection = weak_connection.lock()) {
                        std::lock_guard<std::mutex> lock(connection->mutex);
                        connection->dropped = true;
                        connection->wake.notify_all();
                    }
                });
            if (!session->open(error))
                session.reset();
        }

        if (!session) {
            BOOST_LOG_TRIVIAL(warning) << "KobraXPrinterAgent: " << c->host << ": " << error;
            if (!ever_open) {
                // The first attempt tells the user why; GUI_App shows it and deselects the printer
                dispatch_local_connect(ConnectStatusFailed, c->dev_id, error);
                return;
            }
            if (!c->wait(delay))
                return;
            delay = std::min<std::chrono::steady_clock::duration>(delay * 2, RETRY_MAX);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(c->mutex);
            c->session = session;
        }
        {
            // A new session starts the stream again when the camera is played
            std::lock_guard<std::mutex> lock(m_mutex);
            m_camera_started.clear();
        }
        BOOST_LOG_TRIVIAL(info) << "KobraXPrinterAgent: connected to " << c->host;
        if (!ever_open) {
            dispatch_local_connect(ConnectStatusOk, c->dev_id, "0");
            OnPrinterConnectedFn connected;
            QueueOnMainFn        queue;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                connected = m_on_printer_connected;
                queue     = m_queue_on_main;
            }
            const std::string id = c->dev_id;
            if (connected && queue)
                queue([connected, id] { connected(id); });
            else if (connected)
                connected(id);
        }
        ever_open = true;
        delay     = RETRY_FIRST;

        // A read-only snapshot, then only polls: nothing else is sent unless the user asks
        session->send("info", "query");
        session->send("print", "query");
        session->send("multiColorBox", "getInfo");
        while (c->wait(POLL_EVERY) && session->is_open()) {
            {
                std::lock_guard<std::mutex> lock(c->mutex);
                if (c->dropped)
                    break;
            }
            session->send("info", "query");
            std::string state;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                state = m_state.view_state();
            }
            if (state == "printing" || state == "paused")
                session->send("print", "query");
            // The ACE units and the spool holder on every poll, as KX-Bridge and Kobralink ask: a Kobra X on firmware
            // 1.1.5.1 answered the first getInfo without its ACE in Kobra Control
            session->send("multiColorBox", "getInfo");
            dispatch_status();
        }

        {
            std::lock_guard<std::mutex> lock(c->mutex);
            c->session.reset();
        }
        session->close();
        dispatch_status(); // shows the printer as unavailable until it is back
        if (!c->wait(delay))
            return;
    }
}

std::shared_ptr<KobraX::Session> KobraXPrinterAgent::session() const
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connection = m_connection;
    }
    if (!connection)
        return nullptr;
    std::lock_guard<std::mutex> lock(connection->mutex);
    return connection->session && connection->session->is_open() ? connection->session : nullptr;
}

void KobraXPrinterAgent::on_report(const std::string& suffix, const json& payload)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string before = m_state.view_state();
        m_state.apply(suffix, payload);
        changed = before != m_state.view_state();
        if (!m_connection)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!changed && now - m_connection->last_dispatch < DISPATCH_GAP)
            return;
        m_connection->last_dispatch = now;
    }
    dispatch_status();
}

void KobraXPrinterAgent::dispatch_status()
{
    std::weak_ptr<KobraXPrinterAgent> self = weak_from_this();
    QueueOnMainFn queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queue = m_queue_on_main;
    }
    // Built on the main thread: the filament ids come from the loaded presets
    auto build = [self] {
        auto agent = self.lock();
        if (!agent)
            return;
        json        message;
        std::string dev_id;
        {
            std::lock_guard<std::mutex> lock(agent->m_mutex);
            if (!agent->m_connection)
                return;
            dev_id = agent->m_connection->dev_id;
        }
        const bool open = agent->session() != nullptr;
        {
            std::lock_guard<std::mutex> lock(agent->m_mutex);
            message = agent->m_state.push_status(open, [](const std::string& type) {
                return wxTheApp ? MoonrakerPrinterAgent::map_filament_type_to_generic_id(type) : std::string();
            });
        }
        agent->dispatch(dev_id, message);
    };
    if (queue)
        queue(build);
    else
        build();
}

void KobraXPrinterAgent::dispatch(const std::string& dev_id, const json& message)
{
    OnMessageFn fn;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn = m_on_local_message ? m_on_local_message : m_on_message;
    }
    if (fn)
        fn(dev_id, message.dump());
}

void KobraXPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn fn;
    QueueOnMainFn      queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn    = m_on_local_connect;
        queue = m_queue_on_main;
    }
    if (!fn)
        return;
    if (queue)
        queue([fn, state, dev_id, msg] { fn(state, dev_id, msg); });
    else
        fn(state, dev_id, msg);
}

int KobraXPrinterAgent::send_message(std::string dev_id, std::string json_str, int, int) { return handle(dev_id, json_str); }

int KobraXPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int, int) { return handle(dev_id, json_str); }

int KobraXPrinterAgent::handle(const std::string& dev_id, const std::string& json_str)
{
    const json message = json::parse(json_str, nullptr, false);
    if (!message.is_object())
        return BAMBU_NETWORK_ERR_INVALID_RESULT;

    if (command_of(message, "info") == "get_version") {
        std::string firmware;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            firmware = m_state.firmware;
        }
        dispatch(dev_id, {{"info",
                           {{"command", "get_version"},
                            {"result", "success"},
                            {"module", json::array({{{"name", "ota"}, {"sw_ver", firmware}, {"product_name", PRODUCT}}})}}}});
        return BAMBU_NETWORK_SUCCESS;
    }
    const std::string pushing = command_of(message, "pushing");
    if (pushing == "pushall" || pushing == "start") {
        dispatch_status();
        return BAMBU_NETWORK_SUCCESS;
    }

    const std::string system = command_of(message, "system");
    if (system == "get_access_code") {
        std::string code;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            code = m_connection ? m_connection->access_code : std::string();
        }
        dispatch(dev_id, {{"system", {{"command", "get_access_code"}, {"access_code", code}}}});
        return BAMBU_NETWORK_SUCCESS;
    }
    if (system == "ledctrl") {
        const json& body = message["system"];
        const std::string node = text(body, "led_node"), mode = text(body, "led_mode");
        // The Device tab sends chamber_light then chamber_light2; the Kobra X has one light
        if (node == "chamber_light2")
            return BAMBU_NETWORK_SUCCESS;
        if (node != "chamber_light" || (mode != "on" && mode != "off"))
            return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
        auto s = session();
        return s && s->send("light", "control", {{"type", 3}, {"status", mode == "on" ? 1 : 0}, {"brightness", mode == "on" ? 100 : 0}}) ?
                   BAMBU_NETWORK_SUCCESS :
                   BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    const std::string print = command_of(message, "print");
    if (print == "pause" || print == "resume" || print == "stop") {
        std::string task_id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            task_id = m_state.task_id.empty() ? "-1" : m_state.task_id;
        }
        auto s = session();
        return s && s->send("print", print, {{"taskid", task_id}}) ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    if (!print.empty()) {
        std::string state, task_id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            state   = m_state.view_state();
            task_id = m_state.task_id.empty() ? "-1" : m_state.task_id;
        }
        std::vector<KobraX::Message> messages;
        const KobraX::Control        control = KobraX::device_control(message["print"], state, task_id, messages);
        if (control == KobraX::Control::Unsupported) {
            // Feeding, AMS settings, calibrations, skipping objects: not on a Kobra X yet
            BOOST_LOG_TRIVIAL(info) << "KobraXPrinterAgent: not supported: " << json_str;
            return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
        }
        bool sent = control == KobraX::Control::NoOp;
        if (control == KobraX::Control::Sent) {
            auto s = session();
            sent   = s != nullptr;
            for (const KobraX::Message& m : messages)
                sent = sent && s->send(m.type, m.action, m.data, m.kind);
        }
        if (control == KobraX::Control::Refused)
            BOOST_LOG_TRIVIAL(info) << "KobraXPrinterAgent: refused while the printer is " << state << ": " << json_str;
        // The Device tab waits for the answer to a G-code line (MoonrakerPrinterAgent does the same)
        if (print == "gcode_line") {
            json reply = {{"command", "gcode_line"}, {"param", message["print"].value("param", "")}, {"result", sent ? "success" : "failed"}};
            if (message["print"].contains("sequence_id"))
                reply["sequence_id"] = message["print"]["sequence_id"];
            dispatch(dev_id, {{"print", reply}});
        }
        if (control == KobraX::Control::Refused)
            return BAMBU_NETWORK_ERR_INVALID_HANDLE;
        return sent ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    // Requests the Device tab sends on its own (other info, system and pushing commands) have no Kobra X form
    BOOST_LOG_TRIVIAL(debug) << "KobraXPrinterAgent: ignored: " << json_str;
    return BAMBU_NETWORK_SUCCESS;
}

bool KobraXPrinterAgent::start_discovery(bool start, bool sending)
{
    (void) sending;
    if (!start)
        return true;
    // The printer answers SSDP with an empty datagram, so the configured address is announced instead
    OnMsgArrivedFn fn;
    std::string    dev_id, host;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fn = m_on_ssdp;
        if (m_connection) {
            dev_id = m_connection->dev_id;
            host   = m_connection->host;
        }
        if (m_selected.empty())
            m_selected = dev_id;
    }
    if (fn && !dev_id.empty())
        fn(json{{"dev_name", PRODUCT},
                {"dev_id", dev_id},
                {"dev_ip", host},
                {"dev_type", PRODUCT},
                {"dev_signal", "0"},
                {"connect_type", "lan"},
                {"bind_state", "free"},
                {"sec_link", "secure"},
                {"ssdp_version", "v1"}}
               .dump());
    return true;
}

int KobraXPrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) sec_link;
    std::lock_guard<std::mutex> lock(m_mutex);
    detect.dev_id       = m_connection ? m_connection->dev_id : dev_ip;
    detect.dev_name     = PRODUCT;
    detect.model_id     = m_state.model;
    detect.version      = m_state.firmware;
    detect.connect_type = "lan";
    detect.bind_state   = "free";
    return BAMBU_NETWORK_SUCCESS;
}

std::string KobraXPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected;
}

int KobraXPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_selected = dev_id;
    return BAMBU_NETWORK_SUCCESS;
}

CameraStreamMode KobraXPrinterAgent::get_camera_stream_mode() const
{
    // The printer announces an HTTP-FLV stream as its "rtspUrl"; the scheme says how to play it
    const std::string url = get_camera_url();
    if (boost::istarts_with(url, "https://"))
        return CameraStreamMode::https;
    if (boost::istarts_with(url, "http://"))
        return CameraStreamMode::http;
    if (boost::istarts_with(url, "rtsp"))
        return CameraStreamMode::rtsp;
    return CameraStreamMode::none;
}

int KobraXPrinterAgent::command_start_camera(std::string dev_id)
{
    (void) dev_id;
    // The printer streams once asked to; ask once per stream URL (its token changes with a new stream)
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        url = m_state.camera_url;
        if (url.empty())
            return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
        if (url == m_camera_started)
            return BAMBU_NETWORK_SUCCESS;
    }
    auto s = session();
    if (!s || !s->send("video", "startCapture"))
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_camera_started = url;
    return BAMBU_NETWORK_SUCCESS;
}

std::string KobraXPrinterAgent::get_camera_url() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.camera_url;
}

std::string KobraXPrinterAgent::upload_name(const std::string& project_name, const std::string& path)
{
    std::string name = project_name.empty() ? fs::path(path).filename().string() : project_name;
    for (const char* extension : {".3mf", ".gcode"})
        if (boost::iends_with(name, extension))
            name.erase(name.size() - std::strlen(extension));
    std::replace_if(name.begin(), name.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F || std::strchr("\"#;*/\\", c) != nullptr;
    }, '_');
    name.erase(0, name.find_first_not_of(". "));
    // 255 bytes per name on the printer, without cutting a UTF-8 sequence
    size_t cut = std::min<size_t>(name.size(), 200);
    while (cut > 0 && cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
        --cut;
    name.resize(cut);
    return (name.empty() ? std::string("print") : name) + ".gcode";
}

int KobraXPrinterAgent::start_outcome(const std::string& suffix, const json& report)
{
    if (suffix != "print/report" || !report.is_object())
        return -1;
    const std::string state  = text(report, "state");
    const std::string action = text(report, "action");
    if ((state == "stoped" || state == "failed") && (action == "start" || action == "stop"))
        return 0;
    // A wrong file also reports checking and printing before it ends, so only a later phase proves the print runs
    static const std::vector<std::string> later = {"preheating", "auto_leveling", "hotbed_heating", "nozzle_heating",
                                                   "nozzle_cooling", "nozzle_purging", "nozzle_wiping", "frequency_sweeping",
                                                   "z_offset_calibrating", "ace_filament_feeding", "ace_filament_unfeeding",
                                                   "extfilbox_filament_feeding"};
    if (action == "start" && std::find(later.begin(), later.end(), state) != later.end())
        return 1;
    return -1;
}

int KobraXPrinterAgent::upload(const PrintParams& params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string& name, uint64_t& size)
{
    auto s = session();
    if (!s)
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    std::string upload_url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        upload_url = m_state.upload_url;
    }

    // A .gcode.3mf goes up as the plate's G-code, as Kobra Control sends it
    std::string source = params.dst_file.empty() ? params.filename : params.dst_file;
    fs::path    extracted;
    if (boost::iends_with(source, ".3mf")) {
        boost::system::error_code ec;
        extracted = fs::temp_directory_path(ec) / fs::unique_path("orca-kobrax-%%%%-%%%%.gcode", ec);
        if (ec || !MoonrakerPrinterAgent::extract_plate_gcode(source, params.plate_index, extracted.string())) {
            BOOST_LOG_TRIVIAL(error) << "KobraXPrinterAgent: no G-code for plate " << params.plate_index << " in " << source;
            return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
        }
        source = extracted.string();
    }
    struct Remove
    {
        fs::path path;
        ~Remove()
        {
            boost::system::error_code ec;
            if (!path.empty())
                fs::remove(path, ec);
        }
    } remove{extracted};

    boost::system::error_code ec;
    size = fs::file_size(source, ec);
    if (ec)
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    name = upload_name(params.project_name, params.filename);

    if (update_fn)
        update_fn(PrintingStageUpload, 0, _u8L("Uploading G-code..."));
    std::string error;
    const bool  ok = s->upload(source, name, upload_url,
        [&](int percent) {
            if (update_fn)
                update_fn(PrintingStageUpload, percent, _u8L("Uploading G-code..."));
        },
        cancel_fn, error);
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "KobraXPrinterAgent: upload failed: " << error;
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (update_fn)
        update_fn(PrintingStageCreate, 0, _u8L("Preparing..."));
    json        mapping;
    std::string error, state;
    bool        flow = true, timelapse = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state     = m_state.view_state();
        flow      = !m_state.features.contains("flow_calibration_support") || m_state.features["flow_calibration_support"] != false;
        timelapse = m_state.features.contains("camera_timelapse_support") && m_state.features["camera_timelapse_support"] == true;
        if (!KobraX::box_mapping(params.ams_mapping, m_state.slots(), mapping, error))
            mapping = nullptr;
    }
    if (state == "printing" || state == "paused" || state == "busy") {
        BOOST_LOG_TRIVIAL(error) << "KobraXPrinterAgent: the printer is " << state << "; nothing was sent";
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }
    if (mapping.is_null()) {
        BOOST_LOG_TRIVIAL(error) << "KobraXPrinterAgent: " << error << "; nothing was sent";
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    std::string name;
    uint64_t    size = 0;
    if (const int result = upload(params, update_fn, cancel_fn, name, size); result != BAMBU_NETWORK_SUCCESS)
        return result;

    if (update_fn)
        update_fn(PrintingStageSending, 0, _u8L("Starting print..."));
    auto s = session();
    if (!s)
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    const json data = KobraX::print_start_data(name, size, mapping, params.task_bed_leveling, params.task_flow_cali && flow,
                                               params.task_record_timelapse && timelapse);
    // The first start report, then a few seconds more: a file the printer cannot use is reported as
    // starting and then ends within a second (code 10111)
    json reply;
    if (!s->request("print", "start", data,
                    [](const std::string& suffix, const json& report) {
                        return suffix == "print/report" &&
                               (start_outcome(suffix, report) >= 0 || text(report, "action") == "start");
                    },
                    START_TIMEOUT, reply)) {
        BOOST_LOG_TRIVIAL(error) << "KobraXPrinterAgent: the printer did not answer the print start";
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }
    if (start_outcome("print/report", reply) < 0 && text(reply, "state") != "failed")
        s->wait([](const std::string& suffix, const json& report) { return start_outcome(suffix, report) >= 0; }, START_WATCH, reply);
    if (start_outcome("print/report", reply) == 0 || text(reply, "state") == "failed") {
        BOOST_LOG_TRIVIAL(error) << "KobraXPrinterAgent: the printer ended the print at once: " << reply.dump();
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }
    if (update_fn)
        update_fn(PrintingStageFinished, 100, _u8L("Print started"));
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    // LAN only: the printer has no cloud path here
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int KobraXPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int KobraXPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    if (update_fn)
        update_fn(PrintingStageCreate, 0, _u8L("Preparing..."));
    std::string name;
    uint64_t    size = 0;
    if (const int result = upload(params, update_fn, cancel_fn, name, size); result != BAMBU_NETWORK_SUCCESS)
        return result;
    if (update_fn)
        update_fn(PrintingStageFinished, 100, _u8L("File uploaded"));
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::start_sdcard_print(PrintParams, OnUpdateStatusFn, WasCancelledFn)
{
    // Printing a file already on the printer comes with the file list, in a later phase
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int KobraXPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_ssdp = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_printer_connected = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_local_connect = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_local_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int KobraXPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue_on_main = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
