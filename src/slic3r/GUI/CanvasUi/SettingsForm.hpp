#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Availability.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;
class TextMeasure;

// A switch, or a choice among its options.
struct SettingsField
{
    const char*              id{""};
    std::string              label;
    std::vector<std::string> options;
    // When the form has tabs.
    int                      tab{0};
    // Read as the field is drawn, after the reset and the fields above it; none for a field always available.
    std::function<Availability()> availability;
    // The option chosen, or 1 for a switch that is on.
    std::function<int()>     get;
    std::function<void(int)> set;
};

SettingsField settings_switch(const char* id, const std::string& label, bool& value);

// A panel's settings, open under its header: a bar with the tabs, the count of the settings changed from their defaults and
// a reset, and under it the fields of the tab shown.
struct SettingsFormOptions
{
    float                      width{0.f};
    // As tall as it is shown, at most settings_form_height(); the rest scrolls.
    float                      height{0.f};
    // How far the content under the form reaches down; the form's shadow falls on it.
    float                      below{0.f};
    std::vector<SettingsField> fields;
    std::vector<std::string>   tabs;
    int*                       tab{nullptr};
    // The counter is shown and the reset can be pressed only while some settings are changed.
    size_t                     changed{0};
    std::string                changed_count;
    std::string                reset_label;
    std::function<void()>      reset;
};

float settings_form_height(const Metrics& metrics, const TextMeasure& measure, const TextMeasure& choices, const SettingsFormOptions& form);

// The form at pos; whether a field or the reset changed a setting.
bool settings_form(const char* id, const Point& pos, const SettingsFormOptions& form);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
