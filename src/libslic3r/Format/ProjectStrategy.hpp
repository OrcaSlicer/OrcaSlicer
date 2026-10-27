#pragma once

#include <functional>
#include <type_traits>

// Flags steering how a project (3MF) is loaded and saved. Kept apart from bbs_3mf.hpp so
// headers that only name the flags, such as Plater.hpp, need not pull in the 3MF reader.

namespace Slic3r {

enum class LoadStrategy
{
    Default = 0,
    AddDefaultInstances = 1,
    CheckVersion = 2,
    LoadModel = 4,
    LoadConfig = 8,
    LoadAuxiliary = 16,
    Silence = 32,
    ImperialUnits = 64,

    Restore = 0x10000 | LoadModel | LoadConfig | LoadAuxiliary | Silence,
};

inline LoadStrategy operator | (LoadStrategy lhs, LoadStrategy rhs)
{
    using T = std::underlying_type_t <LoadStrategy>;
    return static_cast<LoadStrategy>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

inline bool operator & (LoadStrategy & lhs, LoadStrategy rhs)
{
    using T = std::underlying_type_t <LoadStrategy>;
    return (static_cast<T>(lhs) & static_cast<T>(rhs)) == static_cast<T>(rhs);
}

enum class SaveStrategy
{
    Default = 0,
    FullPathSources     = 1,
    Zip64               = 1 << 1,
    ProductionExt       = 1 << 2,
    SecureContentExt    = 1 << 3,
    WithGcode           = 1 << 4,
    Silence             = 1 << 5,
    SkipStatic          = 1 << 6,
    SkipModel           = 1 << 7,
    WithSliceInfo       = 1 << 8,
    SkipAuxiliary       = 1 << 9,
    UseLoadedId         = 1 << 10,
    ShareMesh           = 1 << 11,
    // Keep this separate from SplitModel, which uses the 0x1000 bit as part of its
    // production-extension value.
    MinimalPublished    = 1 << 13,
    // With WithGcode, also the G-code's statistics.
    WithStatistics      = 1 << 14,

    SplitModel = 0x1000 | ProductionExt,
    Encrypted  = SecureContentExt | SplitModel,
    Backup = 0x10000 | WithGcode | Silence | SkipStatic | SplitModel,
};

inline SaveStrategy operator | (SaveStrategy lhs, SaveStrategy rhs)
{
    using T = std::underlying_type_t <SaveStrategy>;
    return static_cast<SaveStrategy>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

inline bool operator & (SaveStrategy & lhs, SaveStrategy rhs)
{
    using T = std::underlying_type_t <SaveStrategy>;
    return ((static_cast<T>(lhs) & static_cast<T>(rhs))) == static_cast<T>(rhs);
}

// Progress callback of the 3MF export; cancel may be set to stop it.
typedef std::function<void(int export_stage, int current, int total, bool& cancel)> Export3mfProgressFn;

} // namespace Slic3r
