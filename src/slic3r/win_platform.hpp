#pragma once

// Force-included into libslic3r_gui when SLIC3R_PCH is OFF, standing in for
// pchheader.hpp, which includes <Windows.h> before anything else. Arriving
// late and transitively, rpcndr.h defines a global `byte` that collides with
// std::byte under `using namespace std`, and the control and URL moniker
// types the GUI uses are never declared.
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
    #include <urlmon.h>
#endif // _WIN32

