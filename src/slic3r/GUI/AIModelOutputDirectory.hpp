#pragma once

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/process/environment.hpp>

namespace Slic3r::GUI {

class AIModelOutputDirectory
{
public:
    explicit AIModelOutputDirectory(const boost::filesystem::path& user_data_directory)
    {
        const char* configured = boost::nowide::getenv("ORCASLICER_AI_OUTPUT_DIR");
        const boost::filesystem::path selected = configured != nullptr && configured[0] != '\0'
            ? boost::filesystem::path(configured) : user_data_directory / "generated_models";
        m_root = boost::filesystem::absolute(selected).lexically_normal();
    }

    const boost::filesystem::path& root() const { return m_root; }

    void configure_child_environment(boost::process::environment& environment) const
    {
        environment["ORCASLICER_AI_OUTPUT_DIR"] = m_root.string();
    }

#ifdef _WIN32
    void configure_child_environment(boost::process::wenvironment& environment) const
    {
        environment[L"ORCASLICER_AI_OUTPUT_DIR"] = m_root.wstring();
    }
#endif

private:
    boost::filesystem::path m_root;
};

inline const AIModelOutputDirectory& ai_model_output_directory()
{
    // The feature host initializes this after Orca selects data_dir(), before
    // interactive file dialogs can change the process working directory.
    static const AIModelOutputDirectory directory(Slic3r::data_dir());
    return directory;
}

} // namespace Slic3r::GUI
