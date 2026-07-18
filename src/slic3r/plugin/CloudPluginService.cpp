#include "CloudPluginService.hpp"

#include "OrcaCloudServiceAgent.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <cstddef>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

void CloudPluginService::set_cloud_agent(std::shared_ptr<OrcaCloudServiceAgent> agent) { m_orca_agent = std::move(agent); }

std::shared_ptr<OrcaCloudServiceAgent> CloudPluginService::get_cloud_agent() const { return m_orca_agent; }

bool CloudPluginService::can_fetch_cloud_plugins() const
{
    if (!m_orca_agent) {
        BOOST_LOG_TRIVIAL(warning) << "Orca service agent is null";
        return false;
    }

    if (!m_orca_agent->is_user_login() || m_orca_agent->get_user_id().empty()) {
        BOOST_LOG_TRIVIAL(info) << "User not logged in, no cloud directory";
        return false;
    }

    return true;
}

bool CloudPluginService::fetch_manifests_into_descriptors(std::vector<PluginDescriptor>& descriptors,
                                                      std::vector<std::string>& not_found,
                                                      std::vector<std::string>& unauthorized) const
{
    descriptors.clear();
    not_found.clear();
    unauthorized.clear();

    if (m_orca_agent) {
        int ret = m_orca_agent->fetch_subscribed_manifests_into_descriptors(descriptors, not_found, unauthorized);

        if (ret != 0) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to get subscribed plugins.";
            return false;
        }

        std::vector<PluginDescriptor> mine_descriptors;

        ret = m_orca_agent->fetch_mine_manifests_into_descriptors(mine_descriptors);

        if (ret != 0) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Failed to get owned plugins.";
            return false;
        }

        descriptors.insert(descriptors.end(), mine_descriptors.begin(), mine_descriptors.end());
    }

    return true;
}

bool CloudPluginService::request_cloud_subscribe(const std::string& plugin_uuid, std::string& error) const
{
    error.clear();
    if (!m_orca_agent) {
        error = "No cloud agent.";
        return false;
    }
    if (plugin_uuid.empty()) {
        error = "Cloud plugin key is missing UUID.";
        return false;
    }

    if (m_orca_agent->subscribe_plugin(plugin_uuid) != 0) {
        error = "Failed to subscribe to cloud plugin, see logs for more info.";
        return false;
    }

    return true;
}

bool CloudPluginService::request_cloud_unsubscribe(const PluginDescriptor& plugin, std::string& error) const
{
    if (!m_orca_agent) {
        error = "No cloud agent.";
        return false;
    }

    if (!plugin.is_cloud_plugin()) {
        error = "Only cloud plugins can be unsubscribed.";
        return false;
    }

    const std::string cloud_uuid = plugin.cloud_uuid();
    if (cloud_uuid.empty()) {
        error = "Cloud plugin key is missing UUID.";
        return false;
    }

    int result = m_orca_agent->unsubscribe_plugins({cloud_uuid});

    if (result != 0) {
        error = "Failed to unsubscribe plugin, see logs for more info.";
        return false;
    }

    return true;
}

bool CloudPluginService::request_cloud_delete(const PluginDescriptor& plugin, std::string& error) const
{
    if (!m_orca_agent) {
        error = "No cloud agent.";
        return false;
    }

    if (!plugin.is_cloud_plugin()) {
        error = "Only cloud plugins can be deleted.";
        return false;
    }

    const std::string cloud_uuid = plugin.cloud_uuid();
    if (cloud_uuid.empty()) {
        error = "Cloud plugin key is missing UUID.";
        return false;
    }

    if (!plugin.cloud.has_value() || !plugin.cloud->is_mine) {
        error = "Only your own plugins can be deleted from the cloud.";
        return false;
    }

    int result = m_orca_agent->delete_my_plugin(cloud_uuid);

    if (result != 0) {
        error = "Failed to delete plugin from cloud, see logs for more info.";
        return false;
    }

    return true;
}

bool CloudPluginService::download_cloud_plugin(PluginDescriptor& entry,
                                               const std::string& requested_version,
                                               CloudPluginDownload& download,
                                               std::string& error) const
{
    namespace fs = boost::filesystem;

    error.clear();
    download = {};

    // Look up the cloud plugin metadata in the catalog.
    std::string download_url;
    if (!entry.is_cloud_plugin()) {
        error = "Plugin is not a cloud plugin";
        return false;
    }

    const std::string entry_uuid = entry.cloud_uuid();

    if (!m_orca_agent) {
        error = "Cloud service agent is null";
        return false;
    }

    std::vector<PluginDownloadData> data;
    std::vector<PluginDownloadNotFound> not_found;
    std::vector<std::string> unauthorized;

    int result = m_orca_agent->get_plugin_download_url(entry_uuid, requested_version, data, not_found, unauthorized);
    if (result != 0) {
        error = "Failed to fetch download_url, result =" + std::to_string(result);
        return false;
    }

    for (const std::string& plugin_uuid : unauthorized) {
        if (plugin_uuid == entry_uuid) {
            error = "You are not authorized to download this cloud plugin.";
            return false;
        }
    }

    for (const PluginDownloadNotFound& item : not_found) {
        if (item.id == entry_uuid) {
            if (item.reason == "requested os not found")
                error = "No plugin package is available for this operating system.";
            else if (item.reason == "requested version not found")
                error = requested_version.empty() ? "The selected plugin version was not found." :
                                                    "Plugin version " + requested_version + " was not found.";
            else if (item.reason == "requested plugin not found")
                error = "Cloud plugin was not found.";
            else if (!item.reason.empty())
                error = "Cloud plugin download was not found: " + item.reason + ".";
            else
                error = "Cloud plugin download was not found.";
            return false;
        }
    }

    for (const PluginDownloadData& item : data) {
        if (item.plugin_id == entry_uuid) {
            download_url = item.download_link;
            break;
        }
    }

    if (download_url.empty()) {
        error = "No download URL is available for this plugin. Please "
                "check the logs for errors.";
        return false;
    }

    // Download the plugin package.
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Downloading cloud plugin " << entry_uuid << " from " << download_url;

    std::string body;
    unsigned http_status = 0;

    Http http = Http::get(download_url);
    http.timeout_connect(30)
        .timeout_max(300)
        .on_complete([&body, &http_status](std::string response_body, unsigned status) {
            body        = std::move(response_body);
            http_status = status;
        })
        .on_error([&error](std::string response_body, std::string err, unsigned status) {
            error = std::move(err);
            if (!response_body.empty())
                error += " — " + response_body;
        })
        .perform_sync();

    if (!error.empty()) {
        error = "Failed to download plugin: " + error;
        return false;
    }

    if (http_status >= 400) {
        error = "Plugin download failed with HTTP status " + std::to_string(http_status);
        return false;
    }

    if (body.empty()) {
        error = "Plugin download returned empty data.";
        return false;
    }

    // Detect file format from content: PK magic bytes = zip/wheel, otherwise .py.
    const bool is_wheel   = (body.size() >= 4 && body[0] == 'P' && body[1] == 'K' && body[2] == '\x03' && body[3] == '\x04');
    const std::string ext = is_wheel ? ".whl" : ".py";

    const fs::path tmp_path = fs::temp_directory_path() / (fs::unique_path("cloud_plugin-%%%%-%%%%-%%%%-%%%%%%%").string() + ext);
    {
        boost::nowide::ofstream file(tmp_path.string(), std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "Failed to create temporary file for plugin download.";
            return false;
        }
        file.write(body.data(), body.size());
        if (!file) {
            error = "Failed to write plugin data to temporary file.";
            return false;
        }
    }

    download.package_path = tmp_path;

    if (entry.cloud.has_value())
        entry.cloud->update_available = false;
    return true;
}

bool CloudPluginService::fetch_plugin_changelog(const PluginDescriptor& descriptor,
                                                std::vector<PluginChangelog>& changelog,
                                                std::string& error) const
{
    error.clear();
    changelog.clear();

    std::string plugin_uuid = descriptor.cloud_uuid();
    std::vector<PluginDescriptor> descriptors{descriptor};
    std::unordered_map<std::string, std::vector<PluginChangelog>> changelogs;

    bool result = fetch_plugin_changelog(descriptors, changelogs, error);

    bool found_changelog = changelogs.find(plugin_uuid) != changelogs.end();

    if (changelogs.empty() || !found_changelog) {
        if (!error.empty())
            return false;
        error = "Failed to fetch changelogs for plugin " + descriptor.cloud_uuid();
        return false;
    }

    changelog = std::move(changelogs[plugin_uuid]);

    return result;
}

bool CloudPluginService::fetch_plugin_changelog(const std::vector<PluginDescriptor>& descriptors,
                                                std::unordered_map<std::string, std::vector<PluginChangelog>>& changelog,
                                                std::string& error) const
{
    error.clear();
    changelog.clear();

    if (!m_orca_agent) {
        error = "Cloud service agent is null.";
        return false;
    }

    size_t descriptor_count = descriptors.size();

    if (descriptor_count <= 0) {
        return true;
    }

    std::vector<std::string> uuids;
    uuids.reserve(descriptor_count);
    for (const PluginDescriptor& descriptor : descriptors) {
        const std::string uuid = descriptor.cloud_uuid();
        if (!uuid.empty())
            uuids.push_back(uuid);
    }

    if (uuids.empty())
        return true;

    int result = m_orca_agent->fetch_plugin_changelogs(uuids, changelog);

    if (result != 0) {
        error = "Failed to fetch one or more plugin changelogs. result=" + std::to_string(result);
        BOOST_LOG_TRIVIAL(warning) << error;
        return false;
    }

    return true;
}

} // namespace Slic3r
