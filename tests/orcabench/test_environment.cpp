#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/Policy.hpp"
#include "core/Runner.hpp"
#include "slicer/Environment.hpp"

#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/core.hpp>
#include <tbb/global_control.h>

#include <cstddef>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Bench;

namespace {

Policy at_threads(unsigned threads)
{
    PolicyOverrides overrides;
    overrides.threads = threads;
    return Policy::resolve("quick", overrides, threads);
}

// Inside the environment for the scope's lifetime, as the Runner keeps a run.
class Entered
{
public:
    Entered(RunEnvironment& environment, const Policy& policy) : m_environment(environment) { m_environment.enter(policy); }
    ~Entered() { m_environment.leave(); }

    Entered(const Entered&)            = delete;
    Entered& operator=(const Entered&) = delete;

private:
    RunEnvironment& m_environment;
};

std::size_t thread_cap() { return tbb::global_control::active_value(tbb::global_control::max_allowed_parallelism); }

} // namespace

TEST_CASE("a run caps TBB at its policy's threads, and leaving lifts the cap", "[OrcaBench][Environment]")
{
    const std::size_t before = thread_cap();
    SlicerEnvironment environment(ORCABENCH_RESOURCES_DIR);
    {
        const Entered entered(environment, at_threads(1));
        CHECK(thread_cap() == 1);
    }
    CHECK(thread_cap() == before);
}

TEST_CASE("a run reads the resources it is given and writes to a directory of its own, and leaving puts both back",
          "[OrcaBench][Environment]")
{
    const std::string       resources = resources_dir();
    const std::string       temporary = temporary_dir();
    boost::filesystem::path own;
    SlicerEnvironment       environment(ORCABENCH_RESOURCES_DIR);
    {
        const Entered entered(environment, at_threads(1));
        CHECK(resources_dir() == ORCABENCH_RESOURCES_DIR);
        own = temporary_dir();
        CHECK(own != temporary);
        CHECK(boost::filesystem::is_directory(own));
        CHECK(boost::filesystem::is_empty(own));
    }
    CHECK(resources_dir() == resources);
    CHECK(temporary_dir() == temporary);
    CHECK_FALSE(boost::filesystem::exists(own));
}

TEST_CASE("a run reads paths as UTF-8, as libslic3r writes them, and leaving puts the old encoding back", "[OrcaBench][Environment]")
{
    const auto* const before = &boost::filesystem::path::codecvt();
    SlicerEnvironment environment(ORCABENCH_RESOURCES_DIR);
    {
        const Entered entered(environment, at_threads(1));
        CHECK(boost::filesystem::path("\xc3\xa9").wstring() == L"é");
    }
    CHECK(&boost::filesystem::path::codecvt() == before);
}

TEST_CASE("a run logs nothing, and leaving puts logging back as it was", "[OrcaBench][Environment]")
{
    const bool logging = GENERATE(false, true);
    boost::log::core::get()->set_logging_enabled(logging);
    SlicerEnvironment environment(ORCABENCH_RESOURCES_DIR);
    {
        const Entered entered(environment, at_threads(1));
        CHECK_FALSE(boost::log::core::get()->get_logging_enabled());
    }
    CHECK(boost::log::core::get()->get_logging_enabled() == logging);
}

TEST_CASE("a run capped at one thread still lets a first slice name the pool's threads", "[OrcaBench][Environment]")
{
    SlicerEnvironment environment(ORCABENCH_RESOURCES_DIR);
    const Entered     entered(environment, at_threads(1));
    // What Print::process() calls first, which returns only once every thread of the arena has run.
    name_tbb_thread_pool_threads_set_locale();
    SUCCEED();
}
