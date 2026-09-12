#include "OnlineModels.hpp"

#include "Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace Slic3r {
namespace {

constexpr int registry_version = 1;

const std::vector<OnlineModelsProvider> s_default_providers = {
    {"printables", "Printables", "https://www.printables.com/", {"printables.com", "www.printables.com"}, "", true, true},
    {"makerworld", "MakerWorld", "https://makerworld.com/", {"makerworld.com", "www.makerworld.com"}, "", true, true},
    {"thingiverse", "Thingiverse", "https://www.thingiverse.com/", {"thingiverse.com", "www.thingiverse.com"}, "", true, true},
    {"creality_cloud", "Creality Cloud", "https://www.crealitycloud.com/", {"crealitycloud.com", "www.crealitycloud.com"}, "", true, true},
};

bool safe_identity(const std::string& id)
{
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
}

std::string lower_extension(boost::filesystem::path path)
{
    std::string extension = path.extension().string();
    boost::algorithm::to_lower(extension);
    return extension;
}

} // namespace

bool OnlineModelsProvider::operator==(const OnlineModelsProvider& rhs) const
{
    return id == rhs.id && display_name == rhs.display_name && homepage_url == rhs.homepage_url
        && allowed_hostnames == rhs.allowed_hostnames && icon_name == rhs.icon_name
        && enabled == rhs.enabled && built_in == rhs.built_in;
}

const std::vector<OnlineModelsProvider>& online_models_default_providers()
{
    return s_default_providers;
}

std::vector<OnlineModelsProvider> online_models_enabled_providers(const std::vector<OnlineModelsProvider>& providers)
{
    std::vector<OnlineModelsProvider> enabled;
    std::copy_if(providers.begin(), providers.end(), std::back_inserter(enabled),
                 [](const OnlineModelsProvider& provider) { return provider.enabled; });
    return enabled;
}

void online_models_restore_default_providers(std::vector<OnlineModelsProvider>& providers)
{
    for (const OnlineModelsProvider& default_provider : s_default_providers) {
        const auto existing = std::find_if(providers.begin(), providers.end(), [&](const OnlineModelsProvider& provider) {
            return provider.id == default_provider.id;
        });
        if (existing == providers.end())
            providers.push_back(default_provider);
    }
}

std::string online_models_serialize_providers(const std::vector<OnlineModelsProvider>& providers)
{
    nlohmann::json entries = nlohmann::json::array();
    for (const OnlineModelsProvider& provider : providers) {
        entries.push_back({
            {"id", provider.id}, {"name", provider.display_name}, {"url", provider.homepage_url},
            {"hosts", provider.allowed_hostnames}, {"icon", provider.icon_name},
            {"enabled", provider.enabled}, {"built_in", provider.built_in}
        });
    }
    return nlohmann::json{{"version", registry_version}, {"providers", std::move(entries)}}.dump();
}

std::optional<std::vector<OnlineModelsProvider>> online_models_deserialize_providers(const std::string& value)
{
    const nlohmann::json root = nlohmann::json::parse(value, nullptr, false);
    if (!root.is_object() || root.value("version", 0) != registry_version || !root.contains("providers")
        || !root["providers"].is_array())
        return std::nullopt;

    std::vector<OnlineModelsProvider> providers;
    std::set<std::string> ids;
    for (const nlohmann::json& entry : root["providers"]) {
        if (!entry.is_object())
            continue;
        OnlineModelsProvider provider;
        provider.id = entry.value("id", "");
        provider.display_name = entry.value("name", "");
        provider.homepage_url = entry.value("url", "");
        provider.icon_name = entry.value("icon", "");
        provider.enabled = entry.value("enabled", true);
        provider.built_in = entry.value("built_in", false);
        if (entry.contains("hosts") && entry["hosts"].is_array()) {
            for (const nlohmann::json& host : entry["hosts"])
                if (host.is_string())
                    provider.allowed_hostnames.push_back(host.get<std::string>());
        }
        if (online_models_validate_provider(provider) && ids.insert(provider.id).second)
            providers.push_back(std::move(provider));
    }
    if (!root["providers"].empty() && providers.empty())
        return std::nullopt;
    return providers;
}

bool online_models_validate_homepage_url(const std::string& url, bool* is_unencrypted, std::string* normalized_url)
{
    if (is_unencrypted)
        *is_unencrypted = false;
    std::string candidate = boost::algorithm::trim_copy(url);
    static const std::regex pattern(R"(^(https?)://([^/?#\s]+)([^\s]*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(candidate, match, pattern) || match[2].str().empty())
        return false;
    const std::string host = match[2].str();
    if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos
        || host.find('@') != std::string::npos || host == ":")
        return false;
    const bool http = boost::iequals(match[1].str(), "http");
    if (is_unencrypted)
        *is_unencrypted = http;
    if (normalized_url) {
        if (match[3].str().empty())
            candidate += '/';
        *normalized_url = candidate;
    }
    return true;
}

bool online_models_validate_provider(const OnlineModelsProvider& provider, std::string* error)
{
    auto fail = [&](const char* message) {
        if (error)
            *error = message;
        return false;
    };
    if (!safe_identity(provider.id))
        return fail("invalid id");
    if (boost::algorithm::trim_copy(provider.display_name).empty())
        return fail("empty name");
    if (!online_models_validate_homepage_url(provider.homepage_url))
        return fail("invalid URL");
    return true;
}

boost::filesystem::path online_models_download_dir()
{
    return online_models_download_dir(boost::filesystem::path(data_dir()));
}

boost::filesystem::path online_models_download_dir(const boost::filesystem::path& data_dir)
{
    return (data_dir / "OnlineModels").make_preferred();
}

std::string online_models_sanitize_path_component(const std::string& value)
{
    std::string sanitized = sanitize_filename(value);
    boost::trim(sanitized);
    if (sanitized == "." || sanitized == "..")
        sanitized.clear();
    return sanitized.empty() ? "unknown" : sanitized;
}

std::string online_models_sanitize_filename(const std::string& value)
{
    const boost::filesystem::path input(value);
    std::string sanitized = online_models_sanitize_path_component(input.filename().string());
    if (sanitized == "unknown")
        sanitized = "download";
    return sanitized;
}

boost::filesystem::path online_models_provider_dir(const boost::filesystem::path& root, const OnlineModelsProvider& provider)
{
    return (root / online_models_sanitize_path_component(provider.id)).make_preferred();
}

boost::filesystem::path online_models_download_date_dir(const boost::filesystem::path& root,
                                                        const OnlineModelsProvider& provider,
                                                        const std::string& iso_date)
{
    return (online_models_provider_dir(root, provider) / online_models_sanitize_path_component(iso_date)).make_preferred();
}

boost::filesystem::path online_models_unique_download_path(const boost::filesystem::path& directory,
                                                           const std::string& suggested_filename)
{
    const boost::filesystem::path clean_name(online_models_sanitize_filename(suggested_filename));
    boost::filesystem::path candidate = directory / clean_name;
    for (unsigned int suffix = 2; boost::filesystem::exists(candidate); ++suffix) {
        candidate = directory / (clean_name.stem().string() + " (" + std::to_string(suffix) + ")" + clean_name.extension().string());
    }
    return candidate.make_preferred();
}

bool online_models_is_importable_file(const boost::filesystem::path& path)
{
    static const std::set<std::string> extensions = {
        ".3mf", ".stl", ".oltp", ".stp", ".step", ".svg", ".amf", ".obj", ".gltf", ".glb", ".fbx", ".drc", ".zip"
#ifdef __APPLE__
        , ".usd", ".usda", ".usdc", ".usdz", ".abc", ".ply"
#endif
    };
    return extensions.count(lower_extension(path)) != 0;
}

std::optional<int> OnlineModelsDownloadItem::progress_percent() const
{
    if (total_bytes <= 0)
        return std::nullopt;
    return static_cast<int>(std::clamp<std::int64_t>(bytes_received * 100 / total_bytes, 0, 100));
}

bool OnlineModelsDownloadItem::terminal() const
{
    return state == OnlineModelsDownloadState::Completed
        || state == OnlineModelsDownloadState::Failed
        || state == OnlineModelsDownloadState::Cancelled;
}

bool OnlineModelsDownloadQueue::add(OnlineModelsDownloadItem item)
{
    if (item.id.empty() || item.provider_id.empty() || find(item.id))
        return false;
    m_items.push_back(std::move(item));
    return true;
}

bool OnlineModelsDownloadQueue::update(const std::string& id, std::int64_t bytes_received,
                                       std::int64_t total_bytes, OnlineModelsDownloadState state,
                                       std::string error)
{
    auto found = std::find_if(m_items.begin(), m_items.end(), [&](const OnlineModelsDownloadItem& item) {
        return item.id == id;
    });
    if (found == m_items.end() || found->terminal())
        return false;
    found->bytes_received = std::max<std::int64_t>(0, bytes_received);
    found->total_bytes = total_bytes;
    found->state = state;
    found->error = std::move(error);
    return true;
}

bool OnlineModelsDownloadQueue::remove(const std::string& id)
{
    const auto found = std::find_if(m_items.begin(), m_items.end(), [&](const OnlineModelsDownloadItem& item) {
        return item.id == id;
    });
    if (found == m_items.end())
        return false;
    m_items.erase(found);
    return true;
}

const OnlineModelsDownloadItem* OnlineModelsDownloadQueue::find(const std::string& id) const
{
    const auto found = std::find_if(m_items.begin(), m_items.end(), [&](const OnlineModelsDownloadItem& item) {
        return item.id == id;
    });
    return found == m_items.end() ? nullptr : &*found;
}

} // namespace Slic3r
