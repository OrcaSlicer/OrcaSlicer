#pragma once

#include "core/Policy.hpp"
#include "core/Runner.hpp"

#include <boost/filesystem/path.hpp>
#include <tbb/global_control.h>

#include <locale>
#include <optional>
#include <string>

namespace Slic3r { namespace Bench {

// What a run sets up around libslic3r: paths in UTF-8 as libslic3r writes them, logging off, one TBB
// thread cap for the whole run, the resources the slicer reads and a temporary directory of the run's
// own, each put back on leave().
class SlicerEnvironment : public RunEnvironment
{
public:
    explicit SlicerEnvironment(std::string resources_dir);

    void enter(const Policy& policy) override;
    void leave() noexcept override;

private:
    std::string                        m_resources_dir;
    std::locale                        m_previous_path_locale;
    std::string                        m_previous_resources_dir;
    std::string                        m_previous_temporary_dir;
    boost::filesystem::path            m_temporary_dir;
    bool                               m_logging_was_enabled = true;
    std::optional<tbb::global_control> m_thread_cap;
};

// The threads TBB runs without a cap, which follows the process's affinity, as the hardware thread count a
// policy resolves against.
unsigned hardware_threads();

}} // namespace Slic3r::Bench
