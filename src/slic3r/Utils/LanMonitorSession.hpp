#pragma once

#include "LanMonitorSnapshot.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {

// Pure session state shared by the GUI process owner and its protocol tests.
struct LanMonitorSession
{
    using Clock = LanMonitorStatus::Clock;
    LanMonitorStatus status;
    Clock::time_point retry_at{};
    unsigned backoff = 1;
    std::uint64_t generation = 0;
    bool active = true;

    bool accepts(std::uint64_t token) const { return active && generation == token; }

    void consume(const nlohmann::json& message, std::uint64_t token)
    {
        if (!accepts(token) || !message.is_object()) return;
        const auto kind = message.find("kind");
        if (kind == message.end() || !kind->is_string()) return;
        if (*kind == "sample") {
            if (status.accept_sample(message) && status.live()) backoff = 1;
        } else if (*kind == "connection") {
            const auto state = message.find("state");
            const auto reason = message.find("reason");
            if (state == message.end() || !state->is_number_integer()) return;
            if (*state == 0) status.state = LanMonitorStatus::State::Online;
            else if (reason != message.end() && reason->is_string() && *reason == "authentication")
                status.state = LanMonitorStatus::State::AuthenticationError;
            else status.state = LanMonitorStatus::State::Offline;
        } else if (*kind == "error") {
            status.state = LanMonitorStatus::State::Unavailable;
        }
    }

    void finished(std::uint64_t token, Clock::time_point now = Clock::now())
    {
        if (!accepts(token)) return;
        if (status.state != LanMonitorStatus::State::AuthenticationError && status.state != LanMonitorStatus::State::Unavailable)
            status.state = LanMonitorStatus::State::Offline;
        retry_at = now + std::chrono::seconds(backoff);
        backoff = std::min(backoff * 2, 30u);
    }
};

} // namespace Slic3r
