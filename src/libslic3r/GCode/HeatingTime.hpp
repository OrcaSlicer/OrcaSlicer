#pragma once

#include "libslic3r/Point.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace Slic3r {

// Ramp points are temperature x seconds at full power, relative to one origin.
// Settle points add the time Klipper needs after crossing each target before M109/M190 returns. A point with a
// negative temperature is the settle time of that target (negated) when the heater starts cold, near the first
// ramp point, instead of from a settled warm heater.
class HeaterCurve
{
public:
    HeaterCurve() = default;
    HeaterCurve(std::vector<Vec2d> ramp, std::vector<Vec2d> settle);

    bool valid() const { return m_ramp.size() >= 2; }

    // Extrapolates past either end of the ramp.
    double ramp_time(double temperature) const;
    double ramp_temperature(double seconds) const;
    // Holds the nearest measured value outside the settle range.
    double settle_time(double target, bool cold = false) const;
    // Klipper TEMPERATURE_WAIT crosses the target without settling.
    double heat_time(double from, double to, bool settle = true) const;

private:
    std::vector<Vec2d> m_ramp;
    std::vector<Vec2d> m_settle;
    std::vector<Vec2d> m_cold_settle;
};

struct HeatingEvent
{
    enum class Heater : unsigned char { Nozzle, Bed };

    Heater                heater{Heater::Nozzle};
    unsigned int          index{0};      // nozzle extruder index
    std::optional<double> target;
    std::optional<double> wait_for;
    bool                  settle{true};
    size_t                move_id{0};    // moves stored before this command
    double                machine_time{0.}; // motion seconds before this command
};

// Heater times may overlap; wait counts only blocked time.
struct HeatingTimes
{
    double nozzle{0.};
    double bed{0.};
    double wait{0.};
};

// Cooling is not modelled. Heaters without a valid curve are ignored.
HeatingTimes estimate_heating_times(const std::vector<HeatingEvent> &events,
                                    const HeaterCurve               &nozzle,
                                    const HeaterCurve               &bed,
                                    double                           ambient,
                                    double                           first_extrusion_time);

// Sample time is seconds since the start of the calibration pass.
struct HeatingSample
{
    double time{0.};
    double temperature{0.};
    double power{0.}; // PWM, 0 .. max_power
};

struct HeaterCurvePoints
{
    std::vector<Vec2d> ramp;
    std::vector<Vec2d> settle;
};

// Klipper's control settings for one heater ([extruder] or [heater_bed]).
struct HeaterControl
{
    bool   pid{true};
    double kp{0.}, ki{0.}, kd{0.}; // pid_Kp/Ki/Kd as written in the config; Klipper divides them by 255
    double max_power{1.};
    double smooth_time{1.};
    double max_delta{2.}; // watermark hysteresis
};

// A heater with its sensor: the block gets the power, an optional second mass (nozzle, heat break) is
// coupled to it, and the sensor lags the block. Loss to ambient has a linear and a quadratic part.
struct HeaterModel
{
    double gain{0.};       // °C/s at unit power
    double loss{0.};       // 1/s
    double loss2{0.};      // 1/(s·°C)
    double to_mass{0.};    // 1/s, block to second mass
    double from_mass{0.};  // 1/s, second mass to block
    double mass_loss{0.};  // 1/s
    double sensor_lag{0.}; // s
    double delay{0.};      // s, from setting the power to the block feeling it
    double ambient{25.};
    double rms{0.};        // °C, fit error over the recorded pass

    bool valid() const { return gain > 0.; }
};

// Fits the model to a recorded pass, driving it with the recorded heater power. `two_masses` adds the
// second mass, which a nozzle needs and a bed does not. Returns an invalid model when the pass is too short.
HeaterModel fit_heater_model(const std::vector<HeatingSample> &samples, double ambient, bool two_masses, double smooth_time = 1.);

// A wait as the printer runs it: the heater holds `hold_target` from time 0 (0 = off), then the wait
// command starts at `command_time`, sets `target` and blocks until Klipper releases it.
struct WaitScenario
{
    double start_temperature{25.};
    double hold_target{0.};
    double command_time{0.4};
    double target{0.};
};

// Seconds from `command_time` until M109/M190 returns, by running Klipper's PID (or watermark) control,
// its temperature smoothing and its once-a-second check_busy poll on the model. Empty if it never releases.
std::optional<double> simulate_wait(const HeaterModel &model, const HeaterControl &control, const WaitScenario &scenario);

// A wait measured on the printer.
struct WaitAnchor
{
    WaitScenario scenario;
    double       seconds{0.};
};

// Ramp: the model at full power from `start_temperature`, in 5 °C bands. Settle: the simulated wait for
// targets up to `max_target` minus the ramp time, plus the mean miss of the model on the measured `anchors`.
HeaterCurvePoints heater_curve(const HeaterModel &model, const HeaterControl &control, double start_temperature,
                               double max_target, const std::vector<WaitAnchor> &anchors);

} // namespace Slic3r
