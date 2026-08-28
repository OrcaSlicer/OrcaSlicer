#pragma once

#include <string>

namespace Slic3r {

enum LiveviewLocal {
    LVL_None,
    LVL_Disable,
    LVL_Local,
    LVL_Rtsps,
    LVL_Rtsp
};

enum LiveviewRemote {
    LVR_None,
    LVR_Tutk,
    LVR_Agora,
    LVR_TutkAgora
};

enum FileLocal {
    FL_None,
    FL_Local
};

enum FileRemote {
    FR_None,
    FR_Tutk,
    FR_Agora,
    FR_TutkAgora
};

} // namespace Slic3r
