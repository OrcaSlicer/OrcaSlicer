#include "McpPaths.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

namespace {

// UTF-8 to a native path (wide on Windows).
fs::path to_path(const std::string& utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

std::optional<fs::path> absolute(const std::string& path, std::string& err)
{
    if (path.empty()) {
        err = "path is empty";
        return std::nullopt;
    }
    fs::path p = to_path(path);
    if (!p.is_absolute()) {
        err = "path must be absolute: " + path;
        return std::nullopt;
    }
    return p.lexically_normal();
}

} // namespace

std::optional<fs::path> resolve_input_path(const std::string& path, std::string& err)
{
    auto p = absolute(path, err);
    if (!p)
        return std::nullopt;
    boost::system::error_code ec;
    if (!fs::exists(*p, ec)) {
        err = "file not found: " + path;
        return std::nullopt;
    }
    return p;
}

std::optional<fs::path> resolve_output_path(const std::string& path, bool overwrite, std::string& err)
{
    auto p = absolute(path, err);
    if (!p)
        return std::nullopt;
    boost::system::error_code ec;
    if (p->filename().empty() || fs::is_directory(*p, ec)) {
        err = "path names a directory, not a file: " + path;
        return std::nullopt;
    }
    if (!fs::is_directory(p->parent_path(), ec)) {
        err = "directory does not exist: " + p->parent_path().string();
        return std::nullopt;
    }
    if (fs::exists(*p, ec) && !overwrite) {
        err = "file exists (pass overwrite: true to replace it): " + path;
        return std::nullopt;
    }
    return p;
}

std::optional<fs::path> resolve_output_dir(const std::string& path, std::string& err)
{
    auto p = absolute(path, err);
    if (!p)
        return std::nullopt;
    boost::system::error_code ec;
    if (!fs::is_directory(*p, ec)) {
        err = "directory does not exist: " + path;
        return std::nullopt;
    }
    return p;
}

std::optional<PathParam> parse_path_param(const std::string& param, std::string& err)
{
    PathParam out;
    const auto first = param.find_first_not_of(" \t");
    if (first != std::string::npos && param[first] == '{') {
        const nlohmann::json j = nlohmann::json::parse(param, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("path") || !j["path"].is_string()) {
            err = "the parameter must be a path or {\"path\": \"...\", \"overwrite\": true}";
            return std::nullopt;
        }
        out.path      = j["path"].get<std::string>();
        out.overwrite = j.value("overwrite", false);
    } else {
        out.path = param;
    }
    if (out.path.empty()) {
        err = "path is empty";
        return std::nullopt;
    }
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
