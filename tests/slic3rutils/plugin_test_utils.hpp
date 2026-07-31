#pragma once

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>

#include <string>

namespace Slic3r {

// Point data_dir() at a throwaway directory for the lifetime of a test and
// restore the previous value afterwards, so code under test writes into a
// disposable tree and tests don't leak state into each other.
struct ScopedDataDir
{
    std::string               previous;
    boost::filesystem::path   dir;

    explicit ScopedDataDir(const std::string& tag)
    {
        namespace fs = boost::filesystem;
        previous     = data_dir();
        dir          = fs::temp_directory_path() / fs::unique_path("orca-" + tag + "-%%%%-%%%%");
        fs::create_directories(dir);
        set_data_dir(dir.string());
    }

    ~ScopedDataDir()
    {
        set_data_dir(previous);
        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }

    ScopedDataDir(const ScopedDataDir&)            = delete;
    ScopedDataDir& operator=(const ScopedDataDir&) = delete;
};

} // namespace Slic3r
