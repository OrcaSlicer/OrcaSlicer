#pragma once

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// <datadir>/mcp, created owner-only (0700) on POSIX.
boost::filesystem::path mcp_dir(const boost::filesystem::path& datadir);

// The install's bearer token from <datadir>/mcp/token, created on first use as 64 hex characters in
// an owner-only file. Kept out of AppConfig, whose atomic writes and backups use default
// permissions. Empty with `err` set when the file cannot be read or created.
std::string load_or_create_token(const boost::filesystem::path& datadir, std::string& err);

// Replaces the token file with a fresh token and returns it.
std::string regenerate_token(const boost::filesystem::path& datadir, std::string& err);

// Writes `content` to `path` so that it is never readable by others, even briefly: a temporary
// file created owner-only and exclusively, then renamed over the target.
bool write_private_file(const boost::filesystem::path& path, const std::string& content, std::string& err);

// <datadir>/mcp/server.json: {url, port, pid, version, token} with the effective token, for the
// stdio bridge and the developer harness.
boost::filesystem::path discovery_path(const boost::filesystem::path& datadir);
bool write_discovery(const boost::filesystem::path& datadir, const nlohmann::json& info, std::string& err);
void remove_discovery(const boost::filesystem::path& datadir);

// 32 random bytes as lowercase hex.
std::string random_token();

}}} // namespace Slic3r::GUI::Mcp
