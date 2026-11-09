#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace Slic3r {

// Monitoring consumes only job telemetry. MachineObject::parse_json also sends commands and
// updates the active Device UI, so background observers must not use that parser as a callback.
struct LanMonitorSnapshot
{
    std::string job_id;
    std::string job_name;
    std::string print_state;
    std::optional<int> progress;
    std::optional<int> remaining_seconds;

    static std::optional<int> integer(const nlohmann::json& value, int maximum)
    {
        try {
            std::int64_t number;
            if (value.is_string()) {
                const auto& text = value.get_ref<const std::string&>();
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return {};
                std::size_t end = 0;
                number = std::stoll(text, &end);
                if (end != text.size()) return {};
            } else if (value.is_number_unsigned()) {
                const auto unsigned_number = value.get<std::uint64_t>();
                if (unsigned_number > static_cast<std::uint64_t>(maximum)) return {};
                return static_cast<int>(unsigned_number);
            } else if (value.is_number_integer()) {
                number = value.get<std::int64_t>();
            } else {
                return {};
            }
            if (number < 0 || number > maximum) return {};
            return static_cast<int>(number);
        } catch (const std::exception&) {
            return {};
        }
    }

    bool update(const nlohmann::json& report)
    {
        const auto item = report.find("print");
        if (item == report.end() || !item->is_object()) return false;
        const auto& print = *item;
        const auto id = print.find("subtask_id");
        const auto name = print.find("subtask_name");
        // LAN jobs commonly reuse subtask_id "0"; a changed name also identifies a new job.
        if ((id != print.end() && id->is_string() && *id != job_id) ||
            (name != print.end() && name->is_string() && !job_name.empty() && *name != job_name)) {
            job_name.clear();
            print_state.clear();
            progress.reset();
            remaining_seconds.reset();
        }
        if (id != print.end() && id->is_string()) job_id = id->get<std::string>();
        if (name != print.end() && name->is_string())
            job_name = name->get<std::string>();
        if (auto state = print.find("gcode_state"); state != print.end() && state->is_string())
            print_state = state->get<std::string>();
        if (auto value = print.find("mc_percent"); value != print.end())
            if (auto parsed = integer(*value, 100)) progress = parsed;
        if (auto value = print.find("mc_remaining_time"); value != print.end())
            if (auto parsed = integer(*value, std::numeric_limits<int>::max() / 60))
                remaining_seconds = *parsed * 60;
        return true;
    }

    nlohmann::json to_json() const
    {
        return {{"job_id", job_id}, {"gcode_state", print_state}, {"subtask_name", job_name},
                {"progress", progress ? nlohmann::json(*progress) : nlohmann::json(nullptr)},
                {"remaining_seconds", remaining_seconds ? nlohmann::json(*remaining_seconds) : nlohmann::json(nullptr)}};
    }
};

struct LanMonitorStatus
{
    enum class State { Connecting, Online, Offline, AuthenticationError, Unavailable };
    using Clock = std::chrono::steady_clock;
    State state = State::Connecting;
    LanMonitorSnapshot job;
    bool received = false;
    Clock::time_point last_report{};

    bool live(Clock::time_point now = Clock::now()) const
    {
        return state == State::Online && received && now - last_report <= std::chrono::seconds(30);
    }

    bool accept_sample(const nlohmann::json& sample, Clock::time_point now = Clock::now())
    {
        if (!sample.is_object()) return false;
        auto count = sample.find("reports");
        auto age = sample.find("age_seconds");
        auto status = sample.find("status");
        if (count == sample.end() || age == sample.end() || status == sample.end() || !status->is_object()) return false;
        auto reports = LanMonitorSnapshot::integer(*count, std::numeric_limits<int>::max());
        auto seconds = LanMonitorSnapshot::integer(*age, std::numeric_limits<int>::max());
        if (!reports || *reports == 0 || !seconds) return false;
        for (const char* key : {"job_id", "gcode_state", "subtask_name"})
            if (status->contains(key) && !(*status)[key].is_string()) return false;
        LanMonitorSnapshot next;
        next.job_id = status->value("job_id", std::string());
        next.job_name = status->value("subtask_name", std::string());
        next.print_state = status->value("gcode_state", std::string());
        if (auto value = status->find("progress"); value != status->end()) next.progress = LanMonitorSnapshot::integer(*value, 100);
        if (auto value = status->find("remaining_seconds"); value != status->end())
            next.remaining_seconds = LanMonitorSnapshot::integer(*value, std::numeric_limits<int>::max());
        job = std::move(next);
        received = true;
        last_report = now - std::chrono::seconds(*seconds);
        return true;
    }
};

} // namespace Slic3r
