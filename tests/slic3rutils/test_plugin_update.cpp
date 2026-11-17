#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
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
#include <ios>
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

#include "catch2/catch_message.hpp"
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

// Signs a throwaway agent in for a test. The JSON overload's second argument is notify_login, not
// persist: it forwards to the string overload, which defaults persist to true and would write the
// fake session into the real token store. Sign in through the string overload with persist=false
// so the tests never touch the machine's keychain.
void sign_in_without_persisting(OrcaCloudServiceAgent& agent, const std::string& user_id)
{
    REQUIRE(agent.set_user_session(FAR_FUTURE_JWT, user_id,
                                   /*username=*/"", /*nickname=*/"", /*avatar=*/"",
                                   /*refresh_token=*/"", /*persist=*/false));
}

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
std::string missing_version_response(const std::string& uuid)
{
    json row;
    row["id"]     = uuid;
    row["reason"] = "requested version not found";

    json rows = json::array();
    rows.push_back(std::move(row));

    json root;
    root["not_found"] = std::move(rows);
    return root.dump();
}

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

// The state of the old package after a failed update: nothing about it may have changed.
void check_untouched_state(const fs::path& plugin_dir, const fs::path& entry_path, bool expected_enabled)
{
    REQUIRE(fs::exists(entry_path));
    CHECK(read_text_file(entry_path).find("version = \"1.0\"") != std::string::npos);

    PluginInstallState state;
    REQUIRE(read_install_state(plugin_dir, state));
    CHECK(state.installed_version == "1.0");
    CHECK(state.enabled == expected_enabled);
    REQUIRE(state.capabilities.size() == 1);
    CHECK(state.capabilities.front().first == "Echo");
    CHECK_FALSE(state.capabilities.front().second);
    CHECK(state.permissions.fs_read == std::vector<std::string>{"/old/grant"});
}

// Signs a throwaway agent in for `user_id`, points it at `api_url`, hands it to the manager and
// rebuilds the registry from the data directory. The manager keeps the agent alive, and
// `plugin_system` owns the data directory its config lives in, so neither outlives the test.
PluginManager& start_update_session(ScopedPluginManager& plugin_system, const std::string& user_id,
                                    const std::string& api_url)
{
    auto agent = std::make_shared<OrcaCloudServiceAgent>((plugin_system.python_data_dir.dir / "cloud-agent").string());
    agent->set_api_base_url(api_url);
    sign_in_without_persisting(*agent, user_id);

    PluginManager& manager = PluginManager::instance();
    manager.set_cloud_agent(agent);
    manager.set_cloud_user(user_id);
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    return manager;
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

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

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

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    REQUIRE(manager.update_cloud_plugin(uuid, error, "2.0"));
    INFO("update error: " << error);

    // A package the user deliberately deactivated must not come back activated.
    CHECK_FALSE(manager.is_plugin_loaded(uuid));
    check_updated_state(plugin_dir, /*enabled=*/false);
}

TEST_CASE("Updating a cloud plugin with keep_permissions carries the previous grants over unchanged",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "45454545-5555-6666-7777-888888888888";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", false, {{"Echo", false}}, {"/old/grant"});
    {
        PluginInstallState state;
        REQUIRE(read_install_state(plugin_dir, state));
        state.permissions.full_access = true;
        REQUIRE(write_install_state(plugin_dir, state));
    }

    OneShotHttpServer package_server(cloud_plugin_source("2.0"), "application/octet-stream");
    OneShotHttpServer api_server(download_response(uuid, package_server.url() + "/Cloud_Update_Plugin.py"),
                                 "application/json");

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    REQUIRE(manager.update_cloud_plugin(uuid, error, "2.0", /*keep_permissions=*/true));
    INFO("update error: " << error);

    PluginInstallState state;
    REQUIRE(read_install_state(plugin_dir, state));
    CHECK(state.installed_version == "2.0");
    CHECK_FALSE(state.enabled);
    // Exactly the old grants: nothing dropped, and nothing the user never granted was added.
    CHECK(state.permissions.fs_read == std::vector<std::string>{"/old/grant"});
    CHECK(state.permissions.fs_readwrite.empty());
    CHECK(state.permissions.network_http.empty());
    CHECK(state.permissions.full_access);
}

TEST_CASE("A failed download leaves the installed package and activation intent untouched",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "55555555-6666-7777-8888-999999999999";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", false, {{"Echo", false}}, {"/old/grant"});
    const fs::path entry_path = plugin_dir / "Cloud_Update_Plugin.py";

    OneShotHttpServer api_server(missing_version_response(uuid), "application/json");

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    CHECK_FALSE(manager.update_cloud_plugin(uuid, error, "2.0"));
    CHECK_FALSE(error.empty());

    // The download happens before the unload and replacement, so the old package is still there.
    check_untouched_state(plugin_dir, entry_path, /*expected_enabled=*/false);
}

TEST_CASE("A failed install leaves the installed package and activation intent untouched",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "66666666-7777-8888-9999-aaaaaaaaaaaa";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", false, {{"Echo", false}}, {"/old/grant"});
    const fs::path entry_path = plugin_dir / "Cloud_Update_Plugin.py";

    // A .py package whose PEP 723 block does not parse (an unclosed dependencies array) is
    // downloaded successfully and then rejected by the installer before it moves the old package
    // aside.
    OneShotHttpServer package_server("# /// script\n"
                                     "# dependencies = [\n"
                                     "# ///\n",
                                     "application/octet-stream");
    OneShotHttpServer api_server(download_response(uuid, package_server.url() + "/broken.py"), "application/json");

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    CHECK_FALSE(manager.update_cloud_plugin(uuid, error, "2.0"));
    CHECK_FALSE(error.empty());

    check_untouched_state(plugin_dir, entry_path, /*expected_enabled=*/false);
}

TEST_CASE("A failed install that reloads the previous version does not mark the working plugin erroneous",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "88888888-9999-aaaa-bbbb-cccccccccccc";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", true, {{"Echo", false}}, {"/old/grant"});
    const fs::path entry_path = plugin_dir / "Cloud_Update_Plugin.py";

    // The downloaded package is rejected while its metadata is read, before the installer backs up
    // the working package, so the old version stays in place and is reloaded by the update.
    OneShotHttpServer package_server("# /// script\n"
                                     "# dependencies = [\n"
                                     "# ///\n",
                                     "application/octet-stream");
    OneShotHttpServer api_server(download_response(uuid, package_server.url() + "/broken.py"), "application/json");

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

    std::string load_error;
    manager.load_plugin(uuid, /*skip_deps=*/true);
    REQUIRE(manager.wait_for_plugin_load(uuid, std::chrono::seconds(120), load_error));
    REQUIRE(manager.is_plugin_loaded(uuid));

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    CHECK_FALSE(manager.update_cloud_plugin(uuid, error, "2.0"));
    INFO("update error: " << error);
    REQUIRE_FALSE(error.empty());

    // The update failed, but its rollback put the previous version back in service.
    CHECK(manager.is_plugin_loaded(uuid));
    check_untouched_state(plugin_dir, entry_path, /*expected_enabled=*/true);

    // A running plugin that was restored successfully is not in an error state. Reporting the
    // failed install to the caller must not leave the install error on the plugin's descriptor,
    // where it would turn a working plugin's status into an error.
    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_plugin_descriptor(uuid, descriptor));
    INFO("plugin error: " << descriptor.normalized_error());
    CHECK_FALSE(descriptor.has_error());

    // The capability choice from the old package still applies to the reloaded plugin.
    const auto echo = manager.get_plugin_capability({PluginCapabilityType::Unknown, "Echo", uuid},
                                                    /*only_enabled=*/false);
    REQUIRE(echo != nullptr);
    CHECK_FALSE(echo->is_enabled());

    manager.unload_plugin(uuid);
}

TEST_CASE("A failed update keeps a previously broken plugin's load error visible",
          "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "99999999-aaaa-bbbb-cccc-dddddddddddd";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", true, {{"Echo", false}}, {"/old/grant"});
    const fs::path entry_path = plugin_dir / "Cloud_Update_Plugin.py";

    // Keep the package metadata valid and make the module itself raise on import, so the plugin is
    // enabled but not loaded and carries a real load error.
    {
        std::ofstream out(entry_path.string(), std::ios::binary | std::ios::trunc);
        out << cloud_plugin_source("1.0") << "\nraise RuntimeError(\"load failure marker\")\n";
    }

    OneShotHttpServer package_server("# /// script\n"
                                     "# dependencies = [\n"
                                     "# ///\n",
                                     "application/octet-stream");
    OneShotHttpServer api_server(download_response(uuid, package_server.url() + "/broken.py"), "application/json");

    PluginManager& manager = start_update_session(plugin_system, user_id, api_server.url());

    std::string load_error;
    manager.load_plugin(uuid, /*skip_deps=*/true);
    REQUIRE_FALSE(manager.wait_for_plugin_load(uuid, std::chrono::seconds(120), load_error));
    REQUIRE_FALSE(manager.is_plugin_loaded(uuid));
    REQUIRE_FALSE(load_error.empty());

    PluginDescriptor broken;
    REQUIRE(manager.try_get_plugin_descriptor(uuid, broken));
    REQUIRE(broken.has_error());
    const std::string prior_error = broken.normalized_error();

    ScopedEnvVar no_proxy{"no_proxy", "127.0.0.1"};

    std::string error;
    CHECK_FALSE(manager.update_cloud_plugin(uuid, error, "2.0"));
    INFO("update error: " << error);
    REQUIRE_FALSE(error.empty());

    // The update could not replace the package. The previous one was restored, but it was not
    // loaded before and is not reloaded now, so its real load error must still be visible.
    CHECK_FALSE(manager.is_plugin_loaded(uuid));
    check_untouched_state(plugin_dir, entry_path, /*expected_enabled=*/true);

    PluginDescriptor after;
    REQUIRE(manager.try_get_plugin_descriptor(uuid, after));
    CHECK(after.has_error());
    INFO("plugin error: " << after.normalized_error());
    CHECK(after.normalized_error() == prior_error);
}

TEST_CASE("An update is refused while the plugin is still loading", "[PluginUpdate][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    const std::string user_id = "update-user";
    const std::string uuid    = "77777777-8888-9999-aaaa-bbbbbbbbbbbb";
    const fs::path plugin_dir = write_cloud_package(user_id, uuid, "1.0", true, {{"Echo", false}}, {"/old/grant"});
    const fs::path entry_path = plugin_dir / "Cloud_Update_Plugin.py";
    {
        std::ofstream out(entry_path.string(), std::ios::binary | std::ios::trunc);
        out << cloud_plugin_source("1.0", /*slow=*/true);
    }

    PluginManager& manager = PluginManager::instance();
    manager.set_cloud_user(user_id);
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    manager.load_plugin(uuid, /*skip_deps=*/true);
    REQUIRE(manager.is_plugin_load_in_progress(uuid));

    PluginDescriptor before;
    REQUIRE(manager.try_get_plugin_descriptor(uuid, before));
    REQUIRE_FALSE(before.has_error());

    // The update check runs before the download. No cloud service is configured, so any request
    // that got that far would fail for a different reason; the load refusal must win.
    std::string error;
    CHECK_FALSE(manager.update_cloud_plugin(uuid, error, "2.0"));
    CHECK(error.find("still in progress") != std::string::npos);

    PluginDescriptor after;
    REQUIRE(manager.try_get_plugin_descriptor(uuid, after));
    CHECK_FALSE(after.has_error());

    // The refused update did not delete or unload the loading package.
    check_untouched_state(plugin_dir, entry_path, /*expected_enabled=*/true);

    CHECK(manager.cancel_plugin_load(uuid));
    CHECK(manager.wait_for_all_plugin_loads(std::chrono::seconds(60)));
    CHECK_FALSE(manager.is_plugin_load_in_progress(uuid));

    // The refusal released its update claim: a later attempt reaches the download step instead of
    // being rejected as a concurrent update. No cloud service is configured, so it fails there.
    std::string retry_error;
    CHECK_FALSE(manager.update_cloud_plugin(uuid, retry_error, "2.0"));
    CHECK(retry_error.find("still in progress") == std::string::npos);
    CHECK(retry_error.find("already in progress") == std::string::npos);
}

TEST_CASE("A fake update session is not written to the token store", "[PluginUpdate]")
{
    // The isolated file backend makes persistence observable without touching the keychain.
    ScopedTemporaryDir    secrets("orca-update-secret");
    OrcaCloudServiceAgent agent((secrets.path() / "cloud-agent").string());
    agent.set_use_encrypted_token_file(true);
    agent.set_config_dir(secrets.path().string());

    const fs::path secret_path = secrets.path() / secret_constants::USER_SECRET_FILENAME;

    // The backend does store a secret when asked, so the check below can fail.
    agent.persist_user_secret("refresh-token");
    REQUIRE(fs::exists(secret_path));
    fs::remove(secret_path);

    sign_in_without_persisting(agent, "update-user");
    CHECK(agent.get_user_id() == "update-user");
    CHECK_FALSE(fs::exists(secret_path));
}
