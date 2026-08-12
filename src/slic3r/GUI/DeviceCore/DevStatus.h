#pragma once
#include <optional>
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

#include "DevDefs.h"

namespace Slic3r {

enum DevJobState
{
    JobStateIdle      = 0,
    JobStateSlicing   = 1,
    JobStatePrepare   = 2,
    JobStateStarting  = 3,
    JobStateRunning   = 4,
    JobStatePause     = 5,
    JobStatePausing   = 6,
    JobStateResuming  = 7,
    JobStateFinish    = 8,
    JobStateFailed    = 9,
    JobStateFinishing = 10,
    JobStateStoppping = 11,
};

class MachineObject;
class DevStatus
{
public:
    DevStatus(MachineObject* owner) : m_owner(owner) {};

public:
    std::optional<DevJobState> GetJobState() const { return m_job_state; }

    // Parse
    void ParseStatus(const nlohmann::json& print_jj);

private:
    MachineObject *m_owner = nullptr;
    std::optional<DevJobState> m_job_state; // could be nullopt for some old firmware
};

} // namespace Slic3r