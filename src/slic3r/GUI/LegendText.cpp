#include "LegendText.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/format.hpp"
#include "libvgcode/include/Types.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr double MM_PER_INCH = 25.4;
constexpr double GRAMS_PER_OUNCE = 28.349523125;

std::string fixed(double value, int decimals)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(0, decimals), value);
    return buffer;
}

struct Split
{
    int days{0};
    int hours{0};
    int minutes{0};
    int seconds{0};
};

// Whole seconds, the fraction dropped, rounded into the minutes once there are hours, as the columns show no seconds then.
Split split_time(float time)
{
    long long total = static_cast<long long>(std::max(0.f, time));
    if (total >= 3600)
        total = (total + 30) / 60 * 60;
    Split s;
    s.days    = int(total / 86400);
    s.hours   = int(total % 86400 / 3600);
    s.minutes = int(total % 3600 / 60);
    s.seconds = int(total % 60);
    return s;
}

std::string time_in(float time, const std::string& days_hours, const std::string& hours_minutes, const std::string& minutes_seconds,
                    const std::string& seconds, const std::string& under_second)
{
    if (time > 0.f && time < 1.f)
        return under_second;
    const Split s = split_time(time);
    if (s.days > 0)
        return format(days_hours, s.days, s.hours);
    if (s.hours > 0)
        return format(hours_minutes, s.hours, s.minutes);
    if (s.minutes > 0)
        return format(minutes_seconds, s.minutes, s.seconds);
    return format(seconds, s.seconds);
}

double nice_step(double raw)
{
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    for (const double factor : {1.0, 2.0, 5.0, 10.0})
        if (factor * power >= raw * (1.0 - 1e-9))
            return factor * power;
    return 10.0 * power;
}

} // namespace

LegendModeGroup legend_mode_group(LegendMode mode)
{
    switch (mode) {
    case LegendMode::LineType:
    case LegendMode::Filament: return LegendModeGroup::Categories;
    case LegendMode::Speed:
    case LegendMode::Acceleration:
    case LegendMode::Jerk: return LegendModeGroup::Motion;
    case LegendMode::Flow:
    case LegendMode::LineWidth:
    case LegendMode::LayerHeight:
    case LegendMode::PressureAdvance: return LegendModeGroup::Extrusion;
    case LegendMode::FanSpeed:
    case LegendMode::Temperature: return LegendModeGroup::CoolingAndHeat;
    default: return LegendModeGroup::Time;
    }
}

bool legend_mode_has_variants(LegendMode mode)
{
    return mode == LegendMode::Speed || mode == LegendMode::Flow || mode == LegendMode::LayerTime;
}

libvgcode::EViewType legend_view_type(LegendMode mode, int variant)
{
    using libvgcode::EViewType;
    const bool second = variant == 1;
    switch (mode) {
    case LegendMode::LineType: return EViewType::FeatureType;
    case LegendMode::Filament: return EViewType::ColorPrint;
    case LegendMode::Speed: return second ? EViewType::ActualSpeed : EViewType::Speed;
    case LegendMode::Acceleration: return EViewType::Acceleration;
    case LegendMode::Jerk: return EViewType::Jerk;
    case LegendMode::Flow: return second ? EViewType::ActualVolumetricFlowRate : EViewType::VolumetricFlowRate;
    case LegendMode::LineWidth: return EViewType::Width;
    case LegendMode::LayerHeight: return EViewType::Height;
    case LegendMode::PressureAdvance: return EViewType::PressureAdvance;
    case LegendMode::FanSpeed: return EViewType::FanSpeed;
    case LegendMode::Temperature: return EViewType::Temperature;
    default: return second ? EViewType::LayerTimeLogarithmic : EViewType::LayerTimeLinear;
    }
}

std::optional<LegendModeChoice> legend_mode_of(libvgcode::EViewType type)
{
    for (size_t i = 0; i < LEGEND_MODE_COUNT; ++i) {
        const LegendMode mode = LegendMode(i);
        for (int variant = 0; variant < (legend_mode_has_variants(mode) ? 2 : 1); ++variant)
            if (legend_view_type(mode, variant) == type)
                return LegendModeChoice{mode, variant};
    }
    return std::nullopt;
}

libvgcode::EViewType legend_opened_view_type(libvgcode::EViewType saved)
{
    return saved == libvgcode::EViewType::Summary ? libvgcode::EViewType::ColorPrint : saved;
}

std::vector<double> legend_ticks(double min, double max, size_t max_count)
{
    if (!(max > min) || max_count == 0)
        return {};
    double step = nice_step((max - min) / double(std::max<size_t>(1, max_count)));
    std::vector<double> ticks;
    for (;;) {
        ticks.clear();
        const double eps = step * 1e-9;
        for (double v = std::ceil(min / step - 1e-9) * step; v <= max + eps; v += step)
            ticks.push_back(std::abs(v) < eps ? 0.0 : v);
        if (ticks.size() <= max_count)
            return ticks;
        step = nice_step(step * 1.01);
    }
}

std::vector<double> legend_log_ticks(double min, double max, size_t max_count)
{
    if (!(min > 0.0) || !(max > min) || max_count == 0)
        return {};
    for (const std::vector<double>& factors : {std::vector<double>{1.0, 2.0, 5.0}, std::vector<double>{1.0}}) {
        std::vector<double> ticks;
        for (double power = std::pow(10.0, std::floor(std::log10(min))); power <= max * 10.0; power *= 10.0)
            for (const double factor : factors) {
                const double v = factor * power;
                if (v >= min * (1.0 - 1e-9) && v <= max * (1.0 + 1e-9))
                    ticks.push_back(v);
            }
        if (ticks.size() <= max_count)
            return ticks;
        if (factors.size() == 1) {
            std::vector<double> thinned;
            const size_t        every = (ticks.size() + max_count - 1) / max_count;
            for (size_t i = 0; i < ticks.size(); i += every)
                thinned.push_back(ticks[i]);
            return thinned;
        }
    }
    return {};
}

int legend_ticks_decimals(const std::vector<double>& ticks)
{
    double step = 0.0;
    for (size_t i = 1; i < ticks.size(); ++i) {
        const double gap = ticks[i] - ticks[i - 1];
        if (step == 0.0 || gap < step)
            step = gap;
    }
    if (step <= 0.0) {
        if (ticks.empty())
            return 0;
        step = std::abs(ticks.front());
        if (step == 0.0)
            return 0;
    }
    return std::max(0, int(std::ceil(-std::log10(step) - 1e-9)));
}

std::vector<double> legend_bin_edges(double min, double max, size_t max_count, bool logarithmic)
{
    if (!(max > min) || max_count == 0)
        return {};
    if (logarithmic && min > 0.0) {
        for (const std::vector<double>& factors : {std::vector<double>{1.0, 2.0, 5.0}, std::vector<double>{1.0}}) {
            std::vector<double> edges;
            bool                covered = false;
            for (double power = std::pow(10.0, std::floor(std::log10(min))); !covered; power *= 10.0)
                for (const double factor : factors) {
                    const double v = factor * power;
                    if (v <= min * (1.0 + 1e-9))
                        edges.assign(1, v);
                    else if (!covered)
                        edges.push_back(v);
                    covered = covered || v >= max * (1.0 - 1e-9);
                }
            if (edges.size() - 1 <= max_count || factors.size() == 1)
                return edges;
        }
    }
    double step = nice_step((max - min) / double(max_count));
    for (;;) {
        const double first = std::floor(min / step + 1e-9) * step;
        const double last  = std::ceil(max / step - 1e-9) * step;
        const size_t count = size_t(std::llround((last - first) / step));
        if (count <= max_count) {
            std::vector<double> edges;
            for (size_t i = 0; i <= std::max<size_t>(count, 1); ++i) {
                const double v = first + double(i) * step;
                edges.push_back(std::abs(v) < step * 1e-9 ? 0.0 : v);
            }
            return edges;
        }
        step = nice_step(step * 1.01);
    }
}

size_t legend_bin_of(const std::vector<double>& edges, double value)
{
    if (edges.size() < 2)
        return 0;
    const auto it = std::upper_bound(edges.begin() + 1, edges.end() - 1, value);
    return size_t(it - edges.begin()) - 1;
}

LegendBins legend_merge_bins(const LegendBins& bins, size_t max_filled, double min_share)
{
    const size_t count = bins.amounts.size();
    if (count < 2 || bins.edges.size() != count + 1 || max_filled == 0)
        return bins;
    const double step  = bins.edges[1] - bins.edges[0];
    double       total = 0.0;
    for (const double a : bins.amounts)
        total += a;
    if (!(step > 0.0))
        return bins;
    for (double factor = 1.0;; factor = nice_step(factor * 1.01)) {
        const double wide  = step * factor;
        const double first = std::floor(bins.edges.front() / wide + 1e-9) * wide;
        const size_t merged_count = size_t(std::llround(std::ceil(bins.edges.back() / wide - 1e-9) - first / wide));
        LegendBins   merged;
        for (size_t i = 0; i <= merged_count; ++i)
            merged.edges.push_back(first + double(i) * wide);
        merged.amounts.assign(merged_count, 0.0);
        merged.lowest.assign(merged_count, std::numeric_limits<float>::max());
        merged.highest.assign(merged_count, std::numeric_limits<float>::lowest());
        for (size_t i = 0; i < count; ++i) {
            const size_t m = std::min(merged_count - 1, size_t(std::max(0.0, std::floor((bins.edges[i] - first) / wide + 1e-9))));
            merged.amounts[m] += bins.amounts[i];
            if (i < bins.lowest.size() && bins.amounts[i] > 0.0) {
                merged.lowest[m]  = std::min(merged.lowest[m], bins.lowest[i]);
                merged.highest[m] = std::max(merged.highest[m], bins.highest[i]);
            }
        }
        const size_t filled = size_t(std::count_if(merged.amounts.begin(), merged.amounts.end(),
                                                   [&](double a) { return a > 0.0 && a >= min_share * total; }));
        if (filled <= max_filled || merged_count <= 1)
            return merged;
    }
}

std::vector<LegendLimit> legend_limits(const std::vector<float>& per_filament, const std::vector<int>& used)
{
    std::vector<LegendLimit> limits;
    for (const int number : used) {
        if (number < 1 || size_t(number) > per_filament.size() || !(per_filament[size_t(number - 1)] > 0.f))
            continue;
        const float value = per_filament[size_t(number - 1)];
        const auto  it    = std::find_if(limits.begin(), limits.end(), [value](const LegendLimit& l) { return l.value == value; });
        if (it == limits.end())
            limits.push_back({value, {number}});
        else
            it->filaments.push_back(number);
    }
    std::sort(limits.begin(), limits.end(), [](const LegendLimit& a, const LegendLimit& b) { return a.value < b.value; });
    return limits;
}

bool legend_at_limit(double value, double limit) { return limit > 0.0 && value >= 0.98 * limit; }

bool legend_under_limit(double value, double limit) { return limit > 0.0 && value < limit; }

size_t legend_changed_settings(const LegendSettings& settings)
{
    const LegendSettings defaults;
    return size_t(legend_columns_text(settings) != legend_columns_text(defaults)) + size_t(settings.histogram != defaults.histogram) +
           size_t(settings.hover != defaults.hover) + size_t(settings.range != defaults.range) +
           size_t(settings.palette != defaults.palette);
}

std::string legend_columns_text(const LegendSettings& settings)
{
    std::string text;
    for (const auto& [on, name] : {std::pair<bool, const char*>{settings.time, "time"}, {settings.share, "share"}, {settings.weight, "weight"},
                                   {settings.length, "length"}})
        if (on)
            text += (text.empty() ? "" : ",") + std::string(name);
    return text;
}

void legend_read_columns(LegendSettings& settings, const std::string& text)
{
    LegendSettings read = settings;
    read.time = read.share = read.weight = read.length = false;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t      end  = std::min(text.find(',', begin), text.size());
        const std::string name = text.substr(begin, end - begin);
        read.time |= name == "time";
        read.share |= name == "share";
        read.weight |= name == "weight";
        read.length |= name == "length";
        begin = end + 1;
    }
    if (read.time || read.share || read.weight || read.length)
        settings = read;
}

LegendSpread legend_spread(size_t distinct_values)
{
    if (distinct_values <= 1)
        return LegendSpread::One;
    return distinct_values <= LEGEND_FEW_VALUES ? LegendSpread::Few : LegendSpread::Scale;
}

std::string legend_time(float seconds, const LegendUnits& units)
{
    return time_in(seconds, units.days_hours, units.hours_minutes, units.minutes_seconds, units.seconds, units.under_second);
}

std::string legend_long_time(float seconds, const LegendUnits& units)
{
    return time_in(seconds, units.long_days_hours, units.long_hours_minutes, units.long_minutes_seconds, units.long_seconds,
                   units.under_second);
}

std::string legend_count(unsigned long long count)
{
    static const std::string NO_BREAK_SPACE = "\xC2\xA0";
    std::string digits = std::to_string(count);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += NO_BREAK_SPACE;
        out += digits[i];
    }
    return out;
}

std::string legend_percent(double share)
{
    const double percent = share * 100.0;
    if (percent <= 0.0)
        return "0";
    if (percent < 0.05)
        return "<0.1";
    return fixed(percent, 1);
}

std::string legend_mass_value(double grams, bool imperial) { return fixed(imperial ? grams / GRAMS_PER_OUNCE : grams, 2); }

std::string legend_mass(double grams, bool imperial, const LegendUnits& units)
{
    if (imperial)
        return format(units.ounces, fixed(grams / GRAMS_PER_OUNCE, 2));
    if (std::abs(grams) >= 1000.0)
        return format(units.kilograms, fixed(grams / 1000.0, 2));
    return format(units.grams, fixed(grams, 2));
}

std::string legend_filament_length(double mm, bool imperial, const LegendUnits& units)
{
    if (imperial)
        return format(units.inches, fixed(mm / MM_PER_INCH, 2));
    return format(units.meters, fixed(mm / 1000.0, 2));
}

std::string legend_length_value(double mm, bool imperial) { return fixed(imperial ? mm / MM_PER_INCH : mm / 1000.0, 2); }

std::string legend_distance(double mm, bool imperial, const LegendUnits& units)
{
    if (imperial)
        return format(units.inches, fixed(mm / MM_PER_INCH, 2));
    if (std::abs(mm) < 1000.0)
        return format(units.millimeters, fixed(mm, 0));
    return format(units.meters, fixed(mm / 1000.0, 2));
}

std::string legend_value(double value, int decimals) { return fixed(value, decimals); }

int legend_value_decimals(LegendMode mode)
{
    switch (mode) {
    case LegendMode::Jerk:
    case LegendMode::LayerTime: return 1;
    case LegendMode::Flow:
    case LegendMode::LineWidth:
    case LegendMode::LayerHeight: return 2;
    case LegendMode::PressureAdvance: return 3;
    default: return 0;
    }
}

std::string legend_value_key(double value, int decimals) { return legend_value(value, decimals + LEGEND_EXTRA_DECIMALS); }

std::optional<int> legend_category_decimals(const std::vector<float>& values, int decimals, bool times)
{
    const auto apart = [&values](const auto& label) {
        std::set<std::string> labels;
        for (const float value : values)
            labels.insert(label(value));
        return labels.size() == values.size();
    };
    if (times)
        return apart([](float value) { return legend_time(value, LegendUnits()); }) ? std::optional<int>(decimals) : std::nullopt;
    for (int d = decimals; d <= decimals + LEGEND_EXTRA_DECIMALS; ++d)
        if (apart([d](float value) { return legend_value(value, d); }))
            return d;
    return std::nullopt;
}

LegendSettings legend_load_settings(const AppConfig& config)
{
    LegendSettings settings;
    legend_read_columns(settings, config.get("legend_columns"));
    const auto flag = [&config](const char* key, bool& value) {
        const std::string text = config.get(key);
        if (text == "1" || text == "0")
            value = text == "1";
    };
    flag("legend_histogram", settings.histogram);
    flag("legend_hover", settings.hover);
    if (config.get("legend_range") == "visible")
        settings.range = LegendRange::VisibleLayers;
    if (config.get("legend_palette") == "perceptual")
        settings.palette = LegendPalette::Perceptual;
    return settings;
}

void legend_save_settings(AppConfig& config, const LegendSettings& settings)
{
    config.set("legend_columns", legend_columns_text(settings));
    config.set("legend_histogram", settings.histogram ? "1" : "0");
    config.set("legend_hover", settings.hover ? "1" : "0");
    config.set("legend_range", settings.range == LegendRange::VisibleLayers ? "visible" : "whole");
    config.set("legend_palette", settings.palette == LegendPalette::Perceptual ? "perceptual" : "classic");
}

} // namespace GUI
} // namespace Slic3r
