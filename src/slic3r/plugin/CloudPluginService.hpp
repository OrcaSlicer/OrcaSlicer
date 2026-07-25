#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {

class OrcaCloudServiceAgent;

struct CloudPluginDownload
{
    boost::filesystem::path package_path;
};

class CloudPluginService
{
public:
    void set_cloud_agent(std::shared_ptr<OrcaCloudServiceAgent> agent);
    std::shared_ptr<OrcaCloudServiceAgent> get_cloud_agent() const;
    bool can_fetch_cloud_plugins() const;
    bool fetch_manifests_into_descriptors(std::vector<PluginDescriptor>& descriptors,
                                      std::vector<std::string>& not_found,
                                      std::vector<std::string>& unauthorized) const;
    bool request_cloud_subscribe(const std::string& plugin_uuid, std::string& error) const;
    bool request_cloud_unsubscribe(const PluginDescriptor& plugin, std::string& error) const;
    bool request_cloud_delete(const PluginDescriptor& plugin, std::string& error) const;
    bool download_cloud_plugin(PluginDescriptor& entry,
                               const std::string& requested_version,
                               CloudPluginDownload& download,
                               std::string& error) const;
    bool fetch_plugin_changelog(const PluginDescriptor& descriptor, std::vector<PluginChangelog>& changelog, std::string& error) const;
    bool fetch_plugin_changelog(const std::vector<PluginDescriptor>& descriptors,
                                std::unordered_map<std::string, std::vector<PluginChangelog>>& changelog,
                                std::string& error) const;

private:
    std::shared_ptr<OrcaCloudServiceAgent> m_orca_agent = nullptr;
};

} // namespace Slic3r
