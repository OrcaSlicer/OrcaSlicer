// OpenBambuPrinterAgent - Plugin-free LAN printing for BambuLab printers.
//
// This agent enables direct LAN communication with BambuLab printers
// WITHOUT requiring the proprietary network plugin. It implements the
// IPrinterAgent interface using open protocols.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//   https://github.com/acse-ci223/bambulabs_api
//   https://github.com/greghesp/ha-bambulab
//
// LAN print flow (mirrors the stock plugin behavior):
//   1. Upload .3mf to printer via FTPS (port 990)
//   2. Publish project_file MQTT command to device/{serial}/request
//   3. Receive status updates on device/{serial}/report

#include "OpenBambuPrinterAgent.hpp"
#include "OpenBambuMqtt.hpp"
#include "OpenBambuSsdp.hpp"
#include "OpenBambuFtps.hpp"

#include <boost/log/trivial.hpp>

#include <openssl/evp.h>

#include <boost/filesystem.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Slic3r {

namespace {

constexpr const char* OPEN_BAMBU_AGENT_ID = "open_bambu";
constexpr const char* OPEN_BAMBU_VERSION  = "1.0.0";

// Compute uppercase hex MD5 of a file (for the print command's md5 field).
// The printer cross-checks the uploaded file against this hash.
std::string md5_of_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};

    std::unique_ptr<EVP_MD_CTX, void(*)(EVP_MD_CTX*)> ctx(
        EVP_MD_CTX_new(),
        [](EVP_MD_CTX* c){ if (c) EVP_MD_CTX_free(c); });
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr) != 1)
        return {};

    char buf[64 * 1024];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        if (EVP_DigestUpdate(ctx.get(), buf, static_cast<size_t>(f.gcount())) != 1)
            return {};
    }

    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &len) != 1) return {};

    static const char kHex[] = "0123456789ABCDEF";
    std::string hex(len * 2, '\0');
    for (unsigned i = 0; i < len; ++i) {
        hex[2 * i    ] = kHex[(digest[i] >> 4) & 0xF];
        hex[2 * i + 1] = kHex[ digest[i]       & 0xF];
    }
    return hex;
}

std::string json_escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string to_bool_str(bool v) { return v ? "true" : "false"; }

std::string now_seq_id()
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(ms);
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

OpenBambuPrinterAgent::OpenBambuPrinterAgent(std::string log_dir)
    : m_log_dir(std::move(log_dir))
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuPrinterAgent: created (plugin-free BambuLab LAN agent)";
}

OpenBambuPrinterAgent::~OpenBambuPrinterAgent()
{
    disconnect_printer();
    if (m_ssdp) m_ssdp->stop();
}

void OpenBambuPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_cloud_agent = cloud;
}

// ============================================================================
// Agent Info
// ============================================================================

AgentInfo OpenBambuPrinterAgent::get_agent_info_static()
{
    return AgentInfo{
        OPEN_BAMBU_AGENT_ID,
        "Open Bambu (LAN)",
        OPEN_BAMBU_VERSION,
        "Plugin-free BambuLab LAN printing (based on open-bamboo-networking protocols)"
    };
}

// ============================================================================
// Communication
// ============================================================================

int OpenBambuPrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int /*flag*/)
{
    // In LAN-only mode, send_message goes through the local MQTT connection
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, 0);
}

int OpenBambuPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip,
                                            std::string username, std::string password, bool use_ssl)
{
    BOOST_LOG_TRIVIAL(info) << "OpenBambuPrinterAgent: connecting to " << dev_id
                            << " at " << dev_ip << " (ssl=" << use_ssl << ")";

    std::unique_ptr<OpenBambuMqtt> mqtt;
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // Disconnect any existing connection
        if (m_mqtt && m_mqtt->is_connected()) {
            m_mqtt->disconnect();
        }

        mqtt = std::make_unique<OpenBambuMqtt>();
        m_connected_dev_id = dev_id;
    }

    // Setup callbacks (these will be called from the MQTT read thread or connect thread)
    mqtt->set_on_connect([this, dev_id](int rc) {
        if (rc == 0) {
            BOOST_LOG_TRIVIAL(info) << "OpenBambuPrinterAgent: MQTT connected to " << dev_id;

            OnLocalConnectedFn cb;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                cb = m_on_local_connect_fn;
            }
            if (cb) cb(0, dev_id, "connected");
        } else {
            OnLocalConnectedFn cb;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                cb = m_on_local_connect_fn;
            }
            if (cb) cb(BAMBU_NETWORK_ERR_CONNECT_FAILED, dev_id, "connect failed");
        }
    });

    mqtt->set_on_disconnect([this, dev_id](int rc) {
        OnLocalConnectedFn cb;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            cb = m_on_local_connect_fn;
        }
        if (cb) cb(rc == 0 ? 0 : BAMBU_NETWORK_ERR_DISCONNECT_FAILED, dev_id, "disconnected");
    });

    mqtt->set_on_message([this](const std::string& /*topic*/, const std::string& payload) {
        OnMessageFn cb;
        std::string dev_id;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            cb = m_on_local_message_fn;
            dev_id = m_connected_dev_id;
        }
        if (cb) cb(dev_id, payload);
    });

    // BambuLab LAN MQTT: port 8883 with TLS, user "bblp", password = access code
    MqttConnectConfig cfg;
    cfg.host         = dev_ip;
    cfg.port         = use_ssl ? 8883 : 1883;
    cfg.username     = username.empty() ? "bblp" : username;
    cfg.password     = password;
    cfg.use_tls      = use_ssl;
    cfg.tls_insecure = true;  // LAN mode: skip cert verification
    cfg.keepalive_s  = 60;

    int rc = mqtt->connect(cfg);
    if (rc != 0) {
        BOOST_LOG_TRIVIAL(error) << "OpenBambuPrinterAgent: MQTT connect failed, rc=" << rc;
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }

    // Subscribe to the printer's report topic
    std::string report_topic = "device/" + dev_id + "/report";
    mqtt->subscribe(report_topic, 0);

    // Store the connected mqtt client
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_mqtt = std::move(mqtt);
    }

    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::disconnect_printer()
{
    std::unique_ptr<OpenBambuMqtt> mqtt;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        mqtt = std::move(m_mqtt);
        m_connected_dev_id.clear();
    }
    if (mqtt) {
        mqtt->disconnect();
    }
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int /*flag*/)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_mqtt || !m_mqtt->is_connected()) {
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    std::string topic = "device/" + dev_id + "/request";
    int rc = m_mqtt->publish(topic, json_str, qos);
    return rc == 0 ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

// ============================================================================
// Certificates
// ============================================================================

int OpenBambuPrinterAgent::check_cert()
{
    // In LAN mode with tls_insecure=true, no cert management needed
    return BAMBU_NETWORK_SUCCESS;
}

void OpenBambuPrinterAgent::install_device_cert(std::string /*dev_id*/, bool /*lan_only*/)
{
    // No-op in LAN mode with insecure TLS
}

// ============================================================================
// Discovery
// ============================================================================

bool OpenBambuPrinterAgent::start_discovery(bool start, bool /*sending*/)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    if (start) {
        if (!m_ssdp) {
            m_ssdp = std::make_unique<OpenBambuSsdp>();
        }
        if (!m_ssdp->is_running()) {
            OnMsgArrivedFn cb = m_on_ssdp_msg_fn;
            return m_ssdp->start([cb](std::string json) {
                if (cb) cb(json);
            });
        }
        return true;
    } else {
        if (m_ssdp) {
            m_ssdp->stop();
        }
        return true;
    }
}

// ============================================================================
// Binding (limited in LAN-only mode)
// ============================================================================

int OpenBambuPrinterAgent::ping_bind(std::string /*ping_code*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::bind_detect(std::string /*dev_ip*/, std::string /*sec_link*/, detectResult& /*detect*/)
{
    // Binding requires cloud - not available in pure LAN mode
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

int OpenBambuPrinterAgent::bind(std::string /*dev_ip*/, std::string /*dev_id*/,
                                 std::string /*sec_link*/, std::string /*timezone*/,
                                 bool /*improved*/, OnUpdateStatusFn /*update_fn*/)
{
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

int OpenBambuPrinterAgent::unbind(std::string /*dev_id*/)
{
    return BAMBU_NETWORK_ERR_UNBIND_FAILED;
}

int OpenBambuPrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket) *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string OpenBambuPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_selected_machine;
}

int OpenBambuPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Print Job Operations
// ============================================================================

std::string OpenBambuPrinterAgent::build_print_command_json(
    const PrintParams& params, const std::string& remote_filename, const std::string& md5)
{
    // Build the project_file MQTT command matching BambuLab firmware expectations.
    // Format derived from open-bamboo-networking's print_job.cpp.
    std::string plate_param = "Metadata/plate_" +
        std::to_string(params.plate_index <= 0 ? 1 : params.plate_index) + ".gcode";
    std::string subtask = params.project_name.empty() ? params.task_name : params.project_name;
    std::string bed_type = params.task_bed_type.empty() ? "auto" : params.task_bed_type;

    // AMS mapping
    std::string ams_mapping = params.ams_mapping;
    if (ams_mapping.empty()) {
        ams_mapping = params.task_use_ams ? "[0]" : "[]";
    } else if (ams_mapping.front() != '[') {
        ams_mapping = "[" + ams_mapping + "]";
    }

    std::ostringstream os;
    os << "{\"print\":{";
    os << "\"sequence_id\":" << json_escape(now_seq_id());
    os << ",\"command\":\"project_file\"";
    os << ",\"param\":" << json_escape(plate_param);
    os << ",\"project_id\":\"0\"";
    os << ",\"profile_id\":\"0\"";
    os << ",\"task_id\":\"0\"";
    os << ",\"subtask_id\":\"0\"";
    os << ",\"subtask_name\":" << json_escape(subtask);
    os << ",\"file\":" << json_escape(remote_filename);
    os << ",\"url\":\"ftp://" << remote_filename << "\"";
    os << ",\"md5\":" << json_escape(md5);
    os << ",\"bed_type\":" << json_escape(bed_type);
    os << ",\"bed_leveling\":" << to_bool_str(params.task_bed_leveling);
    os << ",\"flow_cali\":" << to_bool_str(params.task_flow_cali);
    os << ",\"vibration_cali\":" << to_bool_str(params.task_vibration_cali);
    os << ",\"layer_inspect\":" << to_bool_str(params.task_layer_inspect);
    os << ",\"timelapse\":" << to_bool_str(params.task_record_timelapse);
    os << ",\"use_ams\":" << to_bool_str(params.task_use_ams);
    os << ",\"ams_mapping\":" << ams_mapping;

    if (params.ams_mapping2.empty())
        os << ",\"ams_mapping2\":[]";
    else
        os << ",\"ams_mapping2\":" << params.ams_mapping2;

    if (!params.nozzle_mapping.empty())
        os << ",\"nozzle_mapping\":" << params.nozzle_mapping;

    os << "}}";
    return os.str();
}

int OpenBambuPrinterAgent::do_local_print(PrintParams& params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // Stage 1: Create
    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageCreate, 0, "Starting local print");
    if (cancel_fn && cancel_fn()) return BAMBU_NETWORK_ERR_CANCELED;

    // Validate file exists
    boost::system::error_code ec;
    auto file_size = boost::filesystem::file_size(params.filename, ec);
    if (ec) {
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                 BAMBU_NETWORK_ERR_FILE_NOT_EXIST, "File not found");
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    // Check size limit (1 GB)
    if (file_size > 1024ULL * 1024 * 1024) {
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                 BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE, "File over 1 GB");
        return BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE;
    }

    // Compute MD5
    std::string md5 = md5_of_file(params.filename);
    if (md5.empty()) {
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                 BAMBU_NETWORK_ERR_CHECK_MD5_FAILED, "MD5 computation failed");
        return BAMBU_NETWORK_ERR_CHECK_MD5_FAILED;
    }

    // Determine remote path
    std::string ftp_file = params.ftp_file;
    if (ftp_file.empty()) {
        // Use the filename from the local path
        boost::filesystem::path p(params.filename);
        ftp_file = p.filename().string();
    }
    std::string remote_path = "/" + ftp_file;
    if (!params.ftp_folder.empty()) {
        remote_path = "/" + params.ftp_folder + "/" + ftp_file;
    }

    // Stage 2: Upload via FTPS
    if (cancel_fn && cancel_fn()) return BAMBU_NETWORK_ERR_CANCELED;

    FtpsUploadConfig ftps_cfg;
    ftps_cfg.host         = params.dev_ip;
    ftps_cfg.port         = params.use_ssl_for_ftp ? 990 : 21;
    ftps_cfg.username     = params.username.empty() ? "bblp" : params.username;
    ftps_cfg.password     = params.password;
    ftps_cfg.use_tls      = params.use_ssl_for_ftp;
    ftps_cfg.tls_insecure = true;

    auto progress = [&](uint64_t sent, uint64_t total) -> bool {
        if (cancel_fn && cancel_fn()) return false;
        if (update_fn) {
            int pct = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
            update_fn(SendingPrintJobStage::PrintingStageUpload, pct,
                      std::to_string(pct) + "%");
        }
        return true;
    };

    std::string err = open_bambu_ftps_upload(ftps_cfg, params.filename, remote_path, progress);
    if (!err.empty()) {
        int code = (err == "upload cancelled") ? BAMBU_NETWORK_ERR_CANCELED
                                                : BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR, code, err);
        return code;
    }

    // Stage 3: Send print command via MQTT
    if (cancel_fn && cancel_fn()) return BAMBU_NETWORK_ERR_CANCELED;
    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageSending, 0, "Sending print command");

    // Strip leading slash for the MQTT command (firmware expects bare filename)
    std::string mqtt_filename = remote_path;
    if (!mqtt_filename.empty() && mqtt_filename.front() == '/') {
        mqtt_filename = mqtt_filename.substr(1);
    }

    std::string json_cmd = build_print_command_json(params, mqtt_filename, md5);

    std::string topic = "device/" + params.dev_id + "/request";
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_mqtt || !m_mqtt->is_connected()) {
            if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                     BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED,
                                     "MQTT not connected");
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
        int rc = m_mqtt->publish(topic, json_cmd, 0);
        if (rc != 0) {
            if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                     BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED,
                                     "Failed to send MQTT command");
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
    }

    // Stage 4: Done
    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageFinished, 0, "Print job sent");
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn,
                                        WasCancelledFn cancel_fn, OnWaitFn /*wait_fn*/)
{
    // In LAN-only mode, start_print is the same as start_local_print
    return do_local_print(params, update_fn, cancel_fn);
}

int OpenBambuPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn,
                                                          WasCancelledFn cancel_fn, OnWaitFn /*wait_fn*/)
{
    // Without cloud, we can't record - just do a local print
    return do_local_print(params, update_fn, cancel_fn);
}

int OpenBambuPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn,
                                                       WasCancelledFn cancel_fn, OnWaitFn /*wait_fn*/)
{
    // Upload file only, don't start printing
    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageCreate, 0, "Starting upload");
    if (cancel_fn && cancel_fn()) return BAMBU_NETWORK_ERR_CANCELED;

    boost::system::error_code ec;
    auto file_size = boost::filesystem::file_size(params.filename, ec);
    if (ec) {
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                 BAMBU_NETWORK_ERR_FILE_NOT_EXIST, "File not found");
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }
    if (file_size > 1024ULL * 1024 * 1024) {
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                 BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE, "File over 1 GB");
        return BAMBU_NETWORK_ERR_PRINT_LP_FILE_OVER_SIZE;
    }

    std::string ftp_file = params.ftp_file.empty()
        ? boost::filesystem::path(params.filename).filename().string()
        : params.ftp_file;
    std::string remote_path = "/" + ftp_file;
    if (!params.ftp_folder.empty()) remote_path = "/" + params.ftp_folder + "/" + ftp_file;

    FtpsUploadConfig ftps_cfg;
    ftps_cfg.host         = params.dev_ip;
    ftps_cfg.port         = params.use_ssl_for_ftp ? 990 : 21;
    ftps_cfg.username     = params.username.empty() ? "bblp" : params.username;
    ftps_cfg.password     = params.password;
    ftps_cfg.use_tls      = params.use_ssl_for_ftp;
    ftps_cfg.tls_insecure = true;

    auto progress = [&](uint64_t sent, uint64_t total) -> bool {
        if (cancel_fn && cancel_fn()) return false;
        if (update_fn) {
            int pct = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
            update_fn(SendingPrintJobStage::PrintingStageUpload, pct, std::to_string(pct) + "%");
        }
        return true;
    };

    std::string err = open_bambu_ftps_upload(ftps_cfg, params.filename, remote_path, progress);
    if (!err.empty()) {
        int code = (err == "upload cancelled") ? BAMBU_NETWORK_ERR_CANCELED
                                                : BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR, code, err);
        return code;
    }

    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageFinished, 0, "Upload complete");
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return do_local_print(params, update_fn, cancel_fn);
}

int OpenBambuPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // Send print command for a file already on the SD card
    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageSending, 0, "Starting SD card print");
    if (cancel_fn && cancel_fn()) return BAMBU_NETWORK_ERR_CANCELED;

    std::string filename = params.ftp_file.empty() ? params.dst_file : params.ftp_file;
    if (filename.empty()) {
        if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                 BAMBU_NETWORK_ERR_FILE_NOT_EXIST, "No filename specified");
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    std::string md5 = params.ftp_file_md5.empty() ? "0" : params.ftp_file_md5;
    std::string json_cmd = build_print_command_json(params, filename, md5);

    std::string topic = "device/" + params.dev_id + "/request";
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_mqtt || !m_mqtt->is_connected()) {
            if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                     BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED,
                                     "MQTT not connected");
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
        int rc = m_mqtt->publish(topic, json_cmd, 0);
        if (rc != 0) {
            if (update_fn) update_fn(SendingPrintJobStage::PrintingStageERROR,
                                     BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED,
                                     "Failed to send MQTT command");
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
    }

    if (update_fn) update_fn(SendingPrintJobStage::PrintingStageFinished, 0, "Print started");
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Callback Registration
// ============================================================================

int OpenBambuPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_ssdp_msg_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_local_connect_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int OpenBambuPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Filament
// ============================================================================

FilamentSyncMode OpenBambuPrinterAgent::get_filament_sync_mode() const
{
    // We get filament info via MQTT subscription (same as BBL agent)
    return FilamentSyncMode::subscription;
}

} // namespace Slic3r
