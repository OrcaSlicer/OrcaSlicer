#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/HeatingTime.hpp"

#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

HeaterCurve linear_curve(double rate) { return HeaterCurve({{0., 0.}, {300., 300. / rate}}, {}); }

HeatingEvent set_target(HeatingEvent::Heater heater, double target, double machine_time)
{
    HeatingEvent event;
    event.heater       = heater;
    event.target       = target;
    event.machine_time = machine_time;
    return event;
}

HeatingEvent wait_for(HeatingEvent::Heater heater, double target, double machine_time)
{
    HeatingEvent event = set_target(heater, target, machine_time);
    event.wait_for     = target;
    return event;
}

} // namespace

TEST_CASE("Heat time is the ramp difference plus the settle time of the target", "[HeatingTime]")
{
    const HeaterCurve curve({{20., 0.}, {120., 50.}, {220., 150.}}, {{100., 10.}, {200., 30.}});
    CHECK_THAT(curve.heat_time(60., 170.), WithinAbs(80. + 24., 1e-9));
    CHECK_THAT(curve.heat_time(60., 170., false), WithinAbs(80., 1e-9));
    CHECK_THAT(curve.heat_time(20., 250., true) - curve.heat_time(20., 250., false), WithinAbs(30., 1e-9));
    CHECK_THAT(curve.heat_time(10., 20., false), WithinAbs(5., 1e-9));
    CHECK(curve.heat_time(170., 60.) == 0.);
}

TEST_CASE("Heaters heating in parallel block only while the printer waits on them", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {set_target(H::Nozzle, 150., 0.), set_target(H::Bed, 60., 0.),
                                              wait_for(H::Bed, 60., 5.), wait_for(H::Nozzle, 220., 10.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), linear_curve(0.1), 20., 10.);

    CHECK_THAT(times.bed, WithinAbs(400., 1e-9));
    CHECK_THAT(times.nozzle, WithinAbs(65. + 35., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(395. + 35., 1e-9));
}

TEST_CASE("A heater retargeted before reaching its target keeps heating from where it got", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {set_target(H::Nozzle, 150., 0.), wait_for(H::Nozzle, 220., 20.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), linear_curve(0.1), 20., 20.);

    CHECK_THAT(times.nozzle, WithinAbs(100., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(80., 1e-9));
    CHECK(times.bed == 0.);
}

TEST_CASE("Waits on an uncalibrated heater add nothing", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {wait_for(H::Bed, 60., 0.), wait_for(H::Nozzle, 220., 0.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), HeaterCurve(), 20., 0.);

    CHECK(times.bed == 0.);
    CHECK_THAT(times.nozzle, WithinAbs(100., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(100., 1e-9));
}

TEST_CASE("Heating stops accumulating when the first extrusion begins", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {set_target(H::Nozzle, 220., 0.), set_target(H::Bed, 80., 0.),
                                              wait_for(H::Nozzle, 220., 0.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), linear_curve(0.1), 20., 5.);

    CHECK_THAT(times.nozzle, WithinAbs(100., 1e-9));
    CHECK_THAT(times.bed, WithinAbs(105., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(100., 1e-9));
}

namespace {

// One pass recorded on a Klipper printer (Creality Ender-3 V3 KE): both heaters from cold, the nozzle to 150 °C
// held for 120 s and then to 290 °C, the bed to 100 °C. The waits below are what Klipper's M109/M190 took.
struct RecordedPass
{
    std::vector<HeatingSample> nozzle, bed;
};

RecordedPass load_recorded_pass()
{
    RecordedPass pass;
    std::ifstream file(std::string(TEST_DATA_DIR) + "/heating_pass.csv");
    std::string   line;
    std::getline(file, line);
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string        cell;
        double             v[5];
        for (double &value : v) {
            std::getline(in, cell, ',');
            value = std::stod(cell);
        }
        pass.nozzle.push_back({v[0], v[1], v[2]});
        pass.bed.push_back({v[0], v[3], v[4]});
    }
    return pass;
}

HeaterControl nozzle_control()
{
    HeaterControl control;
    control.kp = 23.535;
    control.ki = 2.21;
    control.kd = 62.663;
    return control;
}

HeaterControl bed_control()
{
    HeaterControl control;
    control.kp = 66.917;
    control.ki = 1.121;
    control.kd = 998.735;
    return control;
}

HeaterModel hand_model()
{
    HeaterModel model;
    model.gain       = 3.;
    model.loss       = 0.004;
    model.loss2      = 1e-5;
    model.sensor_lag = 2.;
    model.delay      = 1.;
    model.ambient    = 25.;
    return model;
}

} // namespace

TEST_CASE("A heater that cannot reach its target never releases the wait", "[HeatingTime]")
{
    HeaterModel weak = hand_model();
    weak.gain        = 0.1;
    WaitScenario scenario;
    scenario.start_temperature = 25.;
    scenario.target            = 250.;
    CHECK_FALSE(simulate_wait(weak, nozzle_control(), scenario).has_value());
}

TEST_CASE("A PID wait releases after the full-power ramp reaches the target and a watermark wait before", "[HeatingTime]")
{
    const HeaterModel   model = hand_model();
    WaitScenario        scenario;
    scenario.start_temperature = 25.;
    scenario.target            = 200.;

    const HeaterCurvePoints points = heater_curve(model, nozzle_control(), 25., 250., {});
    const HeaterCurve       ramp(points.ramp, {});
    REQUIRE(ramp.valid());
    const std::optional<double> pid = simulate_wait(model, nozzle_control(), scenario);
    REQUIRE(pid.has_value());
    CHECK(*pid > ramp.heat_time(25., 200., false));

    HeaterControl watermark = nozzle_control();
    watermark.pid           = false;
    const std::optional<double> hysteresis = simulate_wait(model, watermark, scenario);
    REQUIRE(hysteresis.has_value());
    CHECK(*hysteresis < *pid);
}

TEST_CASE("Measured waits shift the settle time by the mean miss of the model", "[HeatingTime]")
{
    const HeaterModel model = hand_model();
    WaitScenario      scenario;
    scenario.start_temperature = 25.;
    scenario.target            = 200.;
    const double simulated     = *simulate_wait(model, nozzle_control(), scenario);

    const HeaterCurvePoints plain   = heater_curve(model, nozzle_control(), 25., 250., {});
    const HeaterCurvePoints shifted = heater_curve(model, nozzle_control(), 25., 250., {{scenario, simulated + 6.}});
    REQUIRE(plain.settle.size() == shifted.settle.size());
    REQUIRE_FALSE(plain.settle.empty());
    for (size_t i = 0; i < plain.settle.size(); ++i)
        CHECK_THAT(shifted.settle[i].y() - plain.settle[i].y(), WithinAbs(6., 1e-9));
}

TEST_CASE("A cold heater start uses the cold settle points and a warm start does not", "[HeatingTime]")
{
    const HeaterCurve curve({{30., 0.}, {300., 270.}}, {{150., 10.}, {300., 10.}, {-150., 25.}, {-300., 25.}});
    CHECK_THAT(curve.heat_time(35., 150.), WithinAbs(115. + 25., 1e-9));
    CHECK_THAT(curve.heat_time(100., 200.), WithinAbs(100. + 10., 1e-9));
    CHECK_THAT(curve.settle_time(200., true), WithinAbs(25., 1e-9));
    CHECK_THAT(curve.settle_time(200.), WithinAbs(10., 1e-9));

    const HeaterCurve without_cold({{30., 0.}, {300., 270.}}, {{150., 10.}, {300., 10.}});
    CHECK_THAT(without_cold.heat_time(35., 150.), WithinAbs(115. + 10., 1e-9));
}

TEST_CASE("Waits timed from a warm heater set the warm settle time exactly", "[HeatingTime]")
{
    const HeaterModel model = hand_model();
    WaitScenario      warm;
    warm.start_temperature = 25.;
    warm.hold_target       = 150.;
    warm.command_time      = 300.;
    warm.target            = 200.;

    const HeaterCurvePoints points = heater_curve(model, nozzle_control(), 25., 250., {{warm, 36.}});
    const HeaterCurve       curve(points.ramp, points.settle);
    REQUIRE(curve.valid());
    CHECK_THAT(curve.heat_time(150., 200.), WithinAbs(36., 1e-9));
    CHECK(curve.settle_time(200., true) != curve.settle_time(200.));
}

TEST_CASE("A model fitted to a recorded pass predicts the waits Klipper took", "[HeatingTime][Recorded]")
{
    const RecordedPass pass = load_recorded_pass();
    REQUIRE(pass.nozzle.size() > 1000);

    const HeaterModel nozzle = fit_heater_model(pass.nozzle, 22.5, true);
    const HeaterModel bed    = fit_heater_model(pass.bed, 22.5, false);
    REQUIRE(nozzle.valid());
    REQUIRE(bed.valid());
    CHECK(nozzle.rms < 4.);
    CHECK(bed.rms < 1.);

    WaitScenario cold;
    cold.start_temperature = pass.nozzle.front().temperature;
    cold.target            = 150.;
    const std::optional<double> to_mid = simulate_wait(nozzle, nozzle_control(), cold);
    REQUIRE(to_mid.has_value());
    CHECK_THAT(*to_mid, WithinAbs(70.8, 10.)); // Klipper: 70.8 s from cold to 150 °C

    WaitScenario held = cold;
    held.hold_target  = 150.;
    held.command_time = 191.4;
    held.target       = 290.;
    const std::optional<double> to_max = simulate_wait(nozzle, nozzle_control(), held);
    REQUIRE(to_max.has_value());
    CHECK_THAT(*to_max, WithinAbs(96.1, 10.)); // Klipper: 96.1 s from 150 °C held to 290 °C

    WaitScenario bed_wait;
    bed_wait.start_temperature = pass.bed.front().temperature;
    bed_wait.hold_target       = 100.;
    bed_wait.command_time      = 287.7;
    bed_wait.target            = 100.;
    const std::optional<double> bed_release = simulate_wait(bed, bed_control(), bed_wait);
    REQUIRE(bed_release.has_value());
    CHECK_THAT(287.7 + *bed_release, WithinAbs(307., 20.)); // Klipper released the bed 307 s after the start

    const HeaterCurvePoints points = heater_curve(bed, bed_control(), bed_wait.start_temperature, 100., {});
    const HeaterCurve       curve(points.ramp, points.settle);
    REQUIRE(curve.valid());
    CHECK_THAT(curve.heat_time(bed_wait.start_temperature, 100.), WithinAbs(307., 30.));
}
