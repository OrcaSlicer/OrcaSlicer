#include "McpStdioBridge.hpp"

#include "McpOptions.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs   = boost::filesystem;
namespace net  = boost::asio;
namespace http = boost::beast::http;
using tcp      = net::ip::tcp;

namespace {

struct Endpoint
{
    int         port = 0;
    std::string token;
};

std::string base64(const std::string& in)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    int                val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(table[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6)
        out.push_back(table[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

bool header_safe(const std::string& v)
{
    if (v.empty() || v.front() == ' ' || v.back() == ' ' || (v.rfind("=?base64?", 0) == 0))
        return false;
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

// Mirrors GUI_App::init_app_config's choice without a wxApp.
fs::path default_datadir()
{
#ifdef __linux__
    return fs::path(default_data_dir());
#elif defined(_WIN32)
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path portable = fs::path(exe).parent_path() / "data_dir";
    if (fs::exists(portable))
        return portable;
    const char* appdata = boost::nowide::getenv("APPDATA");
    return fs::path(appdata ? appdata : ".") / SLIC3R_APP_KEY;
#else
    char     exe[4096] = {0};
    uint32_t size      = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) == 0) {
        const fs::path portable = fs::path(exe).parent_path().parent_path().parent_path().parent_path() / "data_dir";
        if (fs::exists(portable))
            return portable;
    }
    const char* home = boost::nowide::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Application Support" / SLIC3R_APP_KEY;
#endif
}

std::optional<Endpoint> read_discovery(const fs::path& datadir)
{
    boost::nowide::ifstream in((datadir / "mcp" / "server.json").string());
    if (!in)
        return std::nullopt;
    const json info = json::parse(in, nullptr, false);
    if (info.is_discarded() || !info.contains("port"))
        return std::nullopt;
    Endpoint e;
    e.port  = info.value("port", 0);
    e.token = info.value("token", "");
    return e;
}

// POSTs one message; nullopt when the endpoint does not accept a connection.
// `sent` (when given) tells whether the request left this process, so the caller knows whether the
// server may have acted on it.
std::optional<std::pair<int, std::string>> post(const Endpoint& e, const std::string& body, const std::map<std::string, std::string>& headers,
                                                bool* sent = nullptr)
{
    if (sent != nullptr)
        *sent = false;
    try {
        net::io_context ioc;
        tcp::socket     socket(ioc);
        socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), uint16_t(e.port)));
        http::request<http::string_body> req{http::verb::post, "/mcp", 11};
        req.set(http::field::host, "127.0.0.1:" + std::to_string(e.port));
        req.set(http::field::content_type, "application/json");
        req.set(http::field::accept, "application/json, text/event-stream");
        req.set(http::field::authorization, "Bearer " + e.token);
        for (const auto& [k, v] : headers)
            req.set(k, v);
        req.body() = body;
        req.prepare_payload();
        http::write(socket, req);
        if (sent != nullptr)
            *sent = true;
        boost::beast::flat_buffer          buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res);
        return std::make_pair(int(res.result_int()), res.body());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool alive(const Endpoint& e)
{
    const json ping{{"jsonrpc", "2.0"}, {"id", "bridge-probe"}, {"method", "ping"}};
    auto       r = post(e, ping.dump(), {{"Mcp-Method", "ping"}});
    return r && r->first == 200;
}

std::string self_executable(char** argv)
{
    // An AppImage is relaunched through its runtime, not the extracted binary.
    if (const char* appimage = boost::nowide::getenv("APPIMAGE"); appimage && *appimage)
        return appimage;
#ifdef _WIN32
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).string();
#elif defined(__APPLE__)
    char     exe[4096] = {0};
    uint32_t size      = sizeof(exe);
    return _NSGetExecutablePath(exe, &size) == 0 ? std::string(exe) : std::string(argv[0]);
#else
    char    exe[4096] = {0};
    ssize_t n         = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    return n > 0 ? std::string(exe, std::size_t(n)) : std::string(argv[0]);
#endif
}

// Starts `exe --mcp [--datadir dir]` detached from this process. Returns false when it could not be
// started or exited within a moment (a running instance without MCP took the launch over).
bool launch_app(const std::string& exe, const std::string& datadir)
{
#ifdef _WIN32
    std::wstring cmd = L"\"" + fs::path(exe).wstring() + L"\" --mcp";
    if (!datadir.empty())
        cmd += L" --datadir \"" + fs::path(datadir).wstring() + L"\"";
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return false;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_TIMEOUT;
#else
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        // The GUI must not inherit the MCP stdio pipes.
        std::freopen("/dev/null", "r", stdin);
        std::freopen("/dev/null", "w", stdout);
        std::vector<std::string> args{exe, "--mcp"};
        if (!datadir.empty()) {
            args.push_back("--datadir");
            args.push_back(datadir);
        }
        std::vector<char*> cargs;
        for (std::string& a : args)
            cargs.push_back(a.data());
        cargs.push_back(nullptr);
        execv(exe.c_str(), cargs.data());
        _exit(127);
    }
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
#endif
}

void write_line(const std::string& s)
{
    boost::nowide::cout << s << '\n';
    boost::nowide::cout.flush();
}

json error_reply(const json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

} // namespace

std::map<std::string, std::string> bridge_headers(const json& message, const std::string& negotiated_version)
{
    std::map<std::string, std::string> h;
    const std::string                  method = message.value("method", "");
    const json                         params = message.contains("params") && message["params"].is_object() ? message["params"] : json::object();
    std::string                        version = negotiated_version;
    if (params.contains("_meta") && params["_meta"].is_object()) {
        const json& meta = params["_meta"];
        if (auto it = meta.find("io.modelcontextprotocol/protocolVersion"); it != meta.end() && it->is_string())
            version = it->get<std::string>();
    }
    if (!version.empty())
        h["MCP-Protocol-Version"] = version;
    if (!method.empty())
        h["Mcp-Method"] = method;
    std::string name;
    if (method == "tools/call" || method == "prompts/get")
        name = params.value("name", "");
    else if (method == "resources/read")
        name = params.value("uri", "");
    if (!name.empty())
        h["Mcp-Name"] = header_safe(name) ? name : "=?base64?" + base64(name) + "?=";
    return h;
}

int run_stdio_bridge(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::string datadir_arg;
    int         port_arg = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto              value = [&](const char* name) -> std::optional<std::string> {
            const std::string flag = name;
            if (a == flag && i + 1 < argc)
                return std::string(argv[++i]);
            if (a.rfind(flag + "=", 0) == 0)
                return a.substr(flag.size() + 1);
            return std::nullopt;
        };
        if (auto v = value("--datadir"))
            datadir_arg = *v;
        else if (auto v = value("--mcp-port"))
            port_arg = std::atoi(v->c_str());
    }
    const fs::path datadir = datadir_arg.empty() ? default_datadir() : fs::path(datadir_arg);

    auto resolve = [&]() -> std::optional<Endpoint> {
        std::optional<Endpoint> e = read_discovery(datadir);
        if (!e)
            e = Endpoint{};
        if (port_arg > 0)
            e->port = port_arg;
        if (const char* env = boost::nowide::getenv("ORCA_MCP_TOKEN"); env && *env)
            e->token = env;
        if (e->port <= 0)
            e->port = kDefaultPort;
        return alive(*e) ? e : std::nullopt;
    };

    std::optional<Endpoint> endpoint = resolve();
    std::string             failure;
    if (!endpoint) {
        if (!launch_app(self_executable(argv), datadir_arg))
            failure = "OrcaSlicer is running without the MCP server, or could not be started. Turn on \"MCP server for "
                      "AI agents\" in OrcaSlicer's Preferences (or start it with --mcp) and reconnect.";
        else
            for (int i = 0; i < 120 && !endpoint; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                endpoint = resolve();
            }
        if (!endpoint && failure.empty())
            failure = "OrcaSlicer did not start its MCP server within 60 seconds.";
    }

    std::string negotiated;
    std::string line;
    while (std::getline(boost::nowide::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;
        const json msg = json::parse(line, nullptr, false);
        if (msg.is_discarded()) {
            write_line(error_reply(nullptr, -32700, "Parse error").dump());
            continue;
        }
        const bool is_request = msg.is_object() && msg.contains("id") && msg.contains("method");
        if (!endpoint) {
            if (is_request)
                write_line(error_reply(msg["id"], -32603, failure).dump());
            continue;
        }
        bool sent  = false;
        auto reply = post(*endpoint, line, bridge_headers(msg, negotiated), &sent);
        if ((!reply && !sent) || (reply && reply->first == 401)) {
            // The app restarted, or its port or token changed in Preferences: the discovery file
            // has the current ones. Only a request the server cannot have acted on is sent again.
            if (auto fresh = resolve()) {
                endpoint = fresh;
                reply    = post(*endpoint, line, bridge_headers(msg, negotiated), &sent);
            }
        } else if (!reply) {
            // Sent, but the connection closed before the reply (the call restarted the server, or
            // the app quit). Sending it again could run it twice; later calls go to the new endpoint.
            if (auto fresh = resolve())
                endpoint = fresh;
            if (is_request)
                write_line(error_reply(msg["id"], -32603,
                                       "OrcaSlicer closed the connection before replying; the call may have run. Check with "
                                       "get_state before repeating it.")
                               .dump());
            continue;
        }
        if (!reply) {
            if (is_request)
                write_line(error_reply(msg["id"], -32603, "OrcaSlicer's MCP server is not reachable.").dump());
            continue;
        }
        if (reply->first == 202 || reply->second.empty())
            continue;
        const json body = json::parse(reply->second, nullptr, false);
        if (body.is_discarded() || !body.contains("jsonrpc")) {
            if (is_request)
                write_line(error_reply(msg["id"], -32603, "HTTP " + std::to_string(reply->first) + ": " + reply->second).dump());
            continue;
        }
        if (msg.value("method", "") == "initialize" && body.contains("result"))
            negotiated = body["result"].value("protocolVersion", "");
        write_line(body.dump());
    }
    return 0;
}

}}} // namespace Slic3r::GUI::Mcp
