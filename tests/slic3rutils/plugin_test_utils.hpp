#pragma once

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>

#include <string>

#include "test_utils.hpp"

namespace Slic3r {

// Point data_dir() at a throwaway directory for the lifetime of a test and
// restore the previous value afterwards, so code under test writes into a
// disposable tree and tests don't leak state into each other.
struct ScopedDataDir
{
    ScopedTemporaryDir        tmp;   // owns the temp dir (create + recursive remove)
    boost::filesystem::path   dir;   // = tmp.path(); kept as a member for callers
    std::string               previous;

    explicit ScopedDataDir(const std::string& tag)
        : tmp("orca-" + tag), dir(tmp.path()), previous(data_dir())
    {
        set_data_dir(dir.string());
    }

    ~ScopedDataDir() { set_data_dir(previous); } // tmp removes the directory

    // The plugin manager scans {data_dir}/orca_plugins.
    boost::filesystem::path plugins_dir() const { return dir / "orca_plugins"; }

    ScopedDataDir(const ScopedDataDir&)            = delete;
    ScopedDataDir& operator=(const ScopedDataDir&) = delete;
};

} // namespace Slic3r
