#pragma once

// OpenBambuSsdp - SSDP discovery for BambuLab printers on LAN.
//
// Protocol knowledge derived from:
//   https://github.com/ClusterM/open-bamboo-networking (AGPL-3.0)
//   Copyright (C) 2026 Alexey Cluster and contributors
//   https://github.com/Doridian/OpenBambuAPI
//
// BambuLab printers advertise on multicast 239.255.255.250:1990 using
// SSDP NOTIFY packets with Bambu-specific headers (USN=serial,
// DevModel.bambu.com, DevName.bambu.com, Location=IP, etc.).

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

/// Callback receiving a JSON string in DeviceManager::on_machine_alive format
using SsdpMessageCb = std::function<void(std::string dev_info_json)>;

/// Listens for BambuLab SSDP announcements on the local network.
class OpenBambuSsdp {
public:
    OpenBambuSsdp();
    ~OpenBambuSsdp();

    // Non-copyable
    OpenBambuSsdp(const OpenBambuSsdp&) = delete;
    OpenBambuSsdp& operator=(const OpenBambuSsdp&) = delete;

    /// Start listening for SSDP messages.
    /// Returns true if the listener started successfully.
    bool start(SsdpMessageCb cb);

    /// Stop listening.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void run_loop();
    std::string parse_to_json(const char* data, size_t len);

    std::atomic<bool> running_{false};
    std::thread       worker_;
    SsdpMessageCb     callback_;
    std::mutex        cb_mutex_;

#ifdef _WIN32
    uintptr_t         sock_ = ~static_cast<uintptr_t>(0);
#else
    int               sock_ = -1;
#endif
};

} // namespace Slic3r
