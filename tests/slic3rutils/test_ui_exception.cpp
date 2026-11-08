#include <catch2/catch_all.hpp>

#include <exception>
#include <new>
#include <stdexcept>
#include <string>

#include <boost/format.hpp>

#include "slic3r/Utils/UiException.hpp"

using namespace Slic3r;

namespace {

// What the main loop is handed when a UI handler throws: the exception in flight.
template<class E> std::exception_ptr thrown_by_a_handler(const E &ex)
{
    try {
        throw ex;
    } catch (...) {
        return std::current_exception();
    }
}

} // namespace

TEST_CASE("A std::exception from a UI handler is reported and the app keeps running", "[UiException]")
{
    bool              reported = false;
    UiException::Info seen;
    const bool        keep_running = UiException::recover(
        thrown_by_a_handler(std::runtime_error("Voronoi cell source index is out of range!")),
        [&](const UiException::Info &info) {
            reported = true;
            seen     = info;
        });
    CHECK(keep_running);
    CHECK(reported);
    CHECK_FALSE(seen.fatal);
    CHECK(seen.type == "std::runtime_error");
    CHECK(seen.what == "Voronoi cell source index is out of range!");
}

TEST_CASE("Out of memory, a broken translation and non-standard exceptions still end the app", "[UiException]")
{
    const std::exception_ptr fatal = GENERATE(thrown_by_a_handler(std::bad_alloc()),
                                              thrown_by_a_handler(boost::io::bad_format_string(1, 2)),
                                              thrown_by_a_handler(42));
    bool       reported     = false;
    const bool keep_running = UiException::recover(fatal, [&](const UiException::Info &) { reported = true; });
    CHECK_FALSE(keep_running);
    CHECK_FALSE(reported);
    CHECK(UiException::classify(fatal).fatal);
}

TEST_CASE("An exception message keeps no file path", "[UiException]")
{
    CHECK(UiException::short_what("cannot open /Users/sam/My Drive/part 2.stl: no such file") ==
          "cannot open <path>: no such file");
    CHECK(UiException::short_what("failed to read C:\\Users\\Sam\\part.3mf") == "failed to read <path>");
    CHECK(UiException::short_what("open '~/Downloads/a.step', then retry") == "open '<path>', then retry");
    CHECK(UiException::short_what("a 1/2 scale and 3/4 infill") == "a 1/2 scale and 3/4 infill");
}

TEST_CASE("An exception message is one short line", "[UiException]")
{
    CHECK(UiException::short_what("first line\nsecond line") == "first line second line");
    const std::string longer(300, 'x');
    const std::string cut = UiException::short_what(longer);
    CHECK(cut.size() == 120);
    CHECK(cut.substr(117) == "...");
}
