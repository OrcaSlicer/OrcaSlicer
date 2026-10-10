#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Slic3r::BambuBridge {

using Json = nlohmann::json;
constexpr unsigned protocol_version = 1;
constexpr std::uint32_t max_frame_size = 16 * 1024 * 1024;

// Transport functions transfer exactly size bytes or throw. close must interrupt read/write.
// One reader and one dispatcher per endpoint keep reverse callbacks live during a DLL call.
class Rpc {
public:
    using Transfer = std::function<void(void*, std::size_t)>;
    using Handler = std::function<Json(const std::string&, const Json&)>;
    using Background = std::function<bool(const std::string&)>;
    Rpc(Transfer read, Transfer write, std::function<void()> close, Handler handler, Background background = {})
        : m_read(std::move(read)), m_write(std::move(write)), m_close(std::move(close)), m_handler(std::move(handler)), m_background(std::move(background)) {}
    ~Rpc() { stop(); }
    Rpc(const Rpc&) = delete;
    Rpc& operator=(const Rpc&) = delete;

    void start()
    {
        m_running = true;
        m_dispatcher = std::thread([this] { dispatch_loop(); });
        m_reader = std::thread([this] { read_loop(); });
    }
    bool running() const { return m_running; }
    bool has_jobs() const { return m_active_jobs != 0; }
    void stop()
    {
        std::lock_guard<std::mutex> lock(m_stop_mutex);
        disconnect();
        if (m_reader.joinable()) m_reader.join();
        if (m_dispatcher.joinable()) m_dispatcher.join();
        for (auto& job : m_jobs) if (job.thread.joinable()) job.thread.join();
        m_jobs.clear();
    }
    Json request(const std::string& method, Json args, std::chrono::milliseconds timeout = std::chrono::seconds(30))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto pending = std::make_shared<Pending>();
        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) throw std::runtime_error("Bambu helper disconnected");
            id = ++m_next_id;
            m_pending.emplace(id, pending);
        }
        try {
            send({{"id", id}, {"method", method}, {"args", std::move(args)}});
            std::unique_lock<std::mutex> lock(m_mutex);
            while (m_running && !pending->ready) {
                // A callback may synchronously call back into the other endpoint. Pump nested
                // requests on the dispatcher only; ordinary caller threads never run callbacks.
                if (s_dispatching == this && !m_incoming.empty()) {
                    Json incoming = std::move(m_incoming.front());
                    m_incoming.pop_front();
                    lock.unlock();
                    dispatch(incoming);
                    lock.lock();
                } else if (m_condition.wait_until(lock, deadline) == std::cv_status::timeout) {
                    lock.unlock();
                    disconnect(); // Outcome may be unknown. Never retry a timed-out print.
                    throw std::runtime_error("Bambu helper request timed out: " + method);
                }
                if (std::chrono::steady_clock::now() >= deadline && !pending->ready) {
                    lock.unlock();
                    disconnect();
                    throw std::runtime_error("Bambu helper request timed out: " + method);
                }
            }
            m_pending.erase(id);
            if (!pending->ready) throw std::runtime_error("Bambu helper disconnected during " + method);
            if (pending->message.contains("error"))
                throw std::runtime_error(pending->message.at("error").get<std::string>());
            return pending->message.at("result");
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.erase(id);
            throw;
        }
    }

private:
    struct Pending { bool ready = false; Json message; };
    inline static thread_local Rpc* s_dispatching = nullptr;
    void disconnect()
    {
        bool was_running;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            was_running = m_running.exchange(false);
        }
        if (was_running) {
            m_close();
            m_condition.notify_all();
        }
    }
    void send(const Json& message)
    {
        auto bytes = message.dump();
        if (bytes.empty() || bytes.size() > max_frame_size) throw std::runtime_error("Invalid Bambu RPC frame size");
        // Little-endian fixed-width framing is independent of pointer size and C++ layout.
        std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
        unsigned char header[4] = {static_cast<unsigned char>(size), static_cast<unsigned char>(size >> 8),
                                   static_cast<unsigned char>(size >> 16), static_cast<unsigned char>(size >> 24)};
        std::lock_guard<std::mutex> lock(m_write_mutex);
        try {
            m_write(header, sizeof(header));
            m_write(bytes.data(), bytes.size());
        } catch (...) { disconnect(); throw; }
    }
    void read_loop()
    {
        try {
            while (m_running) {
                unsigned char header[4];
                m_read(header, sizeof(header));
                const std::uint32_t size = std::uint32_t(header[0]) | (std::uint32_t(header[1]) << 8) |
                                          (std::uint32_t(header[2]) << 16) | (std::uint32_t(header[3]) << 24);
                if (!size || size > max_frame_size) throw std::runtime_error("Invalid Bambu RPC frame size");
                std::string bytes(size, '\0');
                m_read(bytes.data(), size);
                auto message = Json::parse(bytes);
                const auto id = message.at("id").get<std::uint64_t>();
                std::lock_guard<std::mutex> lock(m_mutex);
                if (message.contains("method")) {
                    if (!message.at("method").is_string() || !message.contains("args"))
                        throw std::runtime_error("Malformed Bambu RPC request");
                    if (m_incoming.size() >= 1024) throw std::runtime_error("Bambu RPC queue overflow");
                    m_incoming.push_back(std::move(message));
                } else {
                    auto it = m_pending.find(id);
                    if (it == m_pending.end()) throw std::runtime_error("Unexpected Bambu RPC response");
                    it->second->message = std::move(message);
                    it->second->ready = true;
                }
                m_condition.notify_all();
            }
        } catch (...) { disconnect(); }
    }
    void dispatch(const Json& message, bool background_job = false)
    {
        Json response = {{"id", message.at("id")}};
        try { response["result"] = m_handler(message.at("method").get<std::string>(), message.at("args")); }
        catch (const std::exception& e) { response["error"] = e.what(); }
        catch (...) { response["error"] = "Unknown Bambu helper error"; }
        if (background_job) --m_active_jobs; // The DLL call has finished before its result reaches the client.
        try { send(response); } catch (...) { disconnect(); }
    }
    void dispatch_loop()
    {
        s_dispatching = this;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running) {
            m_condition.wait(lock, [this] { return !m_running || !m_incoming.empty(); });
            if (!m_running) break;
            Json message = std::move(m_incoming.front());
            m_incoming.pop_front();
            lock.unlock();
            if (m_background && m_background(message.at("method").get<std::string>())) {
                for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                    if (*it->finished) { it->thread.join(); it = m_jobs.erase(it); }
                    else ++it;
                }
                if (m_jobs.size() >= 4) {
                    try { send({{"id", message.at("id")}, {"error", "Bambu helper has too many active jobs"}}); }
                    catch (...) { disconnect(); }
                } else {
                    auto finished = std::make_shared<std::atomic<bool>>(false);
                    ++m_active_jobs;
                    m_jobs.push_back({std::thread([this, message = std::move(message), finished] {
                        dispatch(message, true);
                        *finished = true;
                    }), finished});
                }
            } else dispatch(message);
            lock.lock();
        }
        s_dispatching = nullptr;
    }
    Transfer m_read, m_write;
    std::function<void()> m_close;
    Handler m_handler;
    Background m_background;
    struct Job { std::thread thread; std::shared_ptr<std::atomic<bool>> finished; };
    std::vector<Job> m_jobs;
    std::atomic<bool> m_running{false};
    std::atomic<unsigned> m_active_jobs{0};
    std::thread m_reader, m_dispatcher;
    std::mutex m_mutex, m_write_mutex, m_stop_mutex;
    std::condition_variable m_condition;
    std::uint64_t m_next_id = 0;
    std::map<std::uint64_t, std::shared_ptr<Pending>> m_pending;
    std::deque<Json> m_incoming;
};

} // namespace Slic3r::BambuBridge
