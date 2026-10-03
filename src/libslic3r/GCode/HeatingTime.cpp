#include "HeatingTime.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Slic3r {

namespace {

constexpr double EPSILON_TEMPERATURE = 0.01;

// Extrapolate the end segments when x lies outside the measured points.
double interpolate(const std::vector<Vec2d> &points, double x, int x_axis)
{
    const int y_axis = 1 - x_axis;
    auto      it     = std::lower_bound(points.begin(), points.end(), x,
                                        [x_axis](const Vec2d &p, double value) { return p[x_axis] < value; });
    if (it == points.begin())
        ++it;
    else if (it == points.end())
        --it;
    const Vec2d &a = *(it - 1);
    const Vec2d &b = *it;
    return a[y_axis] + (x - a[x_axis]) * (b[y_axis] - a[y_axis]) / (b[x_axis] - a[x_axis]);
}

struct HeaterSim
{
    const HeaterCurve *curve{nullptr};
    double             temperature{0.}; // at `since` while heating, current otherwise
    double             target{0.};
    bool               heating{false};
    double             since{0.};
    double             done_at{0.};
    double             heating_time{0.};

    void finish_if_done(double clock)
    {
        if (heating && clock >= done_at) {
            heating_time += done_at - since;
            temperature = target;
            heating     = false;
        }
    }

    void set_target(double new_target, double clock)
    {
        finish_if_done(clock);
        if (std::abs(new_target - target) < EPSILON_TEMPERATURE)
            return;
        if (heating) {
            const double reached = curve->ramp_temperature(curve->ramp_time(temperature) + clock - since);
            heating_time += clock - since;
            temperature = std::min(reached, target);
            heating     = false;
        }
        target = new_target;
        if (new_target > temperature + EPSILON_TEMPERATURE) {
            heating = true;
            since   = clock;
            done_at = clock + curve->heat_time(temperature, new_target);
        }
    }

    double release_at(double wait_for, bool settle, double clock)
    {
        finish_if_done(clock);
        if (!heating || wait_for > target + EPSILON_TEMPERATURE)
            return clock;
        if (settle && wait_for >= target - EPSILON_TEMPERATURE)
            return done_at;
        return std::max(clock, since + curve->heat_time(temperature, wait_for, false));
    }

    double total_heating_time(double cutoff) const { return heating_time + (heating ? std::max(0., std::min(done_at, cutoff) - since) : 0.); }
};

} // namespace

HeaterCurve::HeaterCurve(std::vector<Vec2d> ramp, std::vector<Vec2d> settle)
{
    auto by_temperature = [](const Vec2d &a, const Vec2d &b) { return a.x() < b.x(); };
    std::sort(ramp.begin(), ramp.end(), by_temperature);
    for (const Vec2d &p : ramp)
        if (std::isfinite(p.x()) && std::isfinite(p.y()) &&
            (m_ramp.empty() || (p.x() > m_ramp.back().x() && p.y() > m_ramp.back().y())))
            m_ramp.push_back(p);

    std::sort(settle.begin(), settle.end(), by_temperature);
    std::vector<Vec2d> cold;
    for (const Vec2d &p : settle)
        if (p.x() < 0.) {
            if (std::isfinite(p.x()) && std::isfinite(p.y()))
                cold.emplace_back(-p.x(), std::max(0., p.y()));
        } else if (std::isfinite(p.x()) && std::isfinite(p.y()) && (m_settle.empty() || p.x() > m_settle.back().x()))
            m_settle.emplace_back(p.x(), std::max(0., p.y()));
    std::sort(cold.begin(), cold.end(), by_temperature);
    for (const Vec2d &p : cold)
        if (m_cold_settle.empty() || p.x() > m_cold_settle.back().x())
            m_cold_settle.push_back(p);
}

double HeaterCurve::ramp_time(double temperature) const { return interpolate(m_ramp, temperature, 0); }

double HeaterCurve::ramp_temperature(double seconds) const { return interpolate(m_ramp, seconds, 1); }

double HeaterCurve::settle_time(double target, bool cold) const
{
    const std::vector<Vec2d> &points = cold && !m_cold_settle.empty() ? m_cold_settle : m_settle;
    if (points.empty())
        return 0.;
    if (points.size() == 1 || target <= points.front().x())
        return points.front().y();
    if (target >= points.back().x())
        return points.back().y();
    return interpolate(points, target, 0);
}

double HeaterCurve::heat_time(double from, double to, bool settle) const
{
    if (!valid() || to <= from)
        return 0.;
    const bool cold = from <= m_ramp.front().x() + 10.;
    return ramp_time(to) - ramp_time(from) + (settle ? settle_time(to, cold) : 0.);
}

HeatingTimes estimate_heating_times(const std::vector<HeatingEvent> &events,
                                    const HeaterCurve               &nozzle,
                                    const HeaterCurve               &bed,
                                    double                           ambient,
                                    double                           first_extrusion_time)
{
    std::vector<HeaterSim> nozzles;
    HeaterSim              bed_sim{&bed, ambient};
    double                 blocked = 0.;

    for (const HeatingEvent &event : events) {
        const bool is_bed = event.heater == HeatingEvent::Heater::Bed;
        if (!(is_bed ? bed : nozzle).valid())
            continue;
        if (!is_bed && event.index >= nozzles.size())
            nozzles.resize(event.index + 1, HeaterSim{&nozzle, ambient});
        HeaterSim &sim = is_bed ? bed_sim : nozzles[event.index];

        const double clock = event.machine_time + blocked;
        if (event.target)
            sim.set_target(*event.target, clock);
        if (event.wait_for)
            blocked += sim.release_at(*event.wait_for, event.settle, clock) - clock;
    }

    HeatingTimes times;
    const double cutoff = first_extrusion_time + blocked;
    for (const HeaterSim &sim : nozzles)
        times.nozzle += sim.total_heating_time(cutoff);
    times.bed  = bed_sim.total_heating_time(cutoff);
    times.wait = blocked;
    return times;
}

namespace {

constexpr double sim_step   = 0.05; // s
constexpr double sim_report = 0.3;  // s, Klipper's ADC report interval
constexpr double wait_poll  = 1.;   // s, Klipper checks a M109/M190 wait once a second
constexpr double wait_limit = 900.; // s

class HeaterPlant
{
public:
    HeaterPlant(const HeaterModel &model, double start)
        : m_model(model), m_block(start), m_mass(start), m_sensor(start), m_delay(size_t(std::lround(model.delay / sim_step)), 0.)
    {}

    void step(double power)
    {
        if (!m_delay.empty()) {
            std::swap(power, m_delay[m_next]);
            m_next = (m_next + 1) % m_delay.size();
        }
        const double x       = m_block - m_model.ambient;
        const double to_mass = m_model.to_mass * (m_block - m_mass);
        const double d_block = m_model.gain * power - m_model.loss * x - m_model.loss2 * x * std::abs(x) - to_mass;
        const double d_mass  = m_model.from_mass * (m_block - m_mass) - m_model.mass_loss * (m_mass - m_model.ambient);
        m_block += d_block * sim_step;
        m_mass += d_mass * sim_step;
        m_sensor += (m_block - m_sensor) * (m_model.sensor_lag > sim_step ? sim_step / m_model.sensor_lag : 1.);
    }

    double sensor() const { return m_sensor; }

private:
    const HeaterModel  &m_model;
    double              m_block, m_mass, m_sensor;
    std::vector<double> m_delay;
    size_t              m_next{0};
};

// Klipper's heater control loop and the temperature its waits look at (klippy/extras/heaters.py).
class KlipperHeater
{
public:
    KlipperHeater(const HeaterModel &model, const HeaterControl &control, double start)
        : m_plant(model, start), m_control(control), m_prev_temp(start), m_smoothed(start),
          m_integ_max(control.ki > 0. ? control.max_power / (control.ki / 255.) : 0.)
    {}

    void set_target(double target) { m_target = target; }

    void run(double until)
    {
        while (m_time < until - 1e-9) {
            m_plant.step(m_power);
            m_time += sim_step;
            if (m_time >= m_next_report - 1e-9) {
                report();
                m_next_report += sim_report;
            }
        }
    }

    bool busy() const
    {
        if (m_control.pid)
            return std::abs(m_target - m_smoothed) > 1. || std::abs(m_deriv) > 0.1;
        return m_smoothed < m_target - m_control.max_delta;
    }

private:
    void report()
    {
        const double raw  = m_plant.sensor();
        const double dt   = m_time - m_prev_time;
        const double diff = raw - m_prev_temp;
        double       out  = 0.;
        if (m_control.pid) {
            m_deriv = dt >= m_control.smooth_time ? diff / dt : (m_deriv * (m_control.smooth_time - dt) + diff) / m_control.smooth_time;
            const double error = m_target - raw;
            const double integ = std::clamp(m_integ + error * dt, 0., m_integ_max);
            const double co    = m_control.kp / 255. * error + m_control.ki / 255. * integ - m_control.kd / 255. * m_deriv;
            out                = std::clamp(co, 0., m_control.max_power);
            if (co == out)
                m_integ = integ;
        } else {
            if (m_heating && raw >= m_target + m_control.max_delta)
                m_heating = false;
            else if (!m_heating && raw <= m_target - m_control.max_delta)
                m_heating = true;
            out = m_heating ? m_control.max_power : 0.;
        }
        m_power = m_target > 0. ? out : 0.;
        m_smoothed += (raw - m_smoothed) * std::min((m_time - m_last_report) / m_control.smooth_time, 1.);
        m_prev_temp   = raw;
        m_prev_time   = m_time;
        m_last_report = m_time;
    }

    HeaterPlant         m_plant;
    HeaterControl       m_control;
    double              m_target{0.}, m_power{0.}, m_time{0.}, m_next_report{sim_report}, m_last_report{0.};
    double              m_prev_temp, m_prev_time{0.}, m_deriv{0.}, m_integ{0.}, m_smoothed;
    double              m_integ_max;
    bool                m_heating{false};
};

std::vector<double> nelder_mead(const std::function<double(const std::vector<double> &)> &cost, std::vector<double> x0,
                                int iterations)
{
    const size_t                     n = x0.size();
    std::vector<std::vector<double>> pts{x0};
    for (size_t i = 0; i < n; ++i) {
        std::vector<double> p = x0;
        p[i] += 0.3 * std::abs(p[i]) + 1e-5;
        pts.push_back(p);
    }
    std::vector<double> vals;
    for (const auto &p : pts)
        vals.push_back(cost(p));

    for (int it = 0; it < iterations; ++it) {
        std::vector<size_t> order(pts.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return vals[a] < vals[b]; });
        std::vector<std::vector<double>> sorted_pts;
        std::vector<double>              sorted_vals;
        for (size_t i : order) {
            sorted_pts.push_back(pts[i]);
            sorted_vals.push_back(vals[i]);
        }
        pts = std::move(sorted_pts);
        vals = std::move(sorted_vals);
        if (std::abs(vals.back() - vals.front()) < 1e-7)
            break;

        std::vector<double> centroid(n, 0.);
        for (size_t i = 0; i < n; ++i)
            for (size_t k = 0; k < n; ++k)
                centroid[k] += pts[i][k] / double(n);
        auto toward = [&](double factor) {
            std::vector<double> p(n);
            for (size_t k = 0; k < n; ++k)
                p[k] = centroid[k] + factor * (centroid[k] - pts.back()[k]);
            return p;
        };
        const std::vector<double> reflected = toward(1.);
        const double              fr        = cost(reflected);
        if (fr < vals.front()) {
            const std::vector<double> expanded = toward(2.);
            const double              fe       = cost(expanded);
            pts.back()  = fe < fr ? expanded : reflected;
            vals.back() = std::min(fe, fr);
        } else if (fr < vals[vals.size() - 2]) {
            pts.back()  = reflected;
            vals.back() = fr;
        } else {
            const std::vector<double> contracted = toward(-0.5);
            const double              fc         = cost(contracted);
            if (fc < vals.back()) {
                pts.back()  = contracted;
                vals.back() = fc;
            } else {
                for (size_t i = 1; i < pts.size(); ++i) {
                    for (size_t k = 0; k < n; ++k)
                        pts[i][k] = pts[0][k] + 0.5 * (pts[i][k] - pts[0][k]);
                    vals[i] = cost(pts[i]);
                }
            }
        }
    }
    size_t best = 0;
    for (size_t i = 1; i < pts.size(); ++i)
        if (vals[i] < vals[best])
            best = i;
    return pts[best];
}

HeaterModel unpack_model(const std::vector<double> &p, bool two_masses, double ambient)
{
    HeaterModel model;
    model.gain       = p[0];
    model.loss       = p[1];
    model.loss2      = p[2];
    model.sensor_lag = p[3];
    model.delay      = p[4];
    if (two_masses) {
        model.to_mass   = p[5];
        model.from_mass = p[6];
        model.mass_loss = p[7];
    }
    model.ambient = ambient;
    return model;
}

} // namespace

HeaterModel fit_heater_model(const std::vector<HeatingSample> &samples, double ambient, bool two_masses, double smooth_time)
{
    if (samples.size() < 20 || samples.back().time - samples.front().time < 30.)
        return {};

    const double origin = samples.front().time;
    const double start  = samples.front().temperature;
    const size_t n      = size_t((samples.back().time - origin) / sim_step) + 1;
    std::vector<double> power(n), measured(n);
    for (size_t i = 0, j = 0; i < n; ++i) {
        const double t = origin + double(i) * sim_step;
        while (j + 1 < samples.size() && samples[j + 1].time <= t)
            ++j;
        const HeatingSample &a = samples[j];
        const HeatingSample &b = samples[std::min(j + 1, samples.size() - 1)];
        const double         f = b.time > a.time ? std::clamp((t - a.time) / (b.time - a.time), 0., 1.) : 0.;
        power[i]    = a.power + f * (b.power - a.power);
        measured[i] = a.temperature + f * (b.temperature - a.temperature);
    }

    double max_power = 0.;
    for (const HeatingSample &s : samples)
        max_power = std::max(max_power, s.power);
    if (max_power <= 0.)
        return {};

    // Starting guesses from the pass: the steepest full-power climb, and the power that holds the last temperature.
    double max_slope = 0.;
    for (size_t i = 0, j = 0; i < samples.size(); ++i) {
        while (j < samples.size() && samples[j].time < samples[i].time + 2.)
            ++j;
        if (j >= samples.size())
            break;
        bool full = true;
        for (size_t k = i; k <= j; ++k)
            full = full && samples[k].power >= 0.9 * max_power;
        if (full)
            max_slope = std::max(max_slope, (samples[j].temperature - samples[i].temperature) / (samples[j].time - samples[i].time));
    }
    const double gain0 = std::max(1.1 * max_slope / max_power, 0.05);
    double       end_power = 0., end_temp = 0.;
    size_t       end_count = 0;
    for (size_t i = samples.size(); i-- > 0 && samples.back().time - samples[i].time <= 15.;) {
        end_power += samples[i].power;
        end_temp += samples[i].temperature;
        ++end_count;
    }
    end_power /= double(end_count);
    end_temp /= double(end_count);
    const double rise = end_temp - ambient;
    const double budget = end_power * gain0;
    const double loss0  = rise > 5. && budget > 0. ? 0.5 * budget / rise : 0.002;
    const double loss20 = rise > 5. && budget > 0. ? 0.5 * budget / (rise * rise) : 0.;

    const double smoothing = std::min(sim_report / smooth_time, 1.);
    auto cost = [&](const std::vector<double> &p) {
        for (double v : p)
            if (!(v >= 0.))
                return 1e9;
        if (p[0] > 30. || p[3] > 20. || p[4] > 8.)
            return 1e9;
        const HeaterModel model = unpack_model(p, two_masses, ambient);
        HeaterPlant       plant(model, start);
        const size_t      lag = size_t(std::lround(sim_report / sim_step));
        double            smoothed = start, sum = 0.;
        for (size_t i = 0; i < n; ++i) {
            plant.step(power[i]);
            if (i > 0 && i % lag == 0)
                smoothed += (plant.sensor() - smoothed) * smoothing;
            const double e = smoothed - measured[i];
            sum += e * e;
        }
        const double rms = std::sqrt(sum / double(n));
        return std::isfinite(rms) ? rms : 1e9;
    };

    const std::vector<std::vector<double>> starts = {
        {gain0, loss0, loss20, 2., 1., 0.03, 0.05, 0.001},
        {gain0, loss0, loss20, 5., 0.5, 0.03, 0.05, 0.001},
        {gain0, 1.5 * loss0, 0.5 * loss20, 3., 1.5, 0.02, 0.03, 0.002},
    };
    HeaterModel best;
    best.rms = 1e9;
    for (std::vector<double> start_p : starts) {
        start_p.resize(two_masses ? 8 : 5);
        const std::vector<double> p   = nelder_mead(cost, start_p, 1500);
        const double              rms = cost(p);
        if (rms < best.rms) {
            best     = unpack_model(p, two_masses, ambient);
            best.rms = rms;
        }
    }
    return best.rms < 1e9 ? best : HeaterModel();
}

std::optional<double> simulate_wait(const HeaterModel &model, const HeaterControl &control, const WaitScenario &scenario)
{
    KlipperHeater heater(model, control, scenario.start_temperature);
    heater.set_target(scenario.hold_target);
    heater.run(scenario.command_time);
    heater.set_target(scenario.target);
    for (double t = scenario.command_time; t - scenario.command_time < wait_limit; t += wait_poll) {
        heater.run(t);
        if (!heater.busy())
            return t - scenario.command_time;
    }
    return std::nullopt;
}

HeaterCurvePoints heater_curve(const HeaterModel &model, const HeaterControl &control, double start_temperature,
                               double max_target, const std::vector<WaitAnchor> &anchors)
{
    constexpr double band = 5.;
    HeaterCurvePoints points;

    HeaterPlant plant(model, start_temperature);
    double      smoothed = start_temperature, next_band = std::ceil((start_temperature + 0.5) / band) * band;
    const double smoothing = std::min(sim_report / control.smooth_time, 1.);
    const size_t lag       = size_t(std::lround(sim_report / sim_step));
    for (size_t i = 1; double(i) * sim_step < wait_limit && next_band <= max_target; ++i) {
        plant.step(control.max_power);
        if (i % lag == 0)
            smoothed += (plant.sensor() - smoothed) * smoothing;
        for (; next_band <= smoothed && next_band <= max_target; next_band += band)
            points.ramp.emplace_back(next_band, double(i) * sim_step);
    }
    if (points.ramp.size() < 2)
        return {};
    const double origin = points.ramp.front().y();
    for (Vec2d &p : points.ramp)
        p.y() -= origin;
    const HeaterCurve ramp(points.ramp, {});

    // A wait timed from a warm nozzle gives the settle time directly. The rest only shift the simulated waits.
    std::map<double, std::pair<double, int>> measured;
    double bias = 0.;
    size_t counted = 0;
    for (const WaitAnchor &anchor : anchors) {
        const WaitScenario &scenario = anchor.scenario;
        if (scenario.hold_target > 0. && scenario.hold_target < scenario.target) {
            std::pair<double, int> &sum = measured[scenario.target];
            sum.first += std::max(0., anchor.seconds - ramp.heat_time(scenario.hold_target, scenario.target, false));
            ++sum.second;
        } else if (const std::optional<double> simulated = simulate_wait(model, control, scenario)) {
            bias += anchor.seconds - *simulated;
            ++counted;
        }
    }
    bias = counted > 0 ? std::clamp(bias / double(counted), -15., 15.) : 0.;
    const double first = std::ceil((start_temperature + 0.3 * (max_target - start_temperature)) / 10.) * 10.;
    std::vector<double> targets;
    for (double target = first; target < max_target; target += 10.)
        targets.push_back(target);
    targets.push_back(max_target);
    // Simulated from the cold start, shifted by how far the model missed the cold waits.
    std::vector<Vec2d> cold;
    for (double target : targets) {
        WaitScenario scenario;
        scenario.start_temperature = start_temperature;
        scenario.target            = target;
        if (const std::optional<double> wait = simulate_wait(model, control, scenario))
            cold.emplace_back(target, std::max(0., *wait - ramp.heat_time(start_temperature, target, false) + bias));
    }
    if (measured.empty()) {
        points.settle = std::move(cold);
        return points;
    }
    for (const auto &[target, sum] : measured)
        points.settle.emplace_back(target, sum.first / sum.second);
    for (const Vec2d &p : cold)
        points.settle.emplace_back(-p.x(), p.y());
    return points;
}

} // namespace Slic3r
