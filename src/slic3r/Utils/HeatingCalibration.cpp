#include "HeatingCalibration.hpp"

#include "Http.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/log/trivial.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>

using json = nlohmann::json;

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

namespace Slic3r {
namespace {
constexpr double hold_time = 30.; // s the nozzle idles between steps, so the next step starts from a settled heater
constexpr int    staircase_steps = 4;
constexpr double overshoot_margin = 10.; // a heater overshoots its target, and Klipper shuts down above max_temp

// Park 3 mm above the bed so the part fan blows on it and on the bed sensor.
constexpr double park_z = 3.;
constexpr double park_margin = 10.; // mm inside the reachable area
constexpr int    travel_feedrate = 6000;
constexpr long   heating_timeout = 3600;
constexpr long   cleanup_timeout = 30;
constexpr double cooling_stall_time = 180.;
constexpr double cooling_stall_drop = 0.5;
constexpr int    calibration_idle_timeout = 24 * 3600;

std::string fmt_temp(double temperature) { return float_to_string_decimal_point(double(std::lround(temperature)), 0); }

// json::value() throws on an explicit null; Klipper reports null for unset settings and unavailable sensors.
double number_or(const json &object, const char *key, double fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : fallback;
}

std::string string_or(const json &object, const char *key, const std::string &fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}

} // namespace

HeatingCalibration::HeatingCalibration(Params params, StatusFn on_status)
    : m_params(std::move(params)), m_on_status(std::move(on_status))
{}

std::string HeatingCalibration::base_url() const { return (m_params.use_https ? "https://" : "http://") + m_params.host + ":" + std::to_string(m_params.port); }

std::string HeatingCalibration::get(const std::string &path) const
{
    std::string body, error;
    auto        http = Http::get(base_url() + path);
    if (m_params.use_https)
        http.tls_verify(true);
    if (!m_params.api_key.empty())
        http.header("X-Api-Key", m_params.api_key);
    http.on_progress([this](Http::Progress, bool &cancel) { cancel = m_cancelled; });
    http.timeout_connect(5)
        .timeout_max(30)
        .on_complete([&](std::string response, unsigned) { body = std::move(response); })
        .on_error([&](std::string response, std::string message, unsigned status) {
            error = message.empty() ? "HTTP " + std::to_string(status) + " " + response : message;
        })
        .perform_sync();
    check_cancelled();
    if (!error.empty())
        throw RuntimeError(format(_u8L("Moonraker request %1% failed: %2%"), path, error));
    if (body.empty())
        throw RuntimeError(_u8L("Moonraker returned an empty response."));
    return body;
}

void HeatingCalibration::run_gcode(const std::string &script, long timeout, bool cancellable) const
{
    std::string error;
    bool completed = false;
    auto        http = Http::post(base_url() + "/printer/gcode/script");
    if (m_params.use_https)
        http.tls_verify(true);
    http.header("Content-Type", "application/json").set_post_body(json{{"script", script}}.dump());
    if (!m_params.api_key.empty())
        http.header("X-Api-Key", m_params.api_key);
    // Klipper finishes a submitted script even if its HTTP request is cancelled.
    if (cancellable)
        http.on_progress([this](Http::Progress, bool &cancel) { cancel = m_cancelled; });
    http.timeout_connect(5)
        .timeout_max(timeout)
        .on_complete([&](std::string, unsigned) { completed = true; })
        .on_error([&](std::string response, std::string message, unsigned status) {
            error = message;
            if (auto parsed = json::parse(response, nullptr, false); parsed.is_object() && parsed.contains("error"))
                error = parsed["error"].value("message", response);
            else if (error.empty())
                error = "HTTP " + std::to_string(status);
        })
        .perform_sync();
    if (!completed && error.empty())
        error = "Moonraker did not complete the request.";
    if (!error.empty())
        throw RuntimeError(format(_u8L("Printer rejected \"%1%\": %2%"), script, error));
    if (cancellable)
        check_cancelled();
}

HeatingCalibration::Reading HeatingCalibration::read_heaters() const
{
    const json  result = json::parse(get("/printer/objects/query?extruder=temperature,power&heater_bed=temperature,power"))["result"];
    const json &status = result["status"];
    Reading     reading;
    reading.eventtime    = number_or(result, "eventtime", 0.);
    reading.nozzle       = number_or(status["extruder"], "temperature", 0.);
    reading.nozzle_power = number_or(status["extruder"], "power", 0.);
    reading.bed          = number_or(status["heater_bed"], "temperature", 0.);
    reading.bed_power    = number_or(status["heater_bed"], "power", 0.);
    return reading;
}

void HeatingCalibration::check_cancelled() const
{
    if (m_cancelled)
        throw RuntimeError(_u8L("Calibration cancelled."));
}

void HeatingCalibration::sleep(double seconds) const
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        check_cancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check_cancelled();
}

void HeatingCalibration::report(const std::string &message, double nozzle_target, double bed_target, const Reading &reading)
{
    Status status;
    status.message            = message;
    status.step               = m_step;
    status.steps              = m_steps;
    status.nozzle_temperature = reading.nozzle;
    status.nozzle_target      = nozzle_target;
    status.bed_temperature    = reading.bed;
    status.bed_target         = bed_target;
    m_on_status(status);
}

std::string HeatingCalibration::move_to(const Vec2d &point) const
{
    std::string command = "G1 X";
    command.reserve(64);
    command += float_to_string_decimal_point(point.x() - m_gcode_offset.x(), 2);
    command += " Y";
    command += float_to_string_decimal_point(point.y() - m_gcode_offset.y(), 2);
    command += " F";
    command += float_to_string_decimal_point(travel_feedrate, 0);
    return command;
}

void HeatingCalibration::cool_down(const Reading &start)
{
    run_gcode("M104 T0 S0\nM140 S0\nM106 S255");
    // In a warm room, stop cooling when the temperature stalls above the minimum.
    double nozzle_lowest = start.nozzle, nozzle_lowest_at = start.eventtime;
    double bed_lowest = start.bed, bed_lowest_at = start.eventtime;
    for (;;) {
        const Reading reading = read_heaters();
        report(_u8L("Cooling down"), m_params.nozzle_min, m_params.bed_min, reading);
        if (reading.nozzle < nozzle_lowest - cooling_stall_drop) {
            nozzle_lowest    = reading.nozzle;
            nozzle_lowest_at = reading.eventtime;
        }
        if (reading.bed < bed_lowest - cooling_stall_drop) {
            bed_lowest    = reading.bed;
            bed_lowest_at = reading.eventtime;
        }
        const bool nozzle_cool = reading.nozzle <= m_params.nozzle_min || reading.eventtime - nozzle_lowest_at > cooling_stall_time;
        const bool bed_cool    = reading.bed <= m_params.bed_min || reading.eventtime - bed_lowest_at > cooling_stall_time;
        if (nozzle_cool && bed_cool)
            return;
        sleep(1.);
    }
}

void HeatingCalibration::sample(const std::atomic<bool> &stop, const std::function<void(const Reading &)> &on_reading) const
{
    bool subscribed = false;
    if (!m_params.use_https) {
        try {
            net::io_context   ioc;
            tcp::resolver     resolver{ioc};
            beast::tcp_stream tcp_stream{ioc};
            tcp_stream.expires_after(std::chrono::seconds(5));
            tcp_stream.connect(resolver.resolve(m_params.host, std::to_string(m_params.port)));

            websocket::stream<beast::tcp_stream> ws{std::move(tcp_stream)};
            ws.set_option(websocket::stream_base::decorator([&](websocket::request_type &request) {
                if (!m_params.api_key.empty())
                    request.set("X-Api-Key", m_params.api_key);
            }));
            ws.handshake(m_params.host + ":" + std::to_string(m_params.port), "/websocket");
            ws.text(true);
            ws.write(net::buffer(json{{"jsonrpc", "2.0"},
                                      {"method", "printer.objects.subscribe"},
                                      {"params", {{"objects", {{"extruder", {"temperature", "power"}}, {"heater_bed", {"temperature", "power"}}}}}},
                                      {"id", 1}}
                                         .dump()));

            Reading reading;
            auto merge = [&](const json &status, const json &eventtime) {
                if (!status.is_object())
                    return;
                auto read = [&](const char *heater, const char *key, double &value) {
                    if (const auto object = status.find(heater); object != status.end() && object->is_object())
                        if (const auto field = object->find(key); field != object->end() && field->is_number())
                            value = field->get<double>();
                };
                read("extruder", "temperature", reading.nozzle);
                read("extruder", "power", reading.nozzle_power);
                read("heater_bed", "temperature", reading.bed);
                read("heater_bed", "power", reading.bed_power);
                if (eventtime.is_number())
                    reading.eventtime = eventtime.get<double>();
                if (subscribed)
                    on_reading(reading);
            };
            while (!stop) {
                // Klipper reports at least twice a second while heaters are on.
                ws.next_layer().expires_after(std::chrono::seconds(5));
                beast::flat_buffer buffer;
                ws.read(buffer);
                const json message = json::parse(beast::buffers_to_string(buffer.data()), nullptr, false);
                if (!message.is_object())
                    continue;
                if (const auto result = message.find("result"); result != message.end() && message.value("id", 0) == 1 && result->is_object()) {
                    const auto status = result->find("status");
                    if (status != result->end()) {
                        subscribed = false;
                        merge(*status, result->value("eventtime", json()));
                        subscribed = true;
                        on_reading(reading);
                    }
                } else if (message.value("method", "") == "notify_status_update" && message.contains("params") && message["params"].size() >= 2)
                    merge(message["params"][0], message["params"][1]);
            }
            ws.next_layer().expires_after(std::chrono::seconds(1));
            beast::error_code ec;
            ws.close(websocket::close_code::normal, ec);
            return;
        } catch (const std::exception &e) {
            if (subscribed)
                throw;
            BOOST_LOG_TRIVIAL(warning) << "Heating calibration: websocket unavailable, polling instead: " << e.what();
        }
    }
    while (!stop) {
        on_reading(read_heaters());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

HeatingCalibration::Result HeatingCalibration::run()
{
    if (m_params.nozzle_max - m_params.nozzle_min < 10. || m_params.bed_max - m_params.bed_min < 10.)
        throw RuntimeError(_u8L(u8"Nozzle and bed ranges must each span at least 10 \u2103."));
    m_step  = 0;
    m_steps = 0;

    const json info = json::parse(get("/printer/info"))["result"];
    if (info.value("state", "") != "ready")
        throw RuntimeError(format(_u8L("Printer not ready: %1%"), info.value("state_message", info.value("state", ""))));

    const json status = json::parse(get("/printer/objects/query?configfile=settings&print_stats=state&toolhead=homed_axes,axis_minimum,axis_maximum&idle_timeout=idle_timeout"))
                            ["result"]["status"];
    const std::string print_state = status["print_stats"].value("state", "");
    if (print_state == "printing" || print_state == "paused")
        throw RuntimeError(_u8L("Printer is printing or paused."));

    const json &settings = status["configfile"]["settings"];
    if (!settings.contains("extruder") || !settings.contains("heater_bed"))
        throw RuntimeError(_u8L("Printer needs both [extruder] and [heater_bed] sections."));
    auto heater_info = [](const json &section) {
        HeaterInfo heater;
        heater.control.pid         = string_or(section, "control", "") == "pid";
        heater.control.kp          = number_or(section, "pid_kp", 0.);
        heater.control.ki          = number_or(section, "pid_ki", 0.);
        heater.control.kd          = number_or(section, "pid_kd", 0.);
        heater.control.max_power   = number_or(section, "max_power", 1.);
        heater.control.smooth_time = number_or(section, "smooth_time", 1.);
        heater.control.max_delta   = number_or(section, "max_delta", 2.);
        heater.min_temp            = number_or(section, "min_temp", 0.);
        heater.max_temp            = number_or(section, "max_temp", 0.);
        return heater;
    };
    m_nozzle = heater_info(settings["extruder"]);
    m_bed    = heater_info(settings["heater_bed"]);
    auto validate_target = [](const char *name, double target, const HeaterInfo &heater) {
        if (target != 0. && target < heater.min_temp)
            throw RuntimeError(format(_u8L(u8"%1% target is below min_temp %2% \u2103."), name, heater.min_temp));
        if (target > heater.max_temp)
            throw RuntimeError(format(_u8L(u8"%1% target exceeds max_temp %2% \u2103."), name, heater.max_temp));
    };
    validate_target("heater_bed", m_params.bed_max, m_bed);
    m_params.nozzle_max = std::min(m_params.nozzle_max, m_nozzle.max_temp - overshoot_margin);
    if (m_params.nozzle_max - m_params.nozzle_min < 10.)
        throw RuntimeError(_u8L(u8"Nozzle and bed ranges must each span at least 10 \u2103."));
    std::vector<double> nozzle_steps;
    for (int k = 1; k <= staircase_steps; ++k) {
        const double target = k == staircase_steps ? m_params.nozzle_max
                                                   : std::round((m_params.nozzle_min + (m_params.nozzle_max - m_params.nozzle_min) * k / staircase_steps) / 10.) * 10.;
        if (target > (nozzle_steps.empty() ? m_params.nozzle_min : nozzle_steps.back()) + 5.) {
            validate_target("extruder", target, m_nozzle);
            nozzle_steps.push_back(target);
        }
    }
    m_steps = 1 + int(nozzle_steps.size());
    const double idle_timeout = number_or(status.value("idle_timeout", json::object()), "idle_timeout",
                                          settings.contains("idle_timeout") ? number_or(settings["idle_timeout"], "timeout", 600.) : 600.);

    const json &toolhead = status["toolhead"];
    const Vec2d axis_min(toolhead["axis_minimum"][0].get<double>(), toolhead["axis_minimum"][1].get<double>());
    const Vec2d axis_max(toolhead["axis_maximum"][0].get<double>(), toolhead["axis_maximum"][1].get<double>());
    const double z_min = toolhead["axis_minimum"][2].get<double>();
    const double z_max = toolhead["axis_maximum"][2].get<double>();
    if (z_max < park_z || z_min > z_max)
        throw RuntimeError(_u8L("Printer Z travel must reach at least 3 mm."));
    const double park_height = std::clamp(park_z, z_min, z_max);
    const double travel_height = std::max(park_height, std::min(10., z_max));

    // Leave extra clearance for G-code coordinates rounded to 0.01 mm.
    const Vec2d safe_axis_min = axis_min + Vec2d::Constant(park_margin + 0.05);
    const Vec2d safe_axis_max = axis_max - Vec2d::Constant(park_margin + 0.05);
    if ((safe_axis_max - safe_axis_min).minCoeff() < 0.)
        throw RuntimeError(_u8L("No parking position inside printer travel limits."));
    m_park = 0.5 * (safe_axis_min + safe_axis_max);
    if (!m_params.bed_area.empty()) {
        if (m_params.bed_area.size() < 3)
            throw RuntimeError(_u8L("Printable area is not a valid polygon."));
        const Polygon area = Polygon::new_scale(m_params.bed_area);
        if (area.area() == 0.)
            throw RuntimeError(_u8L("Printable area is not a valid polygon."));
        const Polygon travel = Polygon::new_scale({safe_axis_min, {safe_axis_max.x(), safe_axis_min.y()},
                                                  safe_axis_max, {safe_axis_min.x(), safe_axis_max.y()}});
        const ExPolygons safe_area = intersection_ex(shrink_ex(Polygons{area}, float(park_margin + 0.05)), Polygons{travel});
        if (safe_area.empty())
            throw RuntimeError(_u8L("No parking position inside the printable area and printer travel limits."));
        // Park at the bed center, where bed sensors usually sit, or at the nearest safe point to it.
        const Vec2d center = BoundingBoxf(m_params.bed_area).center();
        const Point center_scaled = Point::new_scale(center);
        m_park = unscale(safe_area.front().contour.points.front());
        if (std::any_of(safe_area.begin(), safe_area.end(), [&](const ExPolygon &region) { return region.contains(center_scaled); }))
            m_park = center;
        else
            for (const ExPolygon &region : safe_area)
                for (const Point &point : region.contour.points)
                    if ((unscale(point) - center).squaredNorm() < (m_park - center).squaredNorm())
                        m_park = unscale(point);
    }

    const std::string gcode_state = "ORCASLICER_HEATING_CALIBRATION_" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    bool gcode_state_saved = false;
    auto cleanup = [&]() {
        std::exception_ptr restore_error;
        if (gcode_state_saved) {
            try {
                run_gcode("RESTORE_GCODE_STATE NAME=" + gcode_state + " MOVE=0", cleanup_timeout, false);
            } catch (...) {
                restore_error = std::current_exception();
            }
        }
        run_gcode("M104 T0 S0\nM140 S0\nM107\nSET_IDLE_TIMEOUT TIMEOUT=" + json(idle_timeout).dump(), cleanup_timeout, false);
        if (restore_error)
            std::rethrow_exception(restore_error);
    };

    std::vector<HeatingSample> nozzle_samples, bed_samples;
    std::vector<WaitAnchor>    nozzle_anchors, bed_anchors;
    double                     ambient = 0.;
    try {
        // Bed cooling may exceed Klipper's idle timeout, which switches off heaters and motors.
        run_gcode("SET_IDLE_TIMEOUT TIMEOUT=" + float_to_string_decimal_point(calibration_idle_timeout, 0));
        Reading reading = read_heaters();
        const std::string homed = status["toolhead"].value("homed_axes", "");
        if (homed.find('x') == std::string::npos || homed.find('y') == std::string::npos || homed.find('z') == std::string::npos) {
            report(_u8L("Homing"), 0., 0., reading);
            run_gcode("G28");
        }
        check_cancelled();
        report(_u8L("Parking nozzle"), 0., 0., reading);
        // Save after homing so restoring coordinate state does not undo its origin.
        run_gcode("SAVE_GCODE_STATE NAME=" + gcode_state, cleanup_timeout, false);
        gcode_state_saved = true;
        const json move_status = json::parse(get("/printer/objects/query?gcode_move=position,gcode_position"));
        const json &gcode_move = move_status["result"]["status"]["gcode_move"];
        for (int axis = 0; axis < 3; ++axis)
            m_gcode_offset[axis] = gcode_move["position"][axis].get<double>() - gcode_move["gcode_position"][axis].get<double>();
        run_gcode("G90\nG1 Z" + float_to_string_decimal_point(travel_height - m_gcode_offset.z(), 6) + " F600\n" +
                  move_to(m_park) + "\nG1 Z" +
                  float_to_string_decimal_point(park_height - m_gcode_offset.z(), 6) + " F600");

        m_step = 1;
        cool_down(read_heaters());
        run_gcode("M107");

        // The nozzle climbs in steps and idles at each one, so every wait after the first starts from a
        // settled warm nozzle. The bed goes straight to its maximum.
        std::mutex   samples_mutex;
        std::string  phase = _u8L("Heating");
        double       nozzle_target = nozzle_steps.front(), bed_target = m_params.bed_max;
        const Reading first  = read_heaters();
        const double  origin = first.eventtime;
        ambient              = std::min(first.nozzle, first.bed);

        std::atomic<bool>  stop{false};
        std::atomic<bool>  sampler_failed{false};
        std::thread        sampler([&]() {
            try {
                sample(stop, [&](const Reading &reading) {
                    std::string message;
                    double      nozzle_goal, bed_goal;
                    {
                        std::lock_guard<std::mutex> lock(samples_mutex);
                        const double time = reading.eventtime - origin;
                        if (time >= 0. && (nozzle_samples.empty() || time > nozzle_samples.back().time)) {
                            nozzle_samples.push_back({time, reading.nozzle, reading.nozzle_power});
                            bed_samples.push_back({time, reading.bed, reading.bed_power});
                        }
                        message     = phase;
                        nozzle_goal = nozzle_target;
                        bed_goal    = bed_target;
                    }
                    report(message, nozzle_goal, bed_goal, reading);
                });
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Heating calibration: sampling stopped";
                sampler_failed = true;
            }
        });
        auto stop_sampler = [&]() {
            stop = true;
            sampler.join();
        };
        // A wait blocks until Klipper releases it. Cancelling the HTTP request cannot stop a firmware wait.
        auto wait = [&](const std::string &script) {
            m_waiting_for_firmware = true;
            try {
                check_cancelled();
                run_gcode(script, heating_timeout, false);
                m_waiting_for_firmware = false;
            } catch (...) {
                m_waiting_for_firmware = false;
                throw;
            }
            check_cancelled();
            if (sampler_failed)
                throw RuntimeError(_u8L("Connection to printer lost during heating."));
            return read_heaters().eventtime - origin;
        };
        auto set_phase = [&](const std::string &message, double nozzle_goal, double bed_goal) {
            std::lock_guard<std::mutex> lock(samples_mutex);
            phase         = message;
            nozzle_target = nozzle_goal;
            bed_target    = bed_goal;
        };

        try {
            run_gcode("M140 S" + fmt_temp(m_params.bed_max));
            double previous = 0., release = 0.;
            for (size_t i = 0; i < nozzle_steps.size(); ++i) {
                const double target = nozzle_steps[i];
                m_step              = 2 + int(i);
                set_phase(_u8L("Heating"), target, m_params.bed_max);
                run_gcode("M104 T0 S" + fmt_temp(target));
                const double set = read_heaters().eventtime - origin;
                release          = wait("M109 T0 S" + fmt_temp(target));
                // The first step starts cold and Klipper sent M104 about 0.4 s before it began to wait.
                nozzle_anchors.push_back({{first.nozzle, previous, i == 0 ? 0.4 : set, target}, release - set + (i == 0 ? 0.4 : 0.)});
                previous = target;
                BOOST_LOG_TRIVIAL(info) << "Heating calibration: nozzle " << target << " released after " << release - set << " s";
                if (i + 1 < nozzle_steps.size())
                    sleep(hold_time);
            }

            const double bed_release = wait("M190 S" + fmt_temp(m_params.bed_max));
            bed_anchors.push_back({{first.bed, m_params.bed_max, release, m_params.bed_max}, bed_release - release});
            BOOST_LOG_TRIVIAL(info) << "Heating calibration: bed released after " << bed_release - release << " s";
        } catch (...) {
            stop_sampler();
            throw;
        }
        stop_sampler();
    } catch (...) {
        try {
            cleanup();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Heating calibration: cleanup failed: " << e.what();
        }
        throw;
    }
    cleanup();

    report(_u8L("Fitting heater curves"), 0., 0., read_heaters());
    Result result;
    for (const bool nozzle : {true, false}) {
        const HeaterInfo &heater  = nozzle ? m_nozzle : m_bed;
        const std::vector<HeatingSample> &samples = nozzle ? nozzle_samples : bed_samples;
        const HeaterModel model = fit_heater_model(samples, ambient, nozzle, heater.control.smooth_time);
        if (!model.valid())
            throw RuntimeError(_u8L("Could not fit a heater model to the recorded heating pass."));
        (nozzle ? result.nozzle : result.bed) = heater_curve(model, heater.control, samples.front().temperature,
                                                             nozzle ? m_params.nozzle_max : m_params.bed_max,
                                                             nozzle ? nozzle_anchors : bed_anchors);
    }
    if (!HeaterCurve(result.nozzle.ramp, {}).valid() || !HeaterCurve(result.bed.ramp, {}).valid())
        throw RuntimeError(_u8L("Could not fit a heater model to the recorded heating pass."));
    return result;
}

} // namespace Slic3r
