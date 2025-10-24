#include "DevFilaAmsSetting.h"

namespace Slic3r {

void DevAmsSystemSetting::Reset()
{
    SetDetectOnInsertEnabled(false);
    SetDetectOnPowerupEnabled(false);
    SetDetectRemainEnabled(false);
    SetAutoRefillEnabled(false);
}

}