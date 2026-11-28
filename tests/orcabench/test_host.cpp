#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Host.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace Slic3r::Bench;

// Assert relationships, never absolute times or sizes; those vary by machine and allocator.

TEST_CASE("resident memory is reported for a running process", "[OrcaBench][Host]")
{
    REQUIRE(current_rss_bytes() > 0);
}

TEST_CASE("resident memory grows when pages are actually touched", "[OrcaBench][Host]")
{
    // The compiler must assume a volatile is read, so a buffer whose address is stored here can be
    // neither dropped nor filled after the measurement that follows.
    static const unsigned char* volatile escaped = nullptr;

    const std::uint64_t before = current_rss_bytes();
    REQUIRE(before > 0);

    constexpr std::size_t megabytes = 64;
    const std::vector<unsigned char> ballast(megabytes * 1024 * 1024, 1);
    escaped = ballast.data();

    const std::uint64_t after = current_rss_bytes();
    escaped = nullptr;
    CHECK(after > before);
}

TEST_CASE("peak resident memory is never below the current figure", "[OrcaBench][Host]")
{
    const std::uint64_t current = current_rss_bytes();
    const std::uint64_t peak    = peak_rss_bytes();
    REQUIRE(peak > 0);
    CHECK(peak >= current);
}

TEST_CASE("process CPU time never moves backwards", "[OrcaBench][Host]")
{
    // On Windows the counter moves in ~15 ms ticks and reads 0 until one has passed. The
    // volatile sink keeps the loop that burns one from being optimized away.
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < 50'000'000ull; ++i)
        sink += i * 2654435761ull;
    CHECK(sink != 0);

    // Without this, a stub returning 0 would satisfy monotonicity and pass.
    const std::uint64_t first = process_cpu_ns();
    REQUIRE(first > 0);
    CHECK(process_cpu_ns() >= first);
}

TEST_CASE("machine queries return something other than their fallback", "[OrcaBench][Host]")
{
    // "unknown" is each query's fallback when the platform does not answer.
    CHECK(host_name() != "unknown");
    CHECK(os_description() != "unknown");
    CHECK(cpu_model() != "unknown");
    CHECK(logical_cores() > 0);
}

TEST_CASE("machine identity is the same on every call", "[OrcaBench][Host]")
{
    CHECK(host_name() == host_name());
    CHECK(os_description() == os_description());
    CHECK(cpu_model() == cpu_model());
    CHECK(logical_cores() == logical_cores());
}

TEST_CASE("CPU time advances in clock ticks on Windows, in scheduler ticks on Linux and finely on macOS", "[OrcaBench][Host]")
{
#ifdef _WIN32
    CHECK(cpu_time_step() == std::chrono::microseconds(15625));
#elif defined(__linux__)
    CHECK(cpu_time_step() == std::chrono::milliseconds(4));
#else
    CHECK(cpu_time_step() == std::chrono::nanoseconds::zero());
#endif
}

TEST_CASE("identity strings have no padding or embedded nulls", "[OrcaBench][Host]")
{
    for (const std::string& value : { host_name(), os_description(), cpu_model() }) {
        CAPTURE(value);
        REQUIRE_FALSE(value.empty());
        CHECK(value.find('\0') == std::string::npos);
        CHECK(value.front() != ' ');
        CHECK(value.back() != ' ');
    }
}
