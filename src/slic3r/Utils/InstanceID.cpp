#include "InstanceID.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace instance_id {
namespace {

constexpr const char* CONFIG_KEY = "updater_iid";
constexpr const char* LEGACY_KEY = "iid";

std::mutex& cache_mutex()
{
    static std::mutex mtx;
    return mtx;
}

std::string& cached_iid()
{
    static std::string value;
    return value;
}

bool& cache_ready()
{
    static bool ready = false;
    return ready;
}

std::optional<std::string> normalize_uuid(std::string value)
{
    boost::algorithm::trim(value);
    boost::algorithm::to_lower(value);
    if (value.size() != 36)
        return std::nullopt;
    try {
        const boost::uuids::uuid parsed = boost::uuids::string_generator()(value);
        if (parsed.version() != boost::uuids::uuid::version_random_number_based)
            return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> read_config_value(AppConfig& config)
{
    const auto read_key = [&](const char* key) -> std::optional<std::string> {
        const std::string raw = config.get(key);
        if (raw.empty())
            return std::nullopt;
        if (auto normalized = normalize_uuid(raw))
            return normalized;
        return std::nullopt;
    };

    if (auto value = read_key(CONFIG_KEY))
        return value;
    return read_key(LEGACY_KEY);
}

void write_config_value(AppConfig& config, const std::string& value)
{
    config.set(CONFIG_KEY, value);
    if (config.get(LEGACY_KEY) != value)
        config.set(LEGACY_KEY, value);
}

void prune_config_value(AppConfig& config)
{
    if (config.has(CONFIG_KEY))
        config.erase("app", CONFIG_KEY);
    if (config.has(LEGACY_KEY))
        config.erase("app", LEGACY_KEY);
}

boost::filesystem::path storage_path()
{
    const std::string& base_dir = Slic3r::data_dir();
    if (base_dir.empty())
        return {};
    return boost::filesystem::path(base_dir) / ".orcaslicer_machine_id";
}

std::optional<std::string> read_storage_file()
{
    const auto path = storage_path();
    if (path.empty() || !boost::filesystem::exists(path))
        return std::nullopt;

    boost::nowide::ifstream file(path.string());
    if (!file)
        return std::nullopt;

    std::string value;
    std::getline(file, value);
    file.close();

    return normalize_uuid(value);
}

bool write_storage_file(const std::string& value)
{
    if (value.empty())
        return false;

    const auto path = storage_path();
    if (path.empty())
        return false;

    const auto parent = path.parent_path();
    boost::system::error_code ec;
    if (!parent.empty() && !boost::filesystem::exists(parent))
        boost::filesystem::create_directories(parent, ec);

    if (ec)
        return false;

    boost::nowide::ofstream file(path.string(), std::ios::trunc);
    if (!file)
        return false;

    file << value;
    file.close();

    return file.good();
}

std::optional<std::string> read_secure()
{
    return read_storage_file();
}

bool write_secure(const std::string& value)
{
    return write_storage_file(value);
}

std::string generate_uuid()
{
    const auto uuid = boost::uuids::random_generator()();
    return boost::uuids::to_string(uuid);
}

} // namespace

std::string ensure(AppConfig& config)
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    if (cache_ready())
        return cached_iid();

    if (auto secure = read_secure()) {
        cached_iid() = *secure;
        cache_ready() = true;
        prune_config_value(config);
        return cached_iid();
    }

    if (auto from_config = read_config_value(config)) {
        cached_iid() = *from_config;
        cache_ready() = true;
        if (!write_secure(cached_iid()))
            write_config_value(config, cached_iid());
        else
            prune_config_value(config);
        return cached_iid();
    }

    cached_iid() = generate_uuid();
    cache_ready() = true;

    if (!write_secure(cached_iid()))
        write_config_value(config, cached_iid());
    else
        prune_config_value(config);

    return cached_iid();
}

void reset_cache_for_tests()
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    cached_iid().clear();
    cache_ready() = false;
}

} // namespace instance_id
} // namespace Slic3r
