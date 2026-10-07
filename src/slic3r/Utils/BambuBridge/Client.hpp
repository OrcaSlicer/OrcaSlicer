#pragma once

#include "Bindings.hpp"
#include "../BBLNetworkFunctions.hpp"
#include <set>

namespace Slic3r::BambuBridge {

class Client {
public:
    static Client& instance();
    // Throws with a diagnostic on startup/protocol/DLL failure. No native DLL fallback.
    void start(const std::string& library, std::function<void(const std::string&)> error_handler = {});
    void stop();
    bool running() const;
    void* function(const char* export_name) const;
    std::string version() const;
    bool debug_consistent(bool debug) const;
    int queue_on_main(void*, QueueOnMainFn); // Closures stay in the helper; no code pointers cross IPC.

    template<class F, class... A> auto invoke(const std::string& method, A&&... args)
    {
        using Result = decltype(std::declval<F>()(args...));
        try {
            auto session = get_session();
            std::vector<std::uint64_t> ids;
            const bool setter = method.find("bambu_network_set_") == 0;
            std::unique_lock<std::mutex> registration(session->registration_mutex, std::defer_lock);
            if (setter) registration.lock();
            try {
                if constexpr (std::is_void_v<Result>) {
                    Binding<F>::call(*session->rpc, session->callbacks, method, ids, args...);
                    session->callbacks.erase(ids);
                } else {
                    auto result = Binding<F>::call(*session->rpc, session->callbacks, method, ids, args...);
                    if (setter) {
                        session->callbacks.erase(session->registrations[method]);
                        session->registrations[method] = std::move(ids);
                    } else session->callbacks.erase(ids);
                    return result;
                }
            } catch (...) { session->callbacks.erase(ids); throw; }
        } catch (const std::exception& e) {
            report_error(e.what());
            if constexpr (std::is_same_v<Result, int>) return BAMBU_NETWORK_ERR_INVALID_RESULT;
            else if constexpr (!std::is_void_v<Result>) return Result{};
        }
    }

private:
    struct Session {
        Callbacks callbacks;
        std::mutex registration_mutex;
        std::map<std::string, std::vector<std::uint64_t>> registrations;
        std::unique_ptr<Rpc> rpc;
        // Keep process/pipe ownership opaque here so the app header remains independent of Win32.
        std::shared_ptr<void> process;
        std::set<std::string> exports;
        std::string version;
        bool debug = false;
        ~Session() { if (rpc) rpc->stop(); }
    };
    std::shared_ptr<Session> get_session() const;
    void report_error(const std::string& message);
    mutable std::mutex m_mutex;
    std::shared_ptr<Session> m_session;
    std::function<void(const std::string&)> m_error_handler;
};

} // namespace Slic3r::BambuBridge
