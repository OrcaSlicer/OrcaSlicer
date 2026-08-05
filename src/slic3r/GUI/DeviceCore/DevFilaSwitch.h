#pragma once
#include "DevDefs.h"
#include <optional>

namespace Slic3r
{
    // Previous definitions
class MachineObject;
class DevExtder;
class DevAmsTray;

class DevFilaSwitch
{
public:
    enum class CaliStatus : int
    {        
        CALI_IDLE = 0,
        CALI_STEPING = 1,
    };

    enum class CaliStep : int
    {
        CALI_IDLE = 0,
        CALI_SWITCHING = 1,
        CALI_SWITCH_CHECK = 2,
        CALI_FILA_CHECK = 3,
        CALI_FILA_TO_AMS = 4,
        CALI_FILA_TO_SWITCH = 5,
        CALI_FILA_BACK = 9,
        CALI_FINISHED = 14,
    };

    enum SwitchPos : int
    {
        POS_IN_B = 0,
        POS_IN_A = 1,
    };

public:
    DevFilaSwitch(MachineObject* owner);
    virtual ~DevFilaSwitch() = default;

public:
    bool IsInstalled() const { return m_is_installed; };
    bool IsReady() const;

    std::optional<bool> IsInA_HasFilament() const { return m_in_a_has_filament; };
    std::optional<bool> IsInB_HasFilament() const { return m_in_b_has_filament; };

    std::optional<DevAmsTray> GetInA_Slot() const;
    std::optional<DevAmsTray> GetInB_Slot() const;
    std::optional<DevAmsSlotId> GetInA_SlotId() const { return m_in_a_slot; };
    std::optional<DevAmsSlotId> GetInB_SlotId() const { return m_in_b_slot; };

    std::optional<DevExtder> GetOutA_Extruder() const;
    std::optional<DevExtder> GetOutB_Extruder() const;
    std::optional<int> GetOutA_ExtruderId() const { return m_out_a_extruder_id; };
    std::optional<int> GetOutB_ExtruderId() const { return m_out_b_extruder_id; };

    CaliStatus GetCaliStatus() const { return m_cali_status; };

    void Reset();
    void ParseFilaSwitchInfo(const nlohmann::json& print_jj);

private:
    MachineObject* m_owner;

    bool m_is_installed = false;

    std::optional<bool> m_in_a_has_filament;
    std::optional<bool> m_in_b_has_filament;

    std::optional<DevAmsSlotId> m_in_a_slot;
    std::optional<DevAmsSlotId> m_in_b_slot;

    std::optional<int> m_out_a_extruder_id;
    std::optional<int> m_out_b_extruder_id;

    CaliStatus m_cali_status = CaliStatus::CALI_IDLE;
};
};