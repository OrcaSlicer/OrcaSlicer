#pragma once

// CommonDefs.hpp
// ---------------
// This header provides common definitions and enumerations shared across multiple libraries.
// It is intended for use in projects that require consistent type definitions, such as nozzle types.
// The contents of this file are designed to be reusable and maintainable for cross-library integration.

namespace Slic3r
{
    // BBS
    enum NozzleType
    {
        ntUndefine = 0,
        ntHardenedSteel,
        ntStainlessSteel,
        ntTungstenCarbide,
        ntBrass,
        ntE3D,
        ntCount
    };
}