#pragma once

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

struct OnlineModelsProvider
{
    std::string id;
    std::string display_name;
    std::string homepage_url;
    std::vector<std::string> allowed_hostnames;
    std::string icon_name;
    bool enabled { true };
    bool built_in { false };

    bool operator==(const OnlineModelsProvider& rhs) const;
};

const std::vector<OnlineModelsProvider>& online_models_default_providers();
std::vector<OnlineModelsProvider> online_models_enabled_providers(const std::vector<OnlineModelsProvider>& providers);
void online_models_restore_default_providers(std::vector<OnlineModelsProvider>& providers);

std::string online_models_serialize_providers(const std::vector<OnlineModelsProvider>& providers);
std::optional<std::vector<OnlineModelsProvider>> online_models_deserialize_providers(const std::string& value);

bool online_models_validate_provider(const OnlineModelsProvider& provider, std::string* error = nullptr);
bool online_models_validate_homepage_url(const std::string& url, bool* is_unencrypted = nullptr,
                                         std::string* normalized_url = nullptr);

boost::filesystem::path online_models_download_dir();
boost::filesystem::path online_models_download_dir(const boost::filesystem::path& data_dir);

std::string online_models_sanitize_path_component(const std::string& value);
std::string online_models_sanitize_filename(const std::string& value);
boost::filesystem::path online_models_provider_dir(const boost::filesystem::path& root, const OnlineModelsProvider& provider);
boost::filesystem::path online_models_download_date_dir(const boost::filesystem::path& root,
                                                        const OnlineModelsProvider& provider,
                                                        const std::string& iso_date);
boost::filesystem::path online_models_unique_download_path(const boost::filesystem::path& directory,
                                                           const std::string& suggested_filename);

bool online_models_is_importable_file(const boost::filesystem::path& path);

enum class OnlineModelsDownloadState {
    Pending,
    Downloading,
    Completed,
    Failed,
    Cancelled
};

struct OnlineModelsDownloadItem {
    std::string id;
    std::string provider_id;
    boost::filesystem::path destination;
    std::int64_t bytes_received { 0 };
    std::int64_t total_bytes { -1 };
    OnlineModelsDownloadState state { OnlineModelsDownloadState::Pending };
    std::string error;

    std::optional<int> progress_percent() const;
    bool terminal() const;
};

class OnlineModelsDownloadQueue
{
public:
    bool add(OnlineModelsDownloadItem item);
    bool update(const std::string& id, std::int64_t bytes_received, std::int64_t total_bytes,
                OnlineModelsDownloadState state, std::string error = {});
    bool remove(const std::string& id);
    const OnlineModelsDownloadItem* find(const std::string& id) const;
    size_t size() const { return m_items.size(); }

private:
    std::vector<OnlineModelsDownloadItem> m_items;
};

} // namespace Slic3r
