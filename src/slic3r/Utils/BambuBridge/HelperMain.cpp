#include "Bindings.hpp"
#include "WinPipe.hpp"
#include "../BBLNetworkFunctions.hpp"
#include <filesystem>
#include <iostream>

namespace Slic3r::BambuBridge {

class Server {
public:
    ~Server() { shutdown(); }
    Rpc* rpc = nullptr;
    static bool is_job(const std::string& method)
    {
        return method == "bambu_network_bind" || method == "bambu_network_start_print" ||
               method == "bambu_network_start_local_print_with_record" || method == "bambu_network_start_send_gcode_to_sdcard" ||
               method == "bambu_network_start_local_print" || method == "bambu_network_start_sdcard_print";
    }
    Json handle(const std::string& method, const Json& args)
    {
        if (method == "hello") return hello(args);
        if (!m_module) throw std::runtime_error("Bambu helper requires a handshake");
        if (method == "queue_on_main") {
            auto fn = resolve<func_set_queue_on_main_fn>("bambu_network_set_queue_on_main_fn");
            if (fn && m_agent) fn(m_agent, args.get<bool>() ? QueueOnMainFn([this](std::function<void()> closure) {
                // Arbitrary DLL closures belong to this process. Dispatch on a helper-owned
                // thread via a local queue; never attempt to serialize or run one in Orca.
                const auto id = m_closures.add([closure = std::move(closure)](const Json&) { closure(); return Json(); });
                // A local dispatcher consumes these without involving the parent GUI thread.
                enqueue_closure(id);
            }) : QueueOnMainFn{});
            return nullptr;
        }
        if (method == "bambu_network_create_agent") {
            if (!m_agent) {
                if (!args.is_array() || args.size() != 1) throw std::runtime_error("Invalid create-agent arguments");
                m_agent = resolve<func_create_agent>(method.c_str())(args.at(0).get<std::string>());
                start_closures();
                // Provide a helper-side queue even when Orca has no Bambu cloud provider.
                handle("queue_on_main", true);
            }
            return {{"value", m_agent ? 1 : 0}, {"args", args}};
        }
        if (method == "bambu_network_destroy_agent") {
            if (args != Json::array({1}) || !m_agent) throw std::runtime_error("Invalid destroy-agent arguments");
            if (rpc->has_jobs()) throw std::runtime_error("Bambu helper still has active jobs; disconnect to cancel them");
            int result = destroy_agent();
            return {{"value", result}, {"args", args}};
        }
#define BAMBU_BRIDGE_API(name, type, symbol) \
        if (method == symbol) { \
            auto fn = resolve<type>(symbol); \
            if (!fn) throw std::runtime_error("Bambu DLL does not export " symbol); \
            return Binding<type>::serve(fn, args, *rpc, m_agent); \
        }
#include "Api.def"
#undef BAMBU_BRIDGE_API
        throw std::runtime_error("Unsupported Bambu helper method");
    }
    void start_closures()
    {
        if (m_closure_thread.joinable()) return;
        m_closure_stopped = false;
        m_closure_thread = std::thread([this] {
            std::unique_lock<std::mutex> lock(m_closure_mutex);
            while (true) {
                m_closure_condition.wait(lock, [this] { return m_closure_stopped || !m_closure_queue.empty(); });
                if (m_closure_stopped && m_closure_queue.empty()) break;
                const auto id = m_closure_queue.front();
                m_closure_queue.pop_front();
                lock.unlock();
                try { m_closures.invoke({{"callback", id}, {"args", Json::array()}}); } catch (...) {}
                m_closures.erase({id});
                lock.lock();
            }
        });
    }
    void shutdown()
    {
        // RPC is already disconnected, so reverse callbacks cannot hold DLL workers forever.
        destroy_agent();
        if (m_module) { FreeLibrary(m_module); m_module = nullptr; }
    }
private:
    int destroy_agent()
    {
        if (m_agent) {
            auto queue = resolve<func_set_queue_on_main_fn>("bambu_network_set_queue_on_main_fn");
            if (queue) queue(m_agent, nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(m_closure_mutex);
            m_closure_stopped = true;
            m_closure_condition.notify_all();
        }
        if (m_closure_thread.joinable()) m_closure_thread.join();
        int result = 0;
        if (m_agent) {
            auto destroy = resolve<func_destroy_agent>("bambu_network_destroy_agent");
            if (destroy) result = destroy(m_agent);
            m_agent = nullptr;
        }
        return result;
    }
    template<class F> F resolve(const char* name) const { return reinterpret_cast<F>(GetProcAddress(m_module, name)); }
    Json hello(const Json& args)
    {
        if (m_module) throw std::runtime_error("Bambu handshake already completed");
        if (args.at("protocol") != protocol_version) throw std::runtime_error("Bambu helper protocol mismatch");
        const auto library = utf16(args.at("library").get<std::string>());
        if (!std::filesystem::path(library).is_absolute()) throw std::runtime_error("Bambu DLL path must be absolute");
        m_module = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) throw win_error("Load Bambu DLL (check helper/DLL architecture and runtime dependencies)");
        const auto version = resolve<func_get_version>("bambu_network_get_version");
        const auto check = resolve<func_check_debug_consistent>("bambu_network_check_debug_consistent");
        if (!version || !check || !resolve<func_create_agent>("bambu_network_create_agent") ||
            !resolve<func_destroy_agent>("bambu_network_destroy_agent")) throw std::runtime_error("Missing required Bambu exports");
        if (!check(args.at("debug").get<bool>())) throw std::runtime_error("Bambu DLL debug/release runtime mismatch");
        const auto loaded_version = version();
        if (loaded_version.substr(0, 8) != "02.08.04")
            throw std::runtime_error("Bambu helper supports the 02.08.04 ABI only");
        Json exports = Json::array();
#define BAMBU_BRIDGE_API(name, type, symbol) if (resolve<type>(symbol)) exports.push_back(symbol);
#include "Api.def"
#undef BAMBU_BRIDGE_API
        return {{"protocol", protocol_version}, {"version", loaded_version}, {"exports", exports}};
    }
    void enqueue_closure(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(m_closure_mutex);
        if (m_closure_stopped || m_closure_queue.size() >= 1024) { m_closures.erase({id}); return; }
        m_closure_queue.push_back(id);
        m_closure_condition.notify_one();
    }
    HMODULE m_module = nullptr;
    void* m_agent = nullptr;
    Callbacks m_closures;
    std::mutex m_closure_mutex;
    std::condition_variable m_closure_condition;
    std::deque<std::uint64_t> m_closure_queue;
    std::thread m_closure_thread;
    bool m_closure_stopped = false;
};

} // namespace Slic3r::BambuBridge

int wmain(int argc, wchar_t** argv)
{
    using namespace Slic3r::BambuBridge;
    if (argc != 3 || std::wstring(argv[1]) != L"--pipe") return 2;
    try {
        auto pipe = connect_to_parent(argv[2]);
        Server server;
        Rpc rpc([pipe](void* bytes, std::size_t size) { pipe->read(bytes, size); },
                [pipe](void* bytes, std::size_t size) { pipe->write(bytes, size); },
                [pipe] { pipe->close(); },
                [&server](const std::string& method, const Json& args) { return server.handle(method, args); },
                Server::is_job);
        server.rpc = &rpc;
        rpc.start();
        // Wait on the parent connection; no GUI or privileged Windows service is required.
        while (rpc.running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rpc.stop();
        server.shutdown();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Bambu helper: " << e.what() << '\n';
        return 1;
    }
}
