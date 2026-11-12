#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch_all.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

using tcp = boost::asio::ip::tcp;

// A JWT whose only claim is {"exp":4102444800} (2100-01-01). A session signed in with it never
// expires, so the cloud agent does not try to refresh the token during a test.
constexpr const char* FAR_FUTURE_JWT = "eyJhbGciOiJub25lIn0.eyJleHAiOjQxMDI0NDQ4MDB9.sig";

// The package used for both the installed plugin and its replacement. The version in the PEP 723
// header makes the replaced file observable. When `slow` is set, the module sleeps before its
// registration so a test can hold a load in progress.
std::string cloud_plugin_source(const std::string& version, bool slow = false)
{
    std::string source = "# /// script\n"
                         "# requires-python = \">=3.12\"\n"
                         "#\n"
                         "# [tool.orcaslicer.plugin]\n"
                         "# name = \"Cloud Update Plugin\"\n"
                         "# version = \"" +
                         version +
                         "\"\n"
                         "# type = \"script\"\n"
                         "# ///\n";
    if (slow)
        source += "import time\n"
                  "time.sleep(2)\n"
                  "\n";
    source += "import orca\n"
              "\n"
              "class Echo(orca.script.ScriptPluginCapabilityBase):\n"
              "    def get_name(self):\n"
              "        return \"Echo\"\n"
              "\n"
              "    def execute(self, ctx):\n"
              "        return orca.ExecutionResult.success()\n"
              "\n"
              "@orca.plugin\n"
              "class CloudUpdatePackage(orca.base):\n"
              "    def register_capabilities(self):\n"
              "        orca.register_capability(Echo)\n";
    return source;
}

// Brings the plugin manager up for a case that loads a real plugin. Declare it first in the test
// so it is destroyed last. The interpreter itself is process-wide and finalized once by the suite.
struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-update"};
    bool          initialized = PluginManager::instance().initialize();

    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

// A single-request HTTP server on the loopback interface. The cloud agent tests already use a
// loopback server; this narrower one serves the two endpoints update_cloud_plugin needs: the
// download-URL POST and the package GET.
class OneShotHttpServer
{
public:
    OneShotHttpServer(std::string body, std::string content_type)
        : m_acceptor(m_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)),
          m_body(std::move(body)),
          m_content_type(std::move(content_type))
    {
        accept_next();
        m_thread = std::thread([this] { m_io.run(); });
    }

    ~OneShotHttpServer()
    {
        m_io.stop();
        if (m_thread.joinable())
            m_thread.join();
    }

    std::string url() const
    {
        return "http://127.0.0.1:" + std::to_string(m_acceptor.local_endpoint().port());
    }

    OneShotHttpServer(const OneShotHttpServer&)            = delete;
    OneShotHttpServer& operator=(const OneShotHttpServer&) = delete;

private:
    void accept_next()
    {
        m_acceptor.async_accept([this](const boost::system::error_code& ec, tcp::socket socket) {
            if (ec)
                return;
            serve(std::move(socket));
        });
    }

    void serve(tcp::socket socket)
    {
        auto connection = std::make_shared<tcp::socket>(std::move(socket));
        auto request    = std::make_shared<boost::asio::streambuf>();
        boost::asio::async_read_until(
            *connection, *request, "\r\n\r\n",
            [this, connection, request](const boost::system::error_code& ec, std::size_t) {
                (void) request; // keeps the receive buffer alive until the request is consumed
                if (ec)
                    return;
                auto response = std::make_shared<std::string>(
                    "HTTP/1.1 200 OK\r\nContent-Type: " + m_content_type +
                    "\r\nContent-Length: " + std::to_string(m_body.size()) +
                    "\r\nConnection: close\r\n\r\n" + m_body);
                boost::asio::async_write(
                    *connection, boost::asio::buffer(*response),
                    [connection, response](const boost::system::error_code&, std::size_t) {
                        boost::system::error_code ignored;
                        connection->shutdown(tcp::socket::shutdown_both, ignored);
                    });
            });
    }

    // Declared first so the io_context outlives the acceptor and the connection handlers.
    boost::asio::io_context m_io;
    tcp::acceptor           m_acceptor;
    std::thread             m_thread;
    std::string             m_body;
    std::string             m_content_type;
};

// The JSON body of the download-URL endpoint: one catalog row naming the package to fetch.
std::string download_response(const std::string& uuid, const std::string& download_link)
{
    json row;
    row["plugin_id"]     = uuid;
    row["download_link"] = download_link;

    json rows = json::array();
    rows.push_back(std::move(row));

    json root;
    root["data"] = std::move(rows);
    return root.dump();
}

// The JSON body the cloud sends when the requested version does not exist.
// Installs a cloud package the way a previous update would have left it: one entry file plus the
// sidecar that records the cloud identity, activation flag, capability choices and grants.
fs::path write_cloud_package(const std::string& user_id,
                             const std::string& uuid,
                             const std::string& version,
                             bool               enabled,
                             std::vector<std::pair<std::string, bool>> capabilities,
                             std::vector<std::string>                  fs_read)
{
    const fs::path plugin_dir = fs::path(get_cloud_plugin_dir(user_id)) / uuid;
    fs::create_directories(plugin_dir);
    {
        std::ofstream out((plugin_dir / "Cloud_Update_Plugin.py").string(), std::ios::binary);
        out << cloud_plugin_source(version);
    }

    PluginInstallState state;
    state.installed_from      = "cloud";
    state.installed_version   = version;
    state.plugin_name         = "Cloud Update Plugin";
    state.cloud_uuid          = uuid;
    state.enabled             = enabled;
    state.capabilities        = std::move(capabilities);
    state.permissions.fs_read = std::move(fs_read);
    if (!write_install_state(plugin_dir, state))
        throw std::runtime_error("failed to write cloud plugin install state");
    return plugin_dir;
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The state a package keeps across an update: activation and capability choices survive, the
// executable package is the new version, and every permission is cleared.
void check_updated_state(const fs::path& plugin_dir, bool enabled)
{
    PluginInstallState state;
    REQUIRE(read_install_state(plugin_dir, state));
    CHECK(state.installed_version == "2.0");
    CHECK(state.enabled == enabled);
    REQUIRE(state.capabilities.size() == 1);
    CHECK(state.capabilities.front().first == "Echo");
    CHECK_FALSE(state.capabilities.front().second);
    CHECK(state.permissions.fs_read.empty());
    CHECK(state.permissions.network_http.empty());
    CHECK_FALSE(state.permissions.full_access);
}

} // namespace

TEST_CASE("Updating an active cloud plugin reloads it, keeps capability choices and resets permissions",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "33333333-4444-5555-6666-777777777777";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", true, {{"Echo", false}}, {"/old/grant"});

    OneShotHttpServer package_server(cloud_plugin_source("2.0"), "application/octet-stream");
    OneShotHttpServer api_server(download_response(uuid, package_server.url() + "/Cloud_Update_Plugin.py"),
                                 "application/json");

    auto agent = std::make_shared<OrcaCloudServiceAgent>((plugin_system.python_data_dir.dir / "cloud-agent").string());
    agent->set_api_base_url(api_server.url());
    REQUIRE(agent->set_user_session(json{{"access_token", FAR_FUTURE_JWT}, {"user_id", user_id}}, false));

    PluginManager& manager = PluginManager::instance();
    manager.set_cloud_agent(agent);
    manager.set_cloud_user(user_id);
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string load_error;
    manager.load_plugin(uuid, /*skip_deps=*/true);
    REQUIRE(manager.wait_for_plugin_load(uuid, std::chrono::seconds(120), load_error));
    REQUIRE(manager.is_plugin_loaded(uuid));

    // Off-main-thread traffic must not go through a proxy set in the environment.
    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    REQUIRE(manager.update_cloud_plugin(uuid, error, "2.0"));
    INFO("update error: " << error);

    // The plugin the user saw active is active again after its package is replaced.
    CHECK(manager.is_plugin_loaded(uuid));

    check_updated_state(plugin_dir, /*enabled=*/true);

    // The disabled capability choice rode along; the replacement package did not reset it.
    const auto echo = manager.get_plugin_capability({PluginCapabilityType::Unknown, "Echo", uuid},
                                                    /*only_enabled=*/false);
    REQUIRE(echo != nullptr);
    CHECK_FALSE(echo->is_enabled());

    manager.unload_plugin(uuid);
}

TEST_CASE("Updating a deactivated cloud plugin keeps it deactivated, keeps capability choices and resets permissions",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "44444444-5555-6666-7777-888888888888";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", false, {{"Echo", false}}, {"/old/grant"});

    OneShotHttpServer package_server(cloud_plugin_source("2.0"), "application/octet-stream");
    OneShotHttpServer api_server(download_response(uuid, package_server.url() + "/Cloud_Update_Plugin.py"),
                                 "application/json");

    auto agent = std::make_shared<OrcaCloudServiceAgent>((plugin_system.python_data_dir.dir / "cloud-agent").string());
    agent->set_api_base_url(api_server.url());
    REQUIRE(agent->set_user_session(json{{"access_token", FAR_FUTURE_JWT}, {"user_id", user_id}}, false));

    PluginManager& manager = PluginManager::instance();
    manager.set_cloud_agent(agent);
    manager.set_cloud_user(user_id);
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    REQUIRE(manager.update_cloud_plugin(uuid, error, "2.0"));
    INFO("update error: " << error);

    // A package the user deliberately deactivated must not come back activated.
    CHECK_FALSE(manager.is_plugin_loaded(uuid));
    check_updated_state(plugin_dir, /*enabled=*/false);
}

