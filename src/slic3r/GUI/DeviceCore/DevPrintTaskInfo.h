#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// TODO classes to handle dev print task management and ratings

namespace Slic3r
{

struct DevPrintPausePoint
{
    int m_progressPercent = 0;
    int m_remainingTime   = 0; // minutes of print time left when the pause happens
    int m_pauseIndex      = 0;
    int m_layer           = 0;
};

struct DevPrintPauseList
{
    int                             m_total = 0;
    std::vector<DevPrintPausePoint> m_points;

    // Returns how many pause points precede the next pending pause.
    int getPassedCount() const;
};

struct DevPrintTaskRatingInfo
{
    bool        request_successful;
    int         http_code;
    int         rating_id;
    int         start_count;
    bool        success_printed;
    std::string content;
    std::vector<std::string>  image_url_paths;
};

class DevPrintTaskInfo
{
public:
    // Parses the pause schedule reported in print.p_list. Invalid updates leave the last valid schedule unchanged.
    void parse(const nlohmann::json &printInfo);
    void reset() { m_pauseList.reset(); }

    const std::optional<DevPrintPauseList> &getPauseList() const { return m_pauseList; }

private:
    std::optional<DevPrintPauseList> m_pauseList;
};

}// end namespace Slic3r
