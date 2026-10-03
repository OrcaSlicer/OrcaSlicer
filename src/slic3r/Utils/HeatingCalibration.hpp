#pragma once

#include "libslic3r/GCode/HeatingTime.hpp"
#include "libslic3r/Point.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Uses Moonraker to record one full-power heating pass, time Klipper's M109/M190 waits on it, and fit a
// heater model from the recording.
class HeatingCalibration
{
public:
    struct Params
    {
        std::string host;
        bool        use_https{false};
        int         port{7125};
        std::string api_key;
        double      nozzle_min{30.};
        double      nozzle_max{300.};
        double      bed_min{30.};
        double      bed_max{100.};
        std::vector<Vec2d> bed_area;
    };

    struct Status
    {
        std::string message;
        int         step{0}; // 1-based; 0 during setup
        int         steps{0};
        double      nozzle_temperature{0.};
        double      nozzle_target{0.};
        double      bed_temperature{0.};
        double      bed_target{0.};
    };

    struct Result
    {
        HeaterCurvePoints nozzle;
        HeaterCurvePoints bed;
    };

    using StatusFn = std::function<void(const Status &)>;

    HeatingCalibration(Params params, StatusFn on_status);

    // Blocking; errors and cancellation throw. Always turns off heaters and restores the idle timeout.
    Result run();
    // Can be called from another thread; cancellation is checked during requests and polling.
    void cancel() { m_cancelled = true; }
    bool waiting_for_firmware() const { return m_waiting_for_firmware; }

private:
    struct HeaterInfo
    {
        HeaterControl control;
        double        min_temp{0.};
        double        max_temp{0.};
    };
    struct Reading
    {
        double eventtime{0.};
        double nozzle{0.};
        double nozzle_power{0.};
        double bed{0.};
        double bed_power{0.};
    };

    std::string base_url() const;
    std::string get(const std::string &path) const;
    void        run_gcode(const std::string &script, long timeout = 900, bool cancellable = true) const;
    Reading     read_heaters() const;
    void        check_cancelled() const;
    void        sleep(double seconds) const;
    void        report(const std::string &message, double nozzle_target, double bed_target, const Reading &reading);
    std::string move_to(const Vec2d &point) const;

    void cool_down(const Reading &start);
    // Streams readings until `stop`: over a websocket on http, else by polling.
    void sample(const std::atomic<bool> &stop, const std::function<void(const Reading &)> &on_reading) const;

    Params            m_params;
    StatusFn          m_on_status;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_waiting_for_firmware{false};
    HeaterInfo        m_nozzle;
    HeaterInfo        m_bed;
    int               m_step{0};
    int               m_steps{0};
    Vec2d             m_park{Vec2d::Zero()};
    Vec3d             m_gcode_offset{Vec3d::Zero()};
};

} // namespace Slic3r
