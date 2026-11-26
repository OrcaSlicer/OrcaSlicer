#include "NumberField.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ios>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

#include <imgui/imgui.h>

#include "Availability.hpp"
#include "ComponentParts.hpp"
#include "Context.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

namespace {

// The filter does not see where the character goes, so a misplaced minus is left to parse_integer().
int digits_only(ImGuiInputTextCallbackData* data)
{
    const ImWchar c = data->EventChar;
    return (c >= '0' && c <= '9') || c == '-' ? 0 : 1;
}

int decimal_only(ImGuiInputTextCallbackData* data)
{
    const ImWchar c = data->EventChar;
    return (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ',' ? 0 : 1;
}

} // namespace

bool number_field(const char* id, long long& value, const NumberFieldOptions& options)
{
    const Availability& availability = options.availability;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", value);
    push_field_style();
    if (!availability.enabled)
        ImGui::PushStyleColor(ImGuiCol_Text, im(theme().disabled_foreground));
    ImGui::SetNextItemWidth(options.width);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackCharFilter |
                                      (availability.enabled ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_ReadOnly);
    const bool confirmed = ImGui::InputText(id, buffer, sizeof(buffer), flags, digits_only) && availability.enabled;
    if (!availability.enabled)
        ImGui::PopStyleColor();
    focus_ring();
    pop_field_style();
    reason_tooltip(availability);
    if (!confirmed)
        return false;
    const std::optional<long long> parsed = parse_integer(buffer);
    if (parsed)
        value = *parsed;
    return parsed.has_value();
}

std::optional<long long> parse_integer(const std::string& text)
{
    if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) || text.front() == '+')
        return std::nullopt;
    char* end = nullptr;
    errno     = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0')
        return std::nullopt;
    return value;
}

bool number_field(const char* id, double& value, int decimals, const NumberFieldOptions& options)
{
    const Availability& availability = options.availability;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(0, decimals), value);
    push_field_style();
    if (!availability.enabled)
        ImGui::PushStyleColor(ImGuiCol_Text, im(theme().disabled_foreground));
    ImGui::SetNextItemWidth(options.width);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackCharFilter |
                                      (availability.enabled ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_ReadOnly);
    const bool edited = ImGui::InputText(id, buffer, sizeof(buffer), flags, decimal_only) && availability.enabled;
    if (!availability.enabled)
        ImGui::PopStyleColor();
    focus_ring();
    pop_field_style();
    reason_tooltip(availability);
    if (!edited)
        return false;
    const std::optional<double> parsed = parse_decimal(buffer);
    if (parsed)
        value = *parsed;
    return parsed.has_value();
}

std::optional<double> parse_decimal(const std::string& text)
{
    if (text.empty() || text.front() == '+')
        return std::nullopt;
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), ',', '.');
    if (std::count(normalized.begin(), normalized.end(), '.') > 1)
        return std::nullopt;
    std::istringstream stream(normalized);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> std::noskipws >> value;
    if (stream.fail() || !stream.eof())
        return std::nullopt;
    return value;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
