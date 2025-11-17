#pragma once

namespace Slic3r
{

class DevFilaSystem;
class DevAmsSystemSetting
{
public:
    DevAmsSystemSetting(DevFilaSystem* owner) : m_owner(owner) {};

public:
    // getters
    bool IsDetectOnInsertEnabled() const { return m_enable_detect_on_insert; };
    bool IsDetectOnPowerupEnabled() const { return m_enable_detect_on_powerup; }
    bool IsDetectRemainEnabled() const { return m_enable_detect_remain; }
    bool IsAutoRefillEnabled() const { return m_enable_auto_refill; }

    // setters
    void Reset();
    void SetDetectOnInsertEnabled(bool enable) { m_enable_detect_on_insert = enable; }
    void SetDetectOnPowerupEnabled(bool enable) { m_enable_detect_on_powerup = enable; }
    void SetDetectRemainEnabled(bool enable) { m_enable_detect_remain = enable; }
    void SetAutoRefillEnabled(bool enable) { m_enable_auto_refill = enable; }

private:
    DevFilaSystem* m_owner = nullptr;

    bool m_enable_detect_on_insert = false;
    bool m_enable_detect_on_powerup = false;
    bool m_enable_detect_remain = false;
    bool m_enable_auto_refill = false;
};

}// namespace Slic3r