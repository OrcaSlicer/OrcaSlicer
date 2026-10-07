#include "Client.hpp"
#include "WinPipe.hpp"


namespace Slic3r::BambuBridge {
namespace {

enum class Method {
#define BAMBU_BRIDGE_API(name, type, symbol) name,
#include "Api.def"
#undef BAMBU_BRIDGE_API
};

template<Method M> const char* method_name();
#define BAMBU_BRIDGE_API(name, type, symbol) template<> const char* method_name<Method::name>() { return symbol; }
#include "Api.def"
#undef BAMBU_BRIDGE_API

template<Method M, class F> struct Thunk;
template<Method M, class R, class... A> struct Thunk<M, R (*)(A...)> {
    static R call(A... args) { return Client::instance().invoke<R (*)(A...)>(method_name<M>(), std::forward<A>(args)...); }
};

// These two values are established by the handshake, so polling them does not need IPC.
std::string get_version() { return Client::instance().version(); }
bool check_debug(bool debug) { return Client::instance().debug_consistent(debug); }
int set_queue(void* agent, QueueOnMainFn fn) { return Client::instance().queue_on_main(agent, std::move(fn)); }

} // namespace

Client& Client::instance() { static Client client; return client; }

void Client::start(const std::string& library, std::function<void(const std::string&)> error_handler)
{
    stop();
    { std::lock_guard<std::mutex> lock(m_mutex); m_error_handler = std::move(error_handler); }
    auto session = std::make_shared<Session>();
    auto process = std::make_shared<HelperProcess>(launch_helper(environment(L"ORCA_BAMBU_HELPER")));
    auto pipe = process->pipe;
    session->process = process;
    session->rpc = std::make_unique<Rpc>(
        [pipe](void* bytes, std::size_t size) { pipe->read(bytes, size); },
        [pipe](void* bytes, std::size_t size) { pipe->write(bytes, size); },
        [pipe] { pipe->close(); },
        [session_ptr = session.get()](const std::string& method, const Json& args) -> Json {
            if (method != "callback") throw std::runtime_error("Unknown Bambu reverse method");
            return session_ptr->callbacks.invoke(args);
        });
    session->rpc->start();
#ifdef NDEBUG
    constexpr bool debug = false;
#else
    constexpr bool debug = true;
#endif
    auto hello = session->rpc->request("hello", {{"protocol", protocol_version}, {"library", library}, {"debug", debug}});
    if (hello.at("protocol") != protocol_version || hello.at("version").get<std::string>().substr(0, 8) != "02.08.04")
        throw std::runtime_error("Bambu helper protocol or DLL ABI mismatch (requires 02.08.04)");
    session->version = hello.at("version").get<std::string>();
    session->debug = debug;
    session->exports = hello.at("exports").get<std::set<std::string>>();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session = std::move(session);
}

void Client::stop()
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        session = std::move(m_session);
    }
    if (session) session->rpc->stop();
}

std::shared_ptr<Client::Session> Client::get_session() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_session || !m_session->rpc->running()) throw std::runtime_error("Bambu helper is not connected");
    return m_session;
}
bool Client::running() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session && m_session->rpc->running();
}
std::string Client::version() const
{ try { return get_session()->version; } catch (...) { return "00.00.00.00"; } }
bool Client::debug_consistent(bool debug) const
{ try { return get_session()->debug == debug; } catch (...) { return false; } }
void Client::report_error(const std::string& message)
{
    std::function<void(const std::string&)> handler;
    { std::lock_guard<std::mutex> lock(m_mutex); handler = m_error_handler; }
    if (handler) handler(message);
    else OutputDebugStringA(("Bambu helper: " + message + "\n").c_str());
}

int Client::queue_on_main(void* agent, QueueOnMainFn fn)
{
    try {
        if (agent != agent_token()) return -1;
        get_session()->rpc->request("queue_on_main", bool(fn));
        return 0;
    } catch (const std::exception& e) { report_error(e.what()); return -1; }
}

void* Client::function(const char* export_name) const
{
    try {
        auto session = get_session();
        if (std::string(export_name) == "bambu_network_set_queue_on_main_fn")
            return reinterpret_cast<void*>(&set_queue);
        if (!session->exports.count(export_name)) return nullptr;
        if (std::string(export_name) == "bambu_network_get_version") return reinterpret_cast<void*>(&get_version);
        if (std::string(export_name) == "bambu_network_check_debug_consistent") return reinterpret_cast<void*>(&check_debug);
#define BAMBU_BRIDGE_API(name, type, symbol) \
        if (std::string(export_name) == symbol) return reinterpret_cast<void*>(&Thunk<Method::name, type>::call);
#include "Api.def"
#undef BAMBU_BRIDGE_API
    } catch (...) {}
    return nullptr; // Unsupported exports stay unavailable, never resolve in the native process.
}

} // namespace Slic3r::BambuBridge
