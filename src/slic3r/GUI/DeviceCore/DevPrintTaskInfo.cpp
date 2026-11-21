#include "DevPrintTaskInfo.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r
{
namespace
{

std::optional<int> readInteger(const nlohmann::json &object, const char *key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number_integer())
        return std::nullopt;

    try {
        return value->get<int>();
    } catch (const nlohmann::json::exception &) {
        return std::nullopt;
    }
}

std::optional<DevPrintPauseList> parsePauseList(const nlohmann::json &pauseListJson)
{
    if (!pauseListJson.is_object())
        return std::nullopt;

    const auto total = readInteger(pauseListJson, "total");
    const auto list  = pauseListJson.find("list");
    if (!total || list == pauseListJson.end() || !list->is_array())
        return std::nullopt;

    DevPrintPauseList pauseList;
    pauseList.m_total = *total;
    pauseList.m_points.reserve(list->size());
    for (const auto &pointJson : *list) {
        if (!pointJson.is_object())
            return std::nullopt;

        const auto progressPercent = readInteger(pointJson, "p");
        const auto remainingTime   = readInteger(pointJson, "t");
        const auto pauseIndex      = readInteger(pointJson, "i");
        const auto layer           = readInteger(pointJson, "l");
        if (!progressPercent || !remainingTime || !pauseIndex || !layer)
            return std::nullopt;

        pauseList.m_points.push_back({*progressPercent, *remainingTime, *pauseIndex, *layer});
    }
    return pauseList;
}

} // namespace

int DevPrintPauseList::getPassedCount() const
{
    const int total = std::max(0, m_total);
    if (m_points.empty())
        return total;

    const auto nextPause = std::min_element(m_points.begin(), m_points.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.m_pauseIndex < rhs.m_pauseIndex;
    });
    return std::clamp(nextPause->m_pauseIndex, 0, total);
}

void DevPrintTaskInfo::parse(const nlohmann::json &printInfo)
{
    // The schedule belongs to one task: once the printer is idle or the task has ended, the next task must not
    // inherit it (a printer may omit p_list for a task without pauses).
    const auto state = printInfo.find("gcode_state");
    if (state != printInfo.end() && state->is_string()) {
        const std::string &gcodeState = state->get_ref<const std::string &>();
        if (gcodeState == "IDLE" || gcodeState == "FINISH" || gcodeState == "FAILED")
            m_pauseList.reset();
    }

    const auto pauseListJson = printInfo.find("p_list");
    if (pauseListJson == printInfo.end())
        return;

    auto pauseList = parsePauseList(*pauseListJson);
    if (pauseList)
        m_pauseList = std::move(*pauseList);
}

} // namespace Slic3r
