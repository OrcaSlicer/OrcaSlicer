#pragma once

#include <string>

namespace Slic3r
{

    class MachineObject;
    class DevLamp
    {
    public:
        DevLamp(MachineObject* obj) : m_owner(obj) {};

    public:
        enum LIGHT_EFFECT
        {
            LIGHT_EFFECT_ON,
            LIGHT_EFFECT_OFF,
            LIGHT_EFFECT_FLASHING,
            LIGHT_EFFECT_UNKOWN,
        };

    public:
        void SetChamberLight(const std::string& status);
        void SetChamberLight(LIGHT_EFFECT effect) { m_chamber_light = effect; }
        bool IsChamberLightOn() const { return m_chamber_light == LIGHT_EFFECT_ON || m_chamber_light == LIGHT_EFFECT_FLASHING; }

        void SetLampCloseRecheck(bool enable) { m_lamp_close_recheck = enable;};
        bool HasLampCloseRecheck() const { return m_lamp_close_recheck; }

    public:
        void CtrlSetChamberLight(LIGHT_EFFECT effect);

    private:
        int command_set_chamber_light(LIGHT_EFFECT effect, int on_time = 500, int off_time = 500, int loops = 1, int interval = 1000);
        int command_set_chamber_light2(LIGHT_EFFECT effect, int on_time = 500, int off_time = 500, int loops = 1, int interval = 1000);

    private:
        MachineObject* m_owner = nullptr;

        bool m_lamp_close_recheck = false;
        LIGHT_EFFECT m_chamber_light = LIGHT_EFFECT_UNKOWN;
    };
}