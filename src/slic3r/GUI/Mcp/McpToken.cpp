#include "McpToken.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

namespace {

bool is_token(const std::string& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

} // namespace

std::string random_token()
{
    std::random_device rd;
    std::string        out;
    out.reserve(64);
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        const unsigned v = rd() & 0xFFu;
        out.push_back(hex[v >> 4]);
        out.push_back(hex[v & 0xF]);
    }
    return out;
}

fs::path mcp_dir(const fs::path& datadir)
{
    const fs::path dir = datadir / "mcp";
    boost::system::error_code ec;
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
#ifndef _WIN32
        ::chmod(dir.string().c_str(), 0700);
#endif
    }
    return dir;
}

bool write_private_file(const fs::path& path, const std::string& content, std::string& err)
{
    const fs::path tmp = path.parent_path() / (path.filename().string() + ".tmp-" + random_token().substr(0, 8));
#ifndef _WIN32
    const int fd = ::open(tmp.string().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        err = "cannot create " + tmp.string();
        return false;
    }
    const char* data = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, data, left);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.string().c_str());
            err = "cannot write " + tmp.string();
            return false;
        }
        data += n;
        left -= std::size_t(n);
    }
    ::close(fd);
#else
    // %APPDATA% is private to the user through its ACL; the file inherits it.
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot create " + tmp.string();
            return false;
        }
        out << content;
    }
#endif
    boost::system::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "cannot replace " + path.string();
        return false;
    }
    return true;
}

std::string load_or_create_token(const fs::path& datadir, std::string& err)
{
    const fs::path file = mcp_dir(datadir) / "token";
    boost::system::error_code ec;
    if (fs::exists(file, ec)) {
        boost::nowide::ifstream in(file.string());
        std::string             token;
        std::getline(in, token);
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c) != 0; }), token.end());
        if (is_token(token))
            return token;
    }
    return regenerate_token(datadir, err);
}

std::string regenerate_token(const fs::path& datadir, std::string& err)
{
    const std::string token = random_token();
    if (!write_private_file(mcp_dir(datadir) / "token", token + "\n", err))
        return {};
    return token;
}

fs::path discovery_path(const fs::path& datadir) { return datadir / "mcp" / "server.json"; }

bool write_discovery(const fs::path& datadir, const nlohmann::json& info, std::string& err)
{
    mcp_dir(datadir);
    return write_private_file(discovery_path(datadir), info.dump(2) + "\n", err);
}

void remove_discovery(const fs::path& datadir)
{
    boost::system::error_code ec;
    fs::remove(discovery_path(datadir), ec);
}

}}} // namespace Slic3r::GUI::Mcp
