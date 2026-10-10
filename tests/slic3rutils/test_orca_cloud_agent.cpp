#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch_all.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include "slic3r/Utils/ICloudServiceAgent.hpp"
#include "slic3r/Utils/OrcaCloudServiceAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// The encrypted token file is the one secret backend a test can observe without a system
// keychain. Every agent pointed at the same directory shares it, like separate app instances
// share the keychain entry.
std::unique_ptr<OrcaCloudServiceAgent> make_file_backed_agent(const fs::path& dir)
{
    auto agent = std::make_unique<OrcaCloudServiceAgent>(dir.string());
    agent->set_use_encrypted_token_file(true);
    agent->set_config_dir(dir.string());
    return agent;
}

fs::path secret_file(const fs::path& dir) { return dir / secret_constants::USER_SECRET_FILENAME; }

nlohmann::json flat_session_json(const nlohmann::json& fields)
{
    nlohmann::json session = {
        {"access_token", "test-token"},
        {"user_id", "test-user-id"}
    };
    session.update(fields);
    return session;
}

nlohmann::json nested_session_json(const nlohmann::json& metadata)
{
    return {
        {"access_token", "test-token"},
        {"user", {
            {"id", "test-user-id"},
            {"user_metadata", metadata}
        }}
    };
}

// set_user_session() persists the session, so it goes to a throwaway token file rather than the
// system keychain of whoever runs the tests.
std::string resolved_display_name(const nlohmann::json& session)
{
    ScopedTemporaryDir dir("orca-secret");
    auto               agent = make_file_backed_agent(dir.path());
    REQUIRE(agent->set_user_session(session, false));
    return agent->get_user_nickname();
}

using namespace std::chrono_literals;
using boost::asio::ip::tcp;

// A loopback HTTP server for the health check to talk to. It answers every request according to
// the current mode and records what it was sent.
class LoopbackServer
{
public:
    enum class Mode { Ok, Unavailable, Silent };

    struct Request
    {
        std::string request_line;
        bool        has_authorization{false};
    };

    LoopbackServer() : m_acceptor(m_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
    {
        accept_next();
        m_thread = std::thread([this] { m_io.run(); });
    }

    ~LoopbackServer()
    {
        m_io.stop();
        m_thread.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(m_acceptor.local_endpoint().port()); }
    void        set_mode(Mode mode) { m_mode = mode; }

    bool wait_for_accepts(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [&] { return m_connections.size() >= count; });
    }

    std::vector<Request> requests() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests;
    }

private:
    struct Connection
    {
        explicit Connection(tcp::socket socket) : socket(std::move(socket)) {}
        tcp::socket            socket;
        boost::asio::streambuf buffer;
    };

    void accept_next()
    {
        m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (ec)
                return;
            auto connection = std::make_shared<Connection>(std::move(socket));
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // Held here so a Silent connection stays open until the client gives up.
                m_connections.push_back(connection);
            }
            m_cv.notify_all();
            read_request(connection);
            accept_next();
        });
    }

    void read_request(const std::shared_ptr<Connection>& connection)
    {
        boost::asio::async_read_until(connection->socket, connection->buffer, "\r\n\r\n",
                                      [this, connection](boost::system::error_code ec, size_t) {
            if (ec)
                return;
            std::istream in(&connection->buffer);
            Request      request;
            std::string  line;
            std::getline(in, line);
            request.request_line = line.substr(0, line.find('\r'));
            while (std::getline(in, line) && line != "\r")
                request.has_authorization |= boost::istarts_with(line, "authorization:");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_requests.push_back(request);
            }

            const Mode mode = m_mode;
            if (mode == Mode::Silent)
                return;
            const std::string_view response = mode == Mode::Ok ?
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}" :
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            boost::asio::async_write(connection->socket, boost::asio::buffer(response),
                                     [connection](boost::system::error_code, size_t) {
                boost::system::error_code ignored;
                connection->socket.shutdown(tcp::socket::shutdown_both, ignored);
            });
        });
    }

    // Declared first so it outlives the sockets that use it.
    boost::asio::io_context                  m_io;
    tcp::acceptor                            m_acceptor;
    std::thread                              m_thread;
    std::atomic<Mode>                        m_mode{Mode::Ok};
    mutable std::mutex                       m_mutex;
    std::condition_variable                  m_cv;
    std::vector<Request>                     m_requests;
    std::vector<std::shared_ptr<Connection>> m_connections;
};

// Sets an environment variable for the lifetime of the guard and restores the previous value.
class ScopedEnvVar
{
public:
    ScopedEnvVar(std::string name, const std::string& value) : m_name(std::move(name))
    {
        if (const char* old = boost::nowide::getenv(m_name.c_str()))
            m_previous = old;
        boost::nowide::setenv(m_name.c_str(), value.c_str(), 1);
    }
    ~ScopedEnvVar()
    {
        if (m_previous)
            boost::nowide::setenv(m_name.c_str(), m_previous->c_str(), 1);
        else
            boost::nowide::unsetenv(m_name.c_str());
    }

private:
    std::string                m_name;
    std::optional<std::string> m_previous;
};

// What the agent reported through its callbacks, recorded on whichever thread delivers it so the
// assertions can run on the test thread.
struct CallbackLog
{
    void add_connected(int return_code, int reason_code)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected.emplace_back(return_code, reason_code);
        }
        cv.notify_all();
    }

    void add_http_error(unsigned http_code)
    {
        std::lock_guard<std::mutex> lock(mutex);
        http_errors.push_back(http_code);
    }

    void add_queued(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queued.push_back(std::move(task));
        }
        cv.notify_all();
    }

    bool wait_for_connected(size_t count, std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return connected.size() >= count; });
    }

    bool wait_for_queued(size_t count, std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return queued.size() >= count; });
    }

    std::mutex                         mutex;
    std::condition_variable            cv;
    std::vector<std::pair<int, int>>   connected;
    std::vector<unsigned>              http_errors;
    std::vector<std::function<void()>> queued;
};

// An agent whose API and auth endpoints both point at a loopback server, with its callbacks
// recorded. Members are declared so the agent, and the health check thread it owns, is destroyed
// before the server, the log and the storage it uses.
struct HealthCheckFixture
{
    HealthCheckFixture() : agent(make_file_backed_agent(dir.path()))
    {
        agent->set_api_base_url(server.url());
        agent->set_auth_base_url(server.url());
        agent->set_on_server_connected_fn([this](CloudEvent, int return_code, int reason_code) {
            log.add_connected(return_code, reason_code);
        });
        agent->set_on_http_error_fn([this](CloudEvent, unsigned http_code, std::string) { log.add_http_error(http_code); });
    }

    // Defers callbacks the way the GUI's CallAfter does, instead of running them on the worker.
    void queue_callbacks() { agent->set_queue_on_main_fn([this](std::function<void()> task) { log.add_queued(std::move(task)); }); }

    // Starts the next health check, retrying while a call is still folded into the previous one:
    // its callback fires before the worker thread finishes.
    bool start_check(size_t expected_accepts)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            agent->refresh_connection();
            if (server.wait_for_accepts(expected_accepts, 10ms))
                return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    // Loopback traffic must not go through a proxy configured in the environment. curl reads no_proxy
    // before NO_PROXY.
    ScopedEnvVar                           no_proxy{"no_proxy", "127.0.0.1"};
    LoopbackServer                         server;
    CallbackLog                            log;
    ScopedTemporaryDir                     dir{"orca-health"};
    std::unique_ptr<OrcaCloudServiceAgent> agent;
};

} // namespace

TEST_CASE("Logging out removes the secret this instance saved", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    auto agent = make_file_backed_agent(dir.path());

    agent->persist_user_secret("refresh-token");
    REQUIRE(fs::exists(secret_file(dir.path())));

    agent->user_logout(false);
    CHECK_FALSE(fs::exists(secret_file(dir.path())));
}

TEST_CASE("Logging out removes a secret this instance loaded from the store", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    make_file_backed_agent(dir.path())->persist_user_secret("refresh-token");

    auto        agent = make_file_backed_agent(dir.path());
    std::string secret;
    REQUIRE(agent->load_user_secret(secret));
    CHECK(secret == "refresh-token");

    agent->user_logout(false);
    CHECK_FALSE(fs::exists(secret_file(dir.path())));
}

TEST_CASE("Logging out leaves a secret this instance never loaded or saved alone", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    make_file_backed_agent(dir.path())->persist_user_secret("refresh-token");

    // A logged-out instance is asked to log out on every login-status poll.
    auto other = make_file_backed_agent(dir.path());
    other->user_logout(false);
    other->user_logout(false);
    CHECK(fs::exists(secret_file(dir.path())));

    std::string secret;
    REQUIRE(make_file_backed_agent(dir.path())->load_user_secret(secret));
    CHECK(secret == "refresh-token");
}

TEST_CASE("Logging out leaves a secret this instance could not read alone", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    // Written under another encryption key, e.g. by another OS user sharing the data directory.
    fs::ofstream(secret_file(dir.path())) << "v2:0000:not-a-payload-this-user-can-decrypt";

    auto        agent = make_file_backed_agent(dir.path());
    std::string secret;
    REQUIRE_FALSE(agent->load_user_secret(secret));

    agent->user_logout(false);
    CHECK(fs::exists(secret_file(dir.path())));
}

TEST_CASE("Orca cloud flat session resolves display name consistently", "[OrcaCloudServiceAgent]")
{
    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"display_name", "Display Name"},
        {"nickname", "Nickname"}
    })) == "Display Name");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"nickname", "Nickname"}
    })) == "Nickname");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"full_name", "Full Name"}
    })) == "Full Name");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"name", "Provider Name"}
    })) == "Provider Name");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"}
    })) == "orca_username");
}

TEST_CASE("Orca cloud nested session resolves display name consistently", "[OrcaCloudServiceAgent]")
{
    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"display_name", "Display Name"},
        {"nickname", "Nickname"}
    })) == "Display Name");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"nickname", "Nickname"}
    })) == "Nickname");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"full_name", "Full Name"}
    })) == "Full Name");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"name", "Provider Name"}
    })) == "Provider Name");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"}
    })) == "orca_username");
}

TEST_CASE_METHOD(HealthCheckFixture, "Refreshing the server connection returns before the health check finishes", "[OrcaCloudServiceAgent]")
{
    server.set_mode(LoopbackServer::Mode::Silent);

    const auto start = std::chrono::steady_clock::now();
    CHECK(agent->refresh_connection() == BAMBU_NETWORK_SUCCESS);
    // A synchronous check would wait for the request's 30 s timeout.
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    std::lock_guard<std::mutex> lock(log.mutex);
    CHECK(log.connected.empty());
}

TEST_CASE_METHOD(HealthCheckFixture, "Successive health checks follow the server's state", "[OrcaCloudServiceAgent]")
{
    server.set_mode(LoopbackServer::Mode::Ok);
    REQUIRE(start_check(1));
    REQUIRE(log.wait_for_connected(1));
    CHECK(agent->is_server_connected());

    server.set_mode(LoopbackServer::Mode::Unavailable);
    REQUIRE(start_check(2));
    REQUIRE(log.wait_for_connected(2));
    CHECK_FALSE(agent->is_server_connected());

    server.set_mode(LoopbackServer::Mode::Ok);
    REQUIRE(start_check(3));
    REQUIRE(log.wait_for_connected(3));
    CHECK(agent->is_server_connected());

    std::lock_guard<std::mutex> lock(log.mutex);
    CHECK(log.connected == std::vector<std::pair<int, int>>{{0, 200}, {-1, 503}, {0, 200}});
    CHECK(log.http_errors == std::vector<unsigned>{503});
}

TEST_CASE_METHOD(HealthCheckFixture, "A refresh while a health check is in flight starts no second check", "[OrcaCloudServiceAgent]")
{
    server.set_mode(LoopbackServer::Mode::Silent);

    agent->refresh_connection();
    REQUIRE(server.wait_for_accepts(1, 5s));

    agent->refresh_connection();
    agent->refresh_connection();
    CHECK_FALSE(server.wait_for_accepts(2, 200ms));
}

TEST_CASE_METHOD(HealthCheckFixture, "Stopping the health check cancels the request in flight", "[OrcaCloudServiceAgent]")
{
    server.set_mode(LoopbackServer::Mode::Silent);
    queue_callbacks();

    agent->refresh_connection();
    REQUIRE(server.wait_for_accepts(1, 5s));

    const auto start = std::chrono::steady_clock::now();
    agent->stop_health_check();
    // Well under the request's 30 s timeout.
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK(agent->refresh_connection() == BAMBU_NETWORK_ERR_CANCELED);

    std::lock_guard<std::mutex> lock(log.mutex);
    CHECK(log.connected.empty());
    CHECK(log.http_errors.empty());
    CHECK(log.queued.empty());
}

TEST_CASE_METHOD(HealthCheckFixture, "Connecting to the server reports the result synchronously", "[OrcaCloudServiceAgent]")
{
    server.set_mode(LoopbackServer::Mode::Ok);
    CHECK(agent->connect_server() == BAMBU_NETWORK_SUCCESS);
    CHECK(agent->is_server_connected());

    server.set_mode(LoopbackServer::Mode::Unavailable);
    CHECK(agent->connect_server() == BAMBU_NETWORK_ERR_CONNECTION_TO_SERVER_FAILED);
    CHECK_FALSE(agent->is_server_connected());
}

TEST_CASE_METHOD(HealthCheckFixture, "A health check result queued before stopping is still delivered and nothing is queued after", "[OrcaCloudServiceAgent]")
{
    server.set_mode(LoopbackServer::Mode::Ok);
    queue_callbacks();

    agent->refresh_connection();
    REQUIRE(log.wait_for_queued(1));
    agent->stop_health_check();

    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lock(log.mutex);
        REQUIRE(log.queued.size() == 1);
        task = log.queued.front();
    }
    // The GUI may run a task it queued after the agent is gone; the task must not need the agent.
    agent.reset();
    task();

    std::lock_guard<std::mutex> lock(log.mutex);
    CHECK(log.connected == std::vector<std::pair<int, int>>{{0, 200}});
}

TEST_CASE_METHOD(HealthCheckFixture, "The health check does no token work", "[OrcaCloudServiceAgent]")
{
    // A non-JWT access token has no expiry, so the session reads as due for a refresh.
    REQUIRE(agent->set_user_session(flat_session_json({{"refresh_token", "test-refresh-token"}}), false));
    server.set_mode(LoopbackServer::Mode::Ok);

    REQUIRE(start_check(1));
    REQUIRE(log.wait_for_connected(1));
    CHECK(agent->connect_server() == BAMBU_NETWORK_SUCCESS);

    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    for (const auto& request : requests) {
        CHECK(boost::starts_with(request.request_line, "GET /api/v1/health "));
        CHECK_FALSE(request.has_authorization);
    }
    CHECK(agent->is_user_login());
    CHECK(agent->get_access_token() == "test-token");
}
