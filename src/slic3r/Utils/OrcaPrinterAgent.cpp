#include "OrcaPrinterAgent.hpp"
#include "OrcaCloudSignalingChannel.hpp"
#include "Http.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "OrcaCloudServiceAgent.hpp"
#include "bambu_networking.hpp"
#include "json_diff.hpp"
#include <algorithm>
#include <atomic>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <random>
#include <ratio>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

namespace Slic3r {

const std::string OrcaPrinterAgent_VERSION = "0.0.1";

namespace {

namespace fs = boost::filesystem;

// params.filename is normally the exported .3mf archive; the sliced G-code sits
// beside it with the same stem (".12345.0.3mf" -> ".12345.0.gcode"). params.dst_file,
// when set, already points straight at a file (the "print a file already on the
// card" flow), so it wins.
std::string resolve_local_gcode_path(const PrintParams& params)
{
    if (!params.dst_file.empty())
        return params.dst_file;

    std::string path = params.filename;
    if (boost::iends_with(path, ".3mf"))
        path.replace(path.size() - 4, 4, ".gcode");
    return path;
}

// The name the file is stored as under the printer's `gcodes` root, and the value
// passed to OrcaSonar's print.gcode_file `param`. Must be a pure function of params
// so start_local_print's upload and start_sdcard_print's start agree on it.
// OrcaSonar rejects newlines, ';', '#', '*' and NUL in the path, and Klipper's
// SDCARD_PRINT_FILE splits its argument on whitespace, so collapse anything unsafe.
std::string remote_gcode_name(const PrintParams& params)
{
    std::string name = params.project_name.empty() ? fs::path(resolve_local_gcode_path(params)).filename().string() :
                                                     fs::path(params.project_name).filename().string();

    if (boost::iends_with(name, ".3mf")) // "model.gcode.3mf" -> "model.gcode"
        name.erase(name.size() - 4);

    std::replace_if(
        name.begin(), name.end(),
        [](unsigned char c) { return std::isspace(c) != 0 || c == ';' || c == '#' || c == '*' || c == '/' || c == '\\'; }, '_');

    if (name.empty())
        name = "orca_print";
    if (!boost::iends_with(name, ".gcode"))
        name += ".gcode";
    return name;
}

// http(s) origin of the Moonraker-compatible upload facade, derived from the live
// LAN MQTT session URL ("ws://host:port/mqtt" -> "http://host:port"). Used only as
// a fallback when the print job carries no dev_ip of its own.
std::string http_origin_from_lan_ws(const std::string& ws_url)
{
    if (ws_url.empty())
        return {};
    std::string s = ws_url;
    if (boost::istarts_with(s, "wss://"))
        s = "https://" + s.substr(6);
    else if (boost::istarts_with(s, "ws://"))
        s = "http://" + s.substr(5);
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        if (const auto slash = s.find('/', scheme + 3); slash != std::string::npos)
            s.erase(slash);
    }
    return s;
}

// print.gcode_file is non-idempotent and OrcaSonar replays a cached response for a
// reused (namespace, command, sequence_id). Seed from the wall clock so ids do not
// collide across slicer restarts, then bump once per call within a run.
std::string next_gcode_file_sequence_id()
{
    static std::atomic<uint64_t> counter{[] {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    }()};
    return std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

static constexpr const char* ORCASONAR_FALLBACK = "orcasonar";

bool fetch_orcasonar_body(const std::string& url, std::string& body)
{
    bool ok = false;
    Http::get(url)
        .timeout_connect(4)
        .timeout_max(6)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            }
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: identity probe " << url << " failed status=" << status << " err=" << err;
        })
        .perform_sync();
    return ok;
}

std::string extract_line_value(const std::string& body, const std::string& key)
{
    const auto pos = body.find(key);
    if (pos == std::string::npos)
        return {};
    const auto value_start = pos + key.size();
    const auto value_end   = body.find('\n', value_start);
    std::string value      = body.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
    boost::trim(value);
    return value;
}

// Manual binding uses the OrcaSonar landing page as its sole identity source.
// The page returns device_id and may return device_name/model_id.
bool probe_orcasonar_landing_page(
    const std::string& host, const std::string& port, std::string& device_id, std::string& device_name, std::string& model_id)
{
    device_name = ORCASONAR_FALLBACK;
    model_id    = ORCASONAR_FALLBACK;

    std::string body;
    if (fetch_orcasonar_body("http://" + host + ":" + port + "/", body)) {
        device_id               = extract_line_value(body, "device_id=");
        const std::string name  = extract_line_value(body, "device_name=");
        const std::string model = extract_line_value(body, "model_id=");
        if (!name.empty())
            device_name = name;
        if (!model.empty())
            model_id = model;
        if (!device_id.empty())
            return true;
    }

    return false;
}

// SSDP discovery uses the LOCATION URL's UPnP device description as its sole
// identity source. OrcaSonar maps device_id/device_name/model_id to UDN,
// friendlyName, and modelNumber respectively.
bool parse_orcasonar_device_xml(const std::string& body, std::string& device_id, std::string& device_name, std::string& model_id)
{
    device_name = ORCASONAR_FALLBACK;
    model_id    = ORCASONAR_FALLBACK;

    try {
        boost::property_tree::ptree tree;
        std::istringstream stream(body);
        boost::property_tree::read_xml(stream, tree, boost::property_tree::xml_parser::trim_whitespace);
        const auto device = tree.get_child_optional("root.device");
        if (!device)
            return false;

        std::string udn = device->get<std::string>("UDN", "");
        boost::trim(udn);
        if (boost::istarts_with(udn, "uuid:"))
            device_id = udn.substr(5);
        else
            device_id = device->get<std::string>("device_id", "");
        boost::trim(device_id);
        if (device_id.empty())
            return false;

        device_name = device->get<std::string>("friendlyName", "");
        if (device_name.empty())
            device_name = device->get<std::string>("device_name", ORCASONAR_FALLBACK);
        model_id = device->get<std::string>("modelNumber", "");
        if (model_id.empty())
            model_id = device->get<std::string>("model_id", ORCASONAR_FALLBACK);
        boost::trim(device_name);
        boost::trim(model_id);
        if (device_name.empty())
            device_name = ORCASONAR_FALLBACK;
        if (model_id.empty())
            model_id = ORCASONAR_FALLBACK;
        return true;
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: failed to parse OrcaSonar device.xml: " << error.what();
        return false;
    }
}

bool probe_orcasonar_device_xml(const std::string& location, std::string& device_id, std::string& device_name, std::string& model_id)
{
    std::string body;
    return fetch_orcasonar_body(location, body) && parse_orcasonar_device_xml(body, device_id, device_name, model_id);
}
} // namespace

class OrcaPrinterAgent::OrcaSonarDiscovery
{
public:
    using EmitFn = std::function<void(const std::string&)>;

    explicit OrcaSonarDiscovery(EmitFn emit) : m_emit(std::move(emit)) {}
    ~OrcaSonarDiscovery() { stop(); }

    OrcaSonarDiscovery(const OrcaSonarDiscovery&)            = delete;
    OrcaSonarDiscovery& operator=(const OrcaSonarDiscovery&) = delete;

    void start()
    {
        std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
        if (m_running.exchange(true))
            return;
        m_thread = std::thread(&OrcaSonarDiscovery::browse_loop, this);
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
        if (!m_running.exchange(false))
            return;
        m_wait_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    static std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    static std::string trim_ascii(const std::string& value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    static bool get_header(const std::string& datagram, const std::string& header_name, std::string& value)
    {
        std::size_t line_start = 0;
        while (line_start < datagram.size()) {
            const std::size_t line_end = datagram.find('\n', line_start);
            const std::string line = datagram.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
            line_start             = line_end == std::string::npos ? datagram.size() : line_end + 1;

            const std::size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            if (lower_ascii(trim_ascii(line.substr(0, colon))) != lower_ascii(header_name))
                continue;
            value = trim_ascii(line.substr(colon + 1));
            return !value.empty();
        }
        return false;
    }

    static bool make_machine_alive_json(const std::string& usn, const std::string& host, const std::string& location, std::string& json)
    {
        const std::string lower_usn          = lower_ascii(usn);
        static const std::string uuid_prefix = "uuid:";
        static const std::string device_type = "::urn:schemas-upnp-org:device:basic:1";
        if (lower_usn.rfind(uuid_prefix, 0) != 0)
            return false;

        const std::size_t type_start = lower_usn.find(device_type, uuid_prefix.size());
        if (type_start == std::string::npos)
            return false;
        if (host.empty())
            return false;

        const std::string lower_location = lower_ascii(location);
        const std::size_t scheme_end     = lower_location.find("://");
        if (scheme_end == std::string::npos)
            return false;
        const std::size_t authority_start = scheme_end + 3;
        const std::size_t authority_end   = location.find_first_of("/ ?#", authority_start);
        const std::string authority       = location.substr(authority_start, authority_end == std::string::npos ?
                                                                                 std::string::npos :
                                                                                 authority_end - authority_start);
        std::string port                  = "8280";
        if (!authority.empty()) {
            if (authority.front() == '[') {
                const std::size_t bracket = authority.find(']');
                if (bracket != std::string::npos && bracket + 1 < authority.size() && authority[bracket + 1] == ':')
                    port = authority.substr(bracket + 2);
            } else {
                const std::size_t colon = authority.rfind(':');
                if (colon != std::string::npos && colon + 1 < authority.size())
                    port = authority.substr(colon + 1);
            }
        }
        if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos)
            return false;

        // SSDP LOCATION commonly advertises the device's mDNS name (for
        // example, http://orcasonar-123.local:8280/upnp/device.xml). The
        // discovery response already gives us the sender's reachable address,
        // so use that address for the HTTP probe instead of requiring the
        // platform HTTP client to resolve .local. Keep the advertised path so
        // this remains compatible with non-default device-description URLs.
        const std::string location_path = authority_end == std::string::npos ? "/" : location.substr(authority_end);
        const std::string probe_host    = host.find(':') == std::string::npos ? host : "[" + host + "]";
        const std::string probe_url     = location.substr(0, scheme_end + 3) + probe_host + ":" + port + location_path;

        std::string device_id;
        std::string device_name;
        std::string model_id;
        if (!probe_orcasonar_device_xml(probe_url, device_id, device_name, model_id))
            return false;

        nlohmann::json machine;
        machine["dev_name"]        = device_name;
        machine["dev_id"]          = device_id;
        machine["dev_type"]        = model_id;
        machine["connection_name"] = device_id;
        machine["dev_ip"]          = host + ":" + port;

        machine["dev_signal"]   = "0";
        machine["connect_type"] = "lan";
        machine["bind_state"]   = "free";
        machine["sec_link"]     = "secure";
        machine["ssdp_version"] = "v1";
        json                    = machine.dump();
        return true;
    }

    void ssdp_round()
    {
        namespace asio = boost::asio;
        using asio::ip::udp;
        try {
            asio::io_context io_context;
            udp::socket socket(io_context);
            socket.open(udp::v4());
            socket.set_option(udp::socket::reuse_address(true));
            socket.bind(udp::endpoint(udp::v4(), 0));
            socket.non_blocking(true);

            static constexpr char search_request[] = "M-SEARCH * HTTP/1.1\r\n"
                                                     "HOST: 239.255.255.250:1900\r\n"
                                                     "MAN: \"ssdp:discover\"\r\n"
                                                     "MX: 2\r\n"
                                                     "ST: urn:schemas-upnp-org:device:Basic:1\r\n"
                                                     "\r\n";
            const auto multicast                   = asio::ip::make_address_v4("239.255.255.250");
            socket.send_to(asio::buffer(search_request, sizeof(search_request) - 1), udp::endpoint(multicast, 1900));

            std::array<char, 4096> buffer{};
            udp::endpoint sender;
            std::set<std::string> seen_ids;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (m_running.load() && std::chrono::steady_clock::now() < deadline) {
                boost::system::error_code error;
                const std::size_t received = socket.receive_from(asio::buffer(buffer), sender, 0, error);
                if (!error) {
                    const std::string datagram(buffer.data(), received);
                    std::string usn;
                    std::string location;
                    std::string server;
                    if (get_header(datagram, "USN", usn) && get_header(datagram, "LOCATION", location) &&
                        (!get_header(datagram, "SERVER", server) || lower_ascii(server).find("orcasonar") != std::string::npos)) {
                        std::string machine_alive;
                        if (make_machine_alive_json(usn, sender.address().to_string(), location, machine_alive)) {
                            nlohmann::json machine      = nlohmann::json::parse(machine_alive);
                            const std::string device_id = machine["dev_id"].get<std::string>();
                            if (seen_ids.insert(device_id).second && m_emit)
                                m_emit(machine_alive);
                        }
                    }
                } else if (error != asio::error::would_block && error != asio::error::try_again) {
                    BOOST_LOG_TRIVIAL(warning) << "OrcaSonarDiscovery: SSDP receive failed: " << error.message();
                    break;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(warning) << "OrcaSonarDiscovery: SSDP round failed: " << error.what();
        }
    }

    void browse_loop()
    {
        while (m_running.load()) {
            ssdp_round();
            std::unique_lock<std::mutex> lock(m_wait_mutex);
            m_wait_cv.wait_for(lock, std::chrono::seconds(5), [this] { return !m_running.load(); });
        }
    }

    EmitFn m_emit;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::mutex m_lifecycle_mutex;
    std::mutex m_wait_mutex;
    std::condition_variable m_wait_cv;
};

OrcaPrinterAgent::OrcaPrinterAgent(std::string log_dir) : log_dir(std::move(log_dir)) {}

const char* OrcaPrinterAgent::connection_type_name(CurrentConn connection)
{
    switch (connection) {
    case LAN: return "LAN";
    case CLOUD: return "cloud";
    case NONE: return "none";
    }
    return "unknown";
}

OrcaPrinterAgent::~OrcaPrinterAgent()
{
    start_discovery(false, false);
    ++m_lan_generation; // fence any late worker callback
    ++m_cloud_generation;

    // Drop the cloud status callback before anything else: it holds `this`, and the
    // cloud agent outlives the printer agent (NetworkAgent::set_printer_agent swaps
    // the printer agent while m_cloud_agents persist).
    if (auto* cloud = get_orca_cloud_agent())
        cloud->set_printer_status_callback(nullptr);

    // Stop the LAN connection so the connect thread's start() returns, but keep the
    // object alive until that thread is joined (the thread holds a raw conn pointer).
    OrcaMqttConnection* live_lan = nullptr;
    {
        std::lock_guard<std::mutex> l(state_mutex);
        live_lan = lan_mqtt_connection.get();
    }
    if (live_lan)
        live_lan->stop();

    // Same for the cloud per-printer connection the cloud connect thread may hold.
    if (auto* cloud = get_orca_cloud_agent())
        cloud->teardown_selected_printer_mqtt();

    if (m_lan_connect_thread.joinable())
        m_lan_connect_thread.join();
    if (m_cloud_connect_thread.joinable())
        m_cloud_connect_thread.join();

    // stop() is not sticky: a connect thread that had not yet reached start() when the
    // teardown above ran could have raised a fresh socket in between. Tear down once
    // more now that both threads are joined, so no live socket survives *this.
    if (auto* cloud = get_orca_cloud_agent())
        cloud->teardown_selected_printer_mqtt();

    {
        std::lock_guard<std::mutex> l(state_mutex);
        lan_mqtt_connection.reset();
    }
}

OrcaCloudServiceAgent* OrcaPrinterAgent::get_orca_cloud_agent()
{
    if (!m_cloud_agent)
        return nullptr;

    return dynamic_cast<OrcaCloudServiceAgent*>(m_cloud_agent.get());
}

OrcaMqttConnection* OrcaPrinterAgent::get_appropriate_mqtt_connection(bool is_lan)
{
    if (is_lan) {
        std::lock_guard<std::mutex> l(state_mutex);
        return lan_mqtt_connection.get();
    }
    auto* cloud = get_orca_cloud_agent();
    return cloud ? cloud->get_mqtt_connection() : nullptr;
}

// ============================================================================
// Orca-dialect -> Bambu-dialect compatibility shim for inbound printer reports.
//
// OrcaSonar and the bridge adapter speak the "Orca Protocol" JSON dialect.
// MachineObject::parse_json (DeviceManager.cpp) only understands the Bambu
// dialect. Until the Orca Protocol is formally specified, this function is the
// ONE place where an inbound Orca-dialect report is rewritten into the Bambu
// shape parse_json already handles.
//
// Rules of this seam:
//   1. parse_json and the rest of DeviceManager are NOT modified to accommodate
//      the Orca dialect - every such accommodation is a rule inside this
//      function.
//   2. Each rule documents its Orca-dialect source, the Bambu-dialect target
//      parse_json expects, and the rewrite between them. Rules are independent
//      and can be removed one at a time as parse_json gains native support.
//   3. When parse_json reads the Orca dialect directly, this function and the
//      single call in deliver_to_sink can be deleted, state included. Nothing
//      else should need to change.
//
// It runs on every inbound report, so it stays cheap for payloads it does not
// touch (substring pre-check, no re-serialize unless something changed) and
// never throws (non-throwing parse, every field access guarded).
// ============================================================================
std::string OrcaPrinterAgent::merge_capabilities(const std::string& dev_id, const std::string& payload)
{
    if (payload.find("get_capabilities") == std::string::npos && payload.find("push_status") == std::string::npos)
        return payload;

    nlohmann::json envelope = nlohmann::json::parse(payload, nullptr, false);
    if (!envelope.is_object())
        return payload;

    // Shim-local state, kept here (not on the class) so deleting this function
    // removes its storage too. Process-wide and keyed by the globally-unique
    // dev_id, with its own mutex; C++11 makes the one-time init thread-safe.
    static std::unordered_map<std::string, double> nozzle_diameter_cache;
    static std::mutex                              nozzle_diameter_cache_mutex;

    bool modified = false;

    // ---- Rule: nozzle geometry ---------------------------------------------
    // Orca dialect : info.capabilities.topology.tools[i].nozzle.diameter_mm,
    //                delivered once in the get_capabilities reply; push_status
    //                frames carry no nozzle geometry at all.
    // Bambu dialect: parse_json runs DevNozzleSystemParser::ParseV1_0 only for a
    //                push_status frame that holds BOTH print.nozzle_diameter and
    //                print.nozzle_type.
    // Rewrite      : cache the diameter from the capabilities reply per device,
    //                then stamp print.nozzle_diameter + a neutral print.nozzle_type
    //                ("N/A" -> NozzleType::ntUndefine, as MoonrakerPrinterAgent
    //                does; Klipper has no Bambu nozzle type) onto later push_status
    //                frames that carry no real nozzle data.
    const auto info_it = envelope.find("info");
    const bool is_capabilities_reply = info_it != envelope.end() && info_it->is_object() &&
                                       info_it->value("command", "") == "get_capabilities";

    if (is_capabilities_reply) {
        double nozzle_dia  = 0.0;
        const auto caps_it = info_it->find("capabilities");
        if (caps_it != info_it->end() && caps_it->is_object()) {
            const auto topology_it = caps_it->find("topology");
            if (topology_it != caps_it->end() && topology_it->is_object()) {
                const auto tools_it = topology_it->find("tools");
                if (tools_it != topology_it->end() && tools_it->is_array()) {
                    // First tool with a usable diameter wins: ParseV1_0 keeps a
                    // single nozzle (id 0).
                    for (const auto& tool : *tools_it) {
                        if (!tool.is_object())
                            continue;
                        const auto nozzle_it = tool.find("nozzle");
                        if (nozzle_it == tool.end() || !nozzle_it->is_object())
                            continue;
                        const auto dia_it = nozzle_it->find("diameter_mm");
                        if (dia_it != nozzle_it->end() && dia_it->is_number() && dia_it->get<double>() > 0.0) {
                            nozzle_dia = dia_it->get<double>();
                            break;
                        }
                    }
                }
            }
        }
        if (nozzle_dia > 0.0) {
            std::lock_guard<std::mutex> l(nozzle_diameter_cache_mutex);
            nozzle_diameter_cache[dev_id] = nozzle_dia;
        }
        // The capabilities reply itself is forwarded unchanged.
    }
    else {
        const auto print_it = envelope.find("print");
        if (print_it != envelope.end() && print_it->is_object() &&
            print_it->value("command", "") == "push_status" && !print_it->contains("nozzle_diameter")) {

            double nozzle_dia = 0.0;
            {
                std::lock_guard<std::mutex> l(nozzle_diameter_cache_mutex);
                const auto it = nozzle_diameter_cache.find(dev_id);
                if (it != nozzle_diameter_cache.end())
                    nozzle_dia = it->second;
            }
            if (nozzle_dia > 0.0) {
                (*print_it)["nozzle_diameter"] = nozzle_dia;
                (*print_it)["nozzle_type"]     = "N/A";
                modified = true;
            }
        }
    }
    // ----------------------------------------------------------------------

    return modified ? envelope.dump() : payload;
}

void OrcaPrinterAgent::deliver_to_sink(const std::string& dev_id, const std::string& payload, bool local)
{
    parse_ipcam_info(dev_id, payload);
    std::string merged_payload = merge_capabilities(dev_id, payload);

    OnMessageFn fn;
    QueueOnMainFn q;
    {
        std::lock_guard<std::mutex> l(state_mutex);
        fn = local ? on_local_message_fn : on_message_fn;
        q  = queue_on_main_fn;
    }
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: delivering " << (local ? "local" : "cloud") << " report payload dev_id=" << dev_id
                            << " payload=" << merged_payload
                            << " callback=" << (fn ? "set" : "null") << " queue_on_main=" << (q ? "set" : "null");
    if (!fn) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: dropping " << (local ? "local" : "cloud") << " message because on_message_fn is not set"
                                   << " dev_id=" << dev_id;
        return;
    }
    if (q)
        q([fn, dev_id, merged_payload] { fn(dev_id, merged_payload); });
    else
        fn(dev_id, merged_payload);
}

void OrcaPrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& message)
{
    OnLocalConnectedFn callback;
    QueueOnMainFn queue;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        callback = on_local_connect_fn;
        queue    = queue_on_main_fn;
    }

    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN connection callback state=" << state << " dev_id=" << dev_id << " message=" << message
                            << " callback=" << (callback ? "set" : "null") << " queue_on_main=" << (queue ? "set" : "null");
    if (!callback)
        return;

    auto dispatch = [callback, state, dev_id, message] { callback(state, dev_id, message); };
    if (queue)
        queue(dispatch);
    else
        dispatch();
}

std::function<void(const std::string&, const std::string&)> OrcaPrinterAgent::make_lan_message_handler(uint64_t generation)
{
    return [this, generation](const std::string& id, const std::string& payload) {
        if (generation == m_lan_generation.load())
            deliver_to_sink(id, payload, true);
        else
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: dropping stale LAN message generation=" << generation
                                    << " current_generation=" << m_lan_generation.load() << " dev_id=" << id;
    };
}

void OrcaPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_cloud_agent: cloud=" << (cloud ? cloud->get_id() : "<null>");
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        m_cloud_agent = cloud;
    }
    if (!get_orca_cloud_agent()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent::set_cloud_agent: cloud is not OrcaCloudServiceAgent";
        return; // BBL provider active - nothing to bridge
    }

    // OrcaCloudServiceAgent owns the aggregate MQTT socket; it already strips the
    // device/<id>/report topic and hands us (dev_id, raw_json). Forward to the
    // standard sink. message_arrive_fn self-marshals to the UI thread via CallAfter,
    // so being called from the MQTT worker thread is fine.
    const int callback_result = get_orca_cloud_agent()->set_printer_status_callback(
        [this](std::string dev_id, std::string payload) { deliver_to_sink(dev_id, payload, false); });
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_cloud_agent: status callback result=" << callback_result;
}

std::unique_ptr<ICameraSignalingChannel> OrcaPrinterAgent::create_camera_signaling_channel(const std::string& dev_id)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!m_cloud_agent)
        return nullptr;
    return std::make_unique<OrcaCloudSignalingChannel>(m_cloud_agent, dev_id);
}

// ============================================================================
// Communication
// ============================================================================

int OrcaPrinterAgent::send_message(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{ return route_send(/*is_lan=*/false, dev_id, json_str); }

int OrcaPrinterAgent::command_ams_refresh_rfid(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode)
{
    int tray_number = 0;
    if (!parse_nonnegative_command_id(tray_id, tray_number)) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: invalid RFID tray id=" << tray_id;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    nlohmann::json j;
    j["print"]["command"]     = "ams_get_rfid";
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    j["print"]["tray_id"]     = tray_number;
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_ams_calibrate(std::string /*dev_id*/, int /*ams_id*/, int /*sequence_id*/, bool /*lan_mode*/)
{
    // OrcaSonar has no ams_calibrate command. Do not send the Bambu M620 C
    // dialect through the vendor-neutral OrcaSonar gcode_line command.
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: AMS calibration is not part of the OrcaSonar API";
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int OrcaPrinterAgent::command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode)
{
    int tray_number = 0;
    if (!parse_nonnegative_command_id(tray_id, tray_number)) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: invalid AMS target tray id=" << tray_id;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    nlohmann::json j;
    j["print"]["command"]     = "ams_change_filament";
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    j["print"]["target"]      = tray_number;
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_start_camera(std::string /*dev_id*/)
{
    // OrcaSonar exposes camera.ipcam_* controls, not the legacy start_camera
    // operation used by the Bambu agent.
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: camera start is not part of the OrcaSonar API";
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int OrcaPrinterAgent::command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode)
{
    nlohmann::json j;
    j["print"]["command"]     = "gcode_line";
    j["print"]["param"]       = "G90\n";
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode)
{
    nlohmann::json j;
    j["print"]["command"]     = "gcode_line";
    j["print"]["param"]       = "G29\n";
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode)
{
    nlohmann::json j;
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    if (supports_mqtt_homing) {
        j["print"]["command"] = "back_to_center";
    } else {
        // Preserve the existing safety behavior: never home Z/Y during a print.
        j["print"]["command"] = "gcode_line";
        j["print"]["param"]   = is_printing ? "G28 X\n" : "G28\n";
    }
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_set_bed(std::string dev_id, int temp, bool /*supports_mqtt_bed_ctrl*/, int sequence_id, bool lan_mode)
{
    nlohmann::json j;
    j["print"]["command"]     = "set_bed_temp";
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    j["print"]["temp"]        = temp;
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode)
{
    nlohmann::json j;
    j["print"]["command"]        = "set_nozzle_temp";
    j["print"]["sequence_id"]    = std::to_string(sequence_id);
    j["print"]["extruder_index"] = 0;
    j["print"]["target_temp"]    = temp;
    return route_send(lan_mode, dev_id, j.dump());
}

int OrcaPrinterAgent::command_axis_control(std::string dev_id,
                                           std::string axis,
                                           double unit,
                                           double input_val,
                                           int /*speed*/,
                                           bool is_core_xy,
                                           bool /*supports_mqtt_axis_control*/,
                                           int sequence_id,
                                           bool lan_mode)
{
    std::transform(axis.begin(), axis.end(), axis.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (axis != "X" && axis != "Y" && axis != "Z" && axis != "E") {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: invalid axis control axis=" << axis;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    const double requested_distance = input_val * unit;
    if (!std::isfinite(requested_distance) || requested_distance == 0.0) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: invalid axis control distance input=" << input_val << " unit=" << unit;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    int direction = requested_distance > 0.0 ? 1 : -1;
    if (!is_core_xy && (axis == "Y" || axis == "Z"))
        direction = -direction;

    nlohmann::json j;
    j["print"]["command"]     = "xyz_ctrl";
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    j["print"]["axis"]        = axis;
    j["print"]["dir"]         = direction;
    j["print"]["distance"]    = requested_distance < 0.0 ? -requested_distance : requested_distance;
    return route_send(lan_mode, dev_id, j.dump());
}

bool OrcaPrinterAgent::parse_lan_endpoint(const std::string& dev_ip, std::string& host, std::string& port)
{
    std::string s = dev_ip;
    if (s.rfind("http://", 0) == 0)
        s.erase(0, 7);
    else if (s.rfind("https://", 0) == 0)
        s.erase(0, 8);
    if (const auto slash = s.find('/'); slash != std::string::npos)
        s.erase(slash);
    if (s.empty())
        return false;
    port = "8280";
    // split a trailing :port only for host:port, not an unbracketed IPv6 literal
    if (const auto colon = s.rfind(':'); colon != std::string::npos && s.find(']') == std::string::npos) {
        port = s.substr(colon + 1);
        s.erase(colon);
    }
    if (s.empty() || port.empty())
        return false;
    host = s;
    return true;
}

std::string OrcaPrinterAgent::make_lan_client_id(const std::string& dev_id)
{
    static const std::string suffix = [] {
        std::random_device rd;
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rd()));
        return std::string(buf);
    }();
    return "orcaslicer-lan-" + dev_id + "-" + suffix;
}

bool OrcaPrinterAgent::parse_nonnegative_command_id(const std::string& value, int& result)
{
    if (value.empty())
        return false;
    try {
        std::size_t consumed   = 0;
        const long long parsed = std::stoll(value, &consumed);
        if (consumed != value.size() || parsed < 0 || parsed > std::numeric_limits<int>::max())
            return false;
        result = static_cast<int>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void OrcaPrinterAgent::parse_ipcam_info(const std::string& dev_id, const std::string& payload)
{
    const nlohmann::json envelope = nlohmann::json::parse(payload, nullptr, false);
    if (!envelope.is_object())
        return;

    const auto print_it = envelope.find("print");
    if (print_it == envelope.end() || !print_it->is_object())
        return;

    const nlohmann::json& print = *print_it;
    const auto command_it       = print.find("command");
    const bool is_push_status   = command_it != print.end() && command_it->is_string() && command_it->get<std::string>() == "push_status";
    bool is_full_snapshot       = false;
    if (is_push_status) {
        const auto msg_it = print.find("msg");
        is_full_snapshot  = msg_it == print.end() || (msg_it->is_number_integer() && msg_it->get<int>() == 0);
    }

    CameraStreamMode stream_mode = CameraStreamMode::none;
    std::string stream_url;
    bool has_camera_update = false;
    const auto ipcam_it    = print.find("ipcam");
    if (ipcam_it != print.end() && ipcam_it->is_object()) {
        const auto stream_modes_it = ipcam_it->find("stream_mode");
        if (stream_modes_it != ipcam_it->end() && stream_modes_it->is_array()) {
            has_camera_update = true;
            for (const auto& stream : *stream_modes_it) {
                if (!stream.is_object())
                    continue;
                const auto mode_it = stream.find("mode");
                const auto url_it  = stream.find("url");
                if (mode_it == stream.end() || url_it == stream.end() || !mode_it->is_string() || !url_it->is_string())
                    continue;

                const std::string mode = mode_it->get<std::string>();
                if (mode == "rtsp")
                    stream_mode = CameraStreamMode::rtsp;
                else if (mode == "http")
                    stream_mode = CameraStreamMode::http;
                else if (mode == "http_snapshot")
                    stream_mode = CameraStreamMode::http_snapshot;
                else
                    continue;

                stream_url = url_it->get<std::string>();
                break; // OrcaSonar orders entries by preference.
            }
        } else if (is_full_snapshot) {
            has_camera_update = true;
        }
    } else if (is_full_snapshot) {
        // A full push_status without ipcam means the printer has no camera
        // stream information. Diff reports omit unchanged domains.
        has_camera_update = true;
    }

    if (!has_camera_update)
        return;

    std::lock_guard<std::mutex> lock(state_mutex);
    if (m_current_connection != LAN || m_lan_dev_id != dev_id) {
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: ignoring camera update for inactive LAN printer dev_id=" << dev_id;
        return;
    }

    m_camera_stream_mode = stream_mode;
    m_camera_url         = std::move(stream_url);
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: updated camera state dev_id=" << dev_id << " transport=LAN"
                            << " mode=" << static_cast<int>(m_camera_stream_mode) << " url=" << m_camera_url;
}

std::string OrcaPrinterAgent::lan_connection_target() const
{
    std::lock_guard<std::mutex> l(state_mutex);
    return m_lan_url;
}

std::string OrcaPrinterAgent::seq(int n) { return std::to_string(20000 + (n % 10000)); }

std::string OrcaPrinterAgent::build_pushing_start(const std::string& sid)
{ return R"({"pushing":{"command":"start","sequence_id":")" + sid + R"("}})"; }
std::string OrcaPrinterAgent::build_pushing_stop(const std::string& sid)
{ return R"({"pushing":{"command":"stop","sequence_id":")" + sid + R"("}})"; }
std::string OrcaPrinterAgent::build_pushall(const std::string& sid)
{ return R"({"pushing":{"command":"pushall","sequence_id":")" + sid + R"(","version":1,"push_target":1}})"; }
std::string OrcaPrinterAgent::build_get_version(const std::string& sid)
{ return R"({"info":{"command":"get_version","sequence_id":")" + sid + R"("}})"; }
std::string OrcaPrinterAgent::build_get_capabilities(const std::string& sid)
{ return R"({"info":{"command":"get_capabilities","sequence_id":")" + sid + R"("}})"; }

void OrcaPrinterAgent::emit_connect_sequence(const std::string& dev_id,
                                             std::function<void(const std::string&)> subscribe,
                                             std::function<void(const std::string&)> request)
{
    subscribe(dev_id);
    request(build_pushing_start(seq(1)));
    request(build_pushall(seq(2)));
    request(build_get_version(seq(3)));
    request(build_get_capabilities(seq(4)));
}

void OrcaPrinterAgent::on_connected(const std::string& dev_id, OrcaMqttConnection* conn, uint64_t generation)
{
    // Called from both connect paths with whichever epoch that path captured; the two
    // counters are independent, so matching either one means the caller is still live.
    if (!conn || (generation != m_lan_generation.load() && generation != m_cloud_generation.load()))
        return;
    emit_connect_sequence(
        dev_id, [conn](const std::string& id) { conn->subscribe(id); },
        [conn, dev_id](const std::string& body) { conn->send_request(dev_id, body); }); // dev_id captured BY VALUE
}

int OrcaPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: connect_printer requested dev_id=" << dev_id << " dev_ip=" << dev_ip
                            << " username=" << (username.empty() ? "<default>" : username) << " password_present=" << (!password.empty())
                            << " use_ssl=" << use_ssl;
    (void) use_ssl; // OrcaSonar LAN is plaintext ws://
    if (dev_id.empty() || dev_ip.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: connect_printer rejected missing dev_id or dev_ip";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    std::string host, port;
    if (!parse_lan_endpoint(dev_ip, host, port)) {
        BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: connect_printer rejected unparsable LAN endpoint dev_ip=" << dev_ip;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    disconnect_printer();
    const uint64_t gen = ++m_lan_generation;

    OrcaMqttConnection::Config cfg;
    cfg.url               = "ws://" + host + ":" + port + "/mqtt";
    cfg.use_tls           = false;
    cfg.username          = username.empty() ? std::string("orcasonar") : username;
    cfg.password          = password;
    cfg.client_id         = make_lan_client_id(dev_id);
    cfg.keepalive_seconds = 60;

    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN connection prepared generation=" << gen << " host=" << host << " port=" << port
                            << " url=" << cfg.url << " mqtt_username=" << cfg.username << " password_present=" << (!cfg.password.empty())
                            << " client_id=" << cfg.client_id;

    OrcaMqttConnection* conn = nullptr;
    CurrentConn previous_connection;
    {
        std::lock_guard<std::mutex> l(state_mutex);
        previous_connection  = m_current_connection;
        m_lan_dev_id         = dev_id;
        m_lan_url            = cfg.url;
        m_camera_stream_mode = CameraStreamMode::none;
        m_camera_url.clear();
        m_current_connection = LAN;
        lan_mqtt_connection  = std::make_unique<OrcaMqttConnection>();
        conn                 = lan_mqtt_connection.get();
    }
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: selected LAN printer dev_id=" << dev_id
                            << " transport=" << connection_type_name(previous_connection) << "->LAN";

    if (m_lan_connect_thread.joinable())
        m_lan_connect_thread.join(); // disconnect_printer() above already stopped the old conn, so this is fast
    m_lan_connect_thread = std::thread([this, conn, cfg, dev_id, gen] {
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN connect worker started generation=" << gen << " dev_id=" << dev_id
                                << " url=" << cfg.url;
        if (gen != m_lan_generation.load()) {
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN connect worker abandoned before start generation=" << gen
                                    << " current_generation=" << m_lan_generation.load();
            return; // superseded before we ran: never raise a socket nobody will tear down
        }
        const bool ok = conn->start(cfg, make_lan_message_handler(gen), [this, gen, dev_id, conn](bool connected, bool initial) {
            BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN MQTT state callback connected=" << connected << " initial=" << initial
                                    << " generation=" << gen << " current_generation=" << m_lan_generation.load()
                                    << " connack_rc=" << conn->last_connack_rc();
            if (gen != m_lan_generation.load()) {
                BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: ignoring stale LAN MQTT state callback generation=" << gen;
                return;
            }
            if (connected && !initial) {
                on_connected(dev_id, conn, gen);
                dispatch_local_connect(ConnectStatusOk, dev_id, "0");
            } else if (!connected && !initial) {
                dispatch_local_connect(ConnectStatusLost, dev_id, "connection_lost");
            }
        });
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN MQTT start returned ok=" << ok << " generation=" << gen
                                << " current_generation=" << m_lan_generation.load() << " connected=" << conn->is_connected()
                                << " running=" << conn->is_running() << " connack_rc=" << conn->last_connack_rc();
        if (ok && gen == m_lan_generation.load()) {
            on_connected(dev_id, conn, gen);
            dispatch_local_connect(ConnectStatusOk, dev_id, "0");
        } else if (!ok && gen == m_lan_generation.load() && !conn->is_running()) {
            // A refusal with rc 4/5 terminates the transport. Network errors keep
            // retrying in OrcaMqttConnection, so leave the UI in its connecting state.
            const int rc             = conn->last_connack_rc();
            const std::string reason = rc >= 0 ? std::to_string(rc) : "initial_connect_failed";
            BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: LAN MQTT connection terminated before readiness"
                                       << " generation=" << gen << " connack_rc=" << rc << " reason=" << reason;
            dispatch_local_connect(ConnectStatusFailed, dev_id, reason);
        } else if (!ok && gen == m_lan_generation.load()) {
            BOOST_LOG_TRIVIAL(warning) << "Orca diagnostic: LAN MQTT initial attempt failed but worker is retrying"
                                       << " generation=" << gen << " connack_rc=" << conn->last_connack_rc();
        }
        BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN connect worker exiting generation=" << gen
                                << " current_generation=" << m_lan_generation.load();
    });

    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::disconnect_printer()
{
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: disconnect_printer requested";
    ++m_lan_generation; // fence stale worker callbacks
    std::unique_ptr<OrcaMqttConnection> doomed;
    std::string prev_dev;
    CurrentConn previous_connection;
    CurrentConn current_connection;
    {
        std::lock_guard<std::mutex> l(state_mutex);
        previous_connection = m_current_connection;
        doomed              = std::move(lan_mqtt_connection);
        prev_dev            = m_lan_dev_id;
        m_lan_dev_id.clear();
        if (m_current_connection == LAN) {
            m_current_connection = NONE;
            m_camera_stream_mode = CameraStreamMode::none;
            m_camera_url.clear();
        }
        current_connection = m_current_connection;
    }
    BOOST_LOG_TRIVIAL(info) << "Orca diagnostic: LAN disconnect generation=" << m_lan_generation.load() << " previous_dev_id=" << prev_dev
                            << " had_connection=" << (doomed ? "yes" : "no")
                            << " connected=" << (doomed && doomed->is_connected() ? "yes" : "no")
                            << " transport=" << connection_type_name(previous_connection) << "->"
                            << connection_type_name(current_connection);
    // Tell the printer to stop pushing and drop the report topic before the socket
    // goes away (§3.2/§5.4: deselect issues pushing.stop on both transports).
    if (doomed && !prev_dev.empty() && doomed->is_connected()) {
        doomed->send_request(prev_dev, build_pushing_stop(seq(5)));
        doomed->unsubscribe(prev_dev);
    }
    if (doomed)
        doomed->stop(); // joins the OrcaMqttConnection worker; OUTSIDE state_mutex
    if (m_lan_connect_thread.joinable())
        m_lan_connect_thread.join(); // start() has returned (doomed->stop above); the thread's raw conn ptr
                                     // is still valid here because `doomed` is not destroyed until we return
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{ return route_send(/*is_lan=*/true, dev_id, json_str); }

int OrcaPrinterAgent::route_send(bool is_lan, const std::string& dev_id, const std::string& json_str)
{
    std::string command = "<unparsed>";
    try {
        const nlohmann::json envelope = nlohmann::json::parse(json_str);
        for (const char* namespace_name : {"pushing", "info", "print", "system", "camera", "xcam", "upgrade", "event", "files"}) {
            const auto namespace_it = envelope.find(namespace_name);
            if (namespace_it != envelope.end() && namespace_it->is_object()) {
                const auto command_it = namespace_it->find("command");
                if (command_it != namespace_it->end() && command_it->is_string()) {
                    command = std::string(namespace_name) + "." + command_it->get<std::string>();
                    break;
                }
            }
        }
    } catch (const std::exception&) {
        // Preserve the transport's existing behavior for malformed payloads;
        // the printer will report the protocol error asynchronously.
    }
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::route_send is_lan=" << is_lan << " dev_id=" << dev_id << " command=" << command
                            << " payload_bytes=" << json_str.size();
    if (dev_id.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    OrcaMqttConnection* conn = get_appropriate_mqtt_connection(is_lan);
    if (!conn)
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    const bool queued = conn->send_request(dev_id, json_str);
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::route_send command=" << command << " queued=" << queued << " is_lan=" << is_lan
                            << " dev_id=" << dev_id;
    return queued ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED;
}

// ============================================================================
// Certificates - All Stubs
// ============================================================================

int OrcaPrinterAgent::check_cert() { return BAMBU_NETWORK_SUCCESS; }

void OrcaPrinterAgent::install_device_cert(std::string dev_id, bool lan_only) {}

// ============================================================================
// Discovery
// ============================================================================

bool OrcaPrinterAgent::start_discovery(bool start, bool /*sending*/)
{
    if (start) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!m_discovery) {
            m_discovery = std::make_unique<OrcaSonarDiscovery>([this](const std::string& machine_alive) {
                OnMsgArrivedFn ssdp_fn;
                QueueOnMainFn queue_fn;
                {
                    std::lock_guard<std::mutex> callback_lock(state_mutex);
                    ssdp_fn  = on_ssdp_msg_fn;
                    queue_fn = queue_on_main_fn;
                }
                if (!ssdp_fn)
                    return;
                if (queue_fn)
                    queue_fn([ssdp_fn, machine_alive] { ssdp_fn(machine_alive); });
                else
                    ssdp_fn(machine_alive);
            });
        }
        m_discovery->start();
        return true;
    }

    // The discovery thread invokes the callback, which takes state_mutex. Move the
    // owner out first, then join without holding that mutex.
    std::unique_ptr<OrcaSonarDiscovery> discovery;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        discovery = std::move(m_discovery);
    }
    if (discovery)
        discovery->stop();
    return true;
}

// ============================================================================
// Binding
// ============================================================================

int OrcaPrinterAgent::ping_bind(std::string ping_code) { return BAMBU_NETWORK_SUCCESS; }

// Runs on the "Input IP address" dialog worker thread, before any MachineObject
// exists. Probe the address for a live OrcaSonar and hand its real device id back
// so DeviceManager::insert_local_device keys the machine correctly; connect_type
// and bind_state must be set for is_lan_mode_printer()/is_avaliable() to hold, or
// set_selected_machine never routes to the LAN connect path.
int OrcaPrinterAgent::bind_detect(std::string dev_ip, std::string /*sec_link*/, detectResult& detect)
{
    std::string host, port;
    if (!parse_lan_endpoint(dev_ip, host, port)) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent::bind_detect: unparsable dev_ip=" << dev_ip;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE; // -1: dialog shows "Failed to connect to printer."
    }

    std::string device_id;
    std::string device_name;
    std::string model_id;
    if (!probe_orcasonar_landing_page(host, port, device_id, device_name, model_id) || device_id.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent::bind_detect: no OrcaSonar reachable at " << host << ":" << port;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    detect.dev_id       = device_id;
    detect.dev_name     = device_name;
    detect.model_id     = model_id;
    detect.version      = "";
    detect.connect_type = "lan";  // required by MachineObject::is_lan_mode_printer()
    detect.bind_state   = "free"; // required by MachineObject::is_avaliable()
    detect.result_msg   = "";
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::bind_detect: found OrcaSonar dev_id=" << device_id << " at " << host << ":" << port;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::bind(std::string dev_ip,
                           std::string dev_id,
                           std::string dev_model,
                           std::string sec_link,
                           std::string timezone,
                           bool improved,
                           OnUpdateStatusFn update_fn)
{ return BAMBU_NETWORK_SUCCESS; }

int OrcaPrinterAgent::unbind(std::string dev_id) { return BAMBU_NETWORK_SUCCESS; }

int OrcaPrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket)
        *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback)
{
    // No BBL cloud snapshot source; report failure so the caller falls back.
    (void) dev_id;
    (void) file_name;
    (void) callback;
    return -1;
}

int OrcaPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string OrcaPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return selected_machine;
}

int OrcaPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    auto* cloud = get_orca_cloud_agent();
    std::string previous;
    CurrentConn previous_connection;
    CurrentConn current_connection;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        previous_connection = m_current_connection;
        // An empty cloud selection must not clear an independently active LAN
        // selection. Conversely, selecting a cloud machine with the same id
        // while LAN is active is still a transport switch and must proceed.
        const bool same_selection = dev_id == selected_machine;
        const bool same_transport = dev_id.empty() ? m_current_connection != CLOUD : m_current_connection == CLOUD;
        if (same_selection && same_transport) {
            BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_user_selected_machine: unchanged dev_id=" << dev_id
                                    << " transport=" << connection_type_name(m_current_connection);
            return BAMBU_NETWORK_SUCCESS;
        }
        previous         = selected_machine;
        selected_machine = dev_id;
        if (dev_id.empty()) {
            if (m_current_connection == CLOUD) {
                m_current_connection = NONE;
                m_camera_stream_mode = CameraStreamMode::none;
                m_camera_url.clear();
            }
        } else {
            m_current_connection = CLOUD;
            m_camera_stream_mode = CameraStreamMode::none;
            m_camera_url.clear();
        }
        current_connection = m_current_connection;
    }
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_user_selected_machine: previous=" << previous << " new=" << dev_id
                            << " cloud=" << (cloud ? "set" : "<null>") << " transport=" << connection_type_name(previous_connection) << "->"
                            << connection_type_name(current_connection);
    if (!cloud) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent::set_user_selected_machine: no Orca cloud agent";
        return BAMBU_NETWORK_SUCCESS;
    }

    // Bump ONCE at the top for any change (select or deselect) so a deselect also
    // fences an in-flight configure thread started by the previous selection. This is
    // the CLOUD epoch only — a cloud selection must not fence a live LAN session.
    const uint64_t gen = ++m_cloud_generation;

    auto* conn = cloud->get_mqtt_connection();
    if (!previous.empty()) {
        cloud->del_subscribe({previous});
        if (conn && conn->is_connected())
            conn->send_request(previous, build_pushing_stop(seq(5)));
    }

    // A previous initial-connect worker may still be blocking in start(). Stop it
    // only when no fleet connection has reached CONNACK; an established fleet
    // socket survives printer selection changes.
    if (m_cloud_connect_thread.joinable()) {
        if (conn && !conn->is_connected())
            conn->stop();
        m_cloud_connect_thread.join();
    }

    if (dev_id.empty())
        return BAMBU_NETWORK_SUCCESS;

    if (conn && conn->is_running()) {
        on_connected(dev_id, conn, gen);
        return BAMBU_NETWORK_SUCCESS;
    }

    m_cloud_connect_thread = std::thread([this, cloud, dev_id, gen] {
        if (gen != m_cloud_generation.load())
            return; // superseded before we ran: do not raise a socket nobody owns

        auto state_handler = [this](bool connected, bool initial) {
            if (!connected || initial)
                return;
            auto* current_cloud = get_orca_cloud_agent();
            OrcaMqttConnection* current_conn = current_cloud ? current_cloud->get_mqtt_connection() : nullptr;
            const std::string selected = get_user_selected_machine();
            if (current_conn && !selected.empty())
                on_connected(selected, current_conn, m_cloud_generation.load());
        };

        if (cloud->configure_selected_printer_mqtt(dev_id, std::move(state_handler)) == BAMBU_NETWORK_SUCCESS &&
            gen == m_cloud_generation.load()) {
            on_connected(dev_id, cloud->get_mqtt_connection(), gen);
        }
    });
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Agent Information
// ============================================================================
AgentInfo OrcaPrinterAgent::get_agent_info_static()
{ return AgentInfo{ORCA_PRINTER_AGENT_ID, "Orca", OrcaPrinterAgent_VERSION, "Orca Printer Communication Protocol Agent"}; }

// ============================================================================
// Print Job Operations - All Stubs
// ============================================================================

// Simply uploads the file to the printer via HTTP (cloud) then sends a HTTP request to start print. In the future, this might be a MQTT command to start print instead of HTTP.
int OrcaPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    (void) wait_fn;

    if (update_fn)
        update_fn(PrintingStageCreate, 0, "Preparing...");

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    auto* cloud = get_orca_cloud_agent();
    if (!cloud || params.dev_id.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    const std::string local_path = resolve_local_gcode_path(params);
    boost::system::error_code ec;
    if (!fs::exists(local_path, ec) || !fs::is_regular_file(local_path, ec)) {
        BOOST_LOG_TRIVIAL(error) << "OrcaPrinterAgent: G-code file does not exist: " << local_path;
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Uploading G-code...");

    std::string job_id;
    int result = cloud->upload_gcode_via_cloud(params.dev_id, local_path, &job_id, update_fn, cancel_fn);
    if (result != BAMBU_NETWORK_SUCCESS)
        return result;

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    if (update_fn)
        update_fn(PrintingStageSending, 0, "Starting print...");

    const int start_rc = cloud->start_cloud_print_job(params.dev_id, job_id, remote_gcode_name(params), /*start=*/true);
    if (start_rc != BAMBU_NETWORK_SUCCESS)
        return start_rc;

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "Print started");

    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::start_local_print_with_record(PrintParams params,
                                                    OnUpdateStatusFn update_fn,
                                                    WasCancelledFn cancel_fn,
                                                    OnWaitFn wait_fn)
{ return BAMBU_NETWORK_SUCCESS; }

// Upload one G-code file to the printer's `gcodes` root over OrcaSonar's
// Moonraker-compatible HTTP facade. No print is started here (print=false); the
// caller issues print.gcode_file over MQTT separately (start_sdcard_print).
int OrcaPrinterAgent::start_send_gcode_to_sdcard(PrintParams params,
                                                 OnUpdateStatusFn update_fn,
                                                 WasCancelledFn cancel_fn,
                                                 OnWaitFn /*wait_fn*/)
{
    if (update_fn)
        update_fn(PrintingStageCreate, 0, "Preparing...");

    const std::string local_path = resolve_local_gcode_path(params);
    const fs::path source(local_path);
    boost::system::error_code ec;
    if (!fs::exists(source, ec) || !fs::is_regular_file(source, ec)) {
        BOOST_LOG_TRIVIAL(error) << "OrcaPrinterAgent: G-code file does not exist: " << local_path;
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    const std::uintmax_t file_size = fs::file_size(source, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "OrcaPrinterAgent: cannot stat G-code file " << local_path << ": " << ec.message();
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }
    if (file_size > 1024ull * 1024 * 1024) { // OrcaSonar caps a single upload at 1 GiB
        BOOST_LOG_TRIVIAL(error) << "OrcaPrinterAgent: G-code file too large: " << file_size << " bytes";
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    std::string host, port, origin;
    if (parse_lan_endpoint(params.dev_ip, host, port))
        origin = "http://" + host;
    else
        origin = http_origin_from_lan_ws(lan_connection_target());
    if (origin.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: no LAN HTTP endpoint for G-code upload (dev_ip=" << params.dev_ip << ")";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    const std::string upload_name = remote_gcode_name(params);
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: uploading G-code " << local_path << " -> " << origin << "/server/files/upload as "
                            << upload_name << " (" << file_size << " bytes)";

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Uploading...");

    bool canceled    = false;
    long http_status = 0;
    std::string http_error;
    std::string response_body;

    // check if printer has enough storage
    Http::get(origin + "/server/files/directory?path=gcodes")
        .on_complete([&](std::string body, unsigned status) {
            if (body.empty()) {
                http_status = 400;
                http_error  = "Failed to get gcodes directory.";
            }

            int free = 0;

            nlohmann::json json = nlohmann::json::parse(body);
            if (json.contains("result")) {
                json = json["result"];
                if (json.contains("disk_usage")) {
                    json = json["disk_usage"];
                    if (json.contains("free"))
                        free = json["free"].get<int>();
                }
            }

            if (free < file_size) {
                http_status = 507;
                http_error  = "Not enough storage on the printer.";
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_status   = status;
            http_error    = std::move(err);
            response_body = std::move(body);
        })
        .perform_sync();

    if (http_status >= 400) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " failed with error code: " << http_status << ", " << http_error;
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    auto http = Http::post(origin + "/server/files/upload");
    if (!params.password.empty())
        http.header("X-Api-Key", params.password); // trusted LAN facades may not require it; harmless when they do not
    http.form_add("root", "gcodes")
        .form_add("print", "false")
        .form_add_file("file", source, upload_name)
        .timeout_connect(5)
        .timeout_max(300) // large G-code over a slow link
        .on_complete([&](std::string body, unsigned status) {
            http_status   = status;
            response_body = std::move(body);
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_status   = status;
            http_error    = std::move(err);
            response_body = std::move(body);
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            if (cancel_fn && cancel_fn()) {
                cancel   = true;
                canceled = true;
                return;
            }
            if (update_fn && progress.ultotal > 0) {
                const int percent = static_cast<int>((progress.ulnow * 100) / progress.ultotal);
                update_fn(PrintingStageUpload, percent, "Uploading...");
            }
        })
        .perform_sync();

    if (canceled) {
        BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent: G-code upload canceled by user";
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    // OrcaSonar's Moonraker facade returns 201 Created on a successful save.
    if (http_status != 200 && http_status != 201) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: G-code upload failed http_status=" << http_status << " error=" << http_error
                                   << " body=" << response_body;
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    if (update_fn)
        update_fn(PrintingStageUpload, 100, "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}

// Upload the sliced G-code, then start it: the LAN "print now" path.
int OrcaPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    const int upload_rc = start_send_gcode_to_sdcard(params, update_fn, cancel_fn, nullptr);
    if (upload_rc != BAMBU_NETWORK_SUCCESS)
        return upload_rc;

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    return start_sdcard_print(params, update_fn, cancel_fn);
}

// Start a file that already lives on the printer by publishing the canonical
// OPCP print.gcode_file command to device/<dev_id>/request. The acknowledgement
// and lifecycle progress arrive asynchronously as print.push_status on the
// report topic, which the GUI already consumes.
int OrcaPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (params.dev_id.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: start_sdcard_print rejected missing dev_id";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    // dst_file, when set, names a file already on the printer (print-from-SD flow);
    // otherwise start what start_send_gcode_to_sdcard just uploaded to `gcodes`.
    const std::string target = params.dst_file.empty() ? remote_gcode_name(params) : fs::path(params.dst_file).filename().string();

    nlohmann::json j;
    j["print"]["command"]     = "gcode_file";
    j["print"]["sequence_id"] = next_gcode_file_sequence_id();
    j["print"]["param"]       = target;

    if (update_fn)
        update_fn(PrintingStageSending, 0, "Starting print...");

    const bool is_lan = params.connection_type == "lan";
    const int rc      = route_send(is_lan, params.dev_id, j.dump());
    if (rc != BAMBU_NETWORK_SUCCESS) {
        BOOST_LOG_TRIVIAL(warning) << "OrcaPrinterAgent: start_sdcard_print publish failed rc=" << rc << " dev_id=" << params.dev_id
                                   << " param=" << target;
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    if (update_fn)
        update_fn(PrintingStageFinished, 100, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Callback Registration
// ============================================================================

int OrcaPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_ssdp_msg_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_message_fn = fn;
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_on_message_fn: callback=" << (fn ? "set" : "clear");
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_connect_fn = fn;
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_on_local_connect_fn: callback=" << (fn ? "set" : "clear");
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    on_local_message_fn = fn;
    BOOST_LOG_TRIVIAL(info) << "OrcaPrinterAgent::set_on_local_message_fn: callback=" << (fn ? "set" : "clear");
    return BAMBU_NETWORK_SUCCESS;
}

int OrcaPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

CameraStreamMode OrcaPrinterAgent::get_camera_stream_mode() const
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (m_current_connection == CLOUD)
        return CameraStreamMode::webrtc;
    return m_camera_stream_mode;
}

std::string OrcaPrinterAgent::get_camera_url() const
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (m_current_connection != LAN)
        return {};
    return m_camera_url;
}

} // namespace Slic3r
