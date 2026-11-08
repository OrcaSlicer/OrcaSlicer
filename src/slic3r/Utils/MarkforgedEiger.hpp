#pragma once

// Markforged printers through the Eiger cloud API (https://www.eiger.io/api/v3, HTTP basic auth with an
// access key and secret key). Eiger slices on its side and takes STL only, so a job uploads the plate as an
// STL with an Eiger settings preset, waits for Eiger to slice it, finds the build Eiger made for it (the
// API cannot create builds) and starts, queues or backlogs that build.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

struct EigerKeys
{
    std::string access;
    std::string secret;
    bool        empty() const { return access.empty() || secret.empty(); }
};

class Eiger
{
public:
    explicit Eiger(EigerKeys keys) : m_keys(std::move(keys)) {}

    // Keys from the printer preset (printhost_user / printhost_password, or printhost_apikey "ACCESS:SECRET"),
    // else from <data_dir>/orca_plugins/markforged_eiger_data/settings.json, which the Markforged agent shares.
    static EigerKeys            keys_for(const DynamicPrintConfig* printer_config);
    static void                 save_keys(const EigerKeys& keys);
    static std::string          saved_settings_preset();
    static boost::filesystem::path settings_file();

    // All throw std::runtime_error. Blocking.
    std::vector<nlohmann::json>                      devices();
    nlohmann::json                                   device(const std::string& id);
    std::vector<std::pair<std::string, std::string>> settings_presets(); // (id, title)
    nlohmann::json upload_stl(const boost::filesystem::path& stl, const std::string& title, const std::string& preset_id);
    std::string    slice_status(const std::string& slice_job_id);
    std::vector<nlohmann::json> builds_titled(const std::string& title);
    void                        post(const std::string& path, const nlohmann::json& body);

private:
    nlohmann::json get(const std::string& path_and_query);
    EigerKeys      m_keys;
};

// Runs the Eiger job; the target device and choices travel in PrintHostUpload::extended_info
// ("eiger_device", "eiger_action" now|queue|backlog, "eiger_preset", "eiger_title"), the STL in source_path.
class MarkforgedEigerHost : public PrintHost
{
public:
    explicit MarkforgedEigerHost(EigerKeys keys) : m_keys(std::move(keys)) {}

    const char*                get_name() const override { return "Markforged Eiger"; }
    bool                       test(wxString& msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return false; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return "www.eiger.io"; }
    bool                       is_cloud() const override { return true; }

private:
    EigerKeys m_keys;
};

} // namespace Slic3r
