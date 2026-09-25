#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "slic3r/GUI/PrebuildQueue.hpp"

using Slic3r::GUI::LazyBase;
using Slic3r::GUI::PrebuildQueue;

namespace {

// A task with `left` units, each taking `unit_ms` of the shared fake clock and logging its id.
struct Counter : LazyBase
{
    std::string      id;
    int              left;
    int              order;
    long long        unit_ms{ 1 };
    inline static std::vector<int> log;
    inline static long long        now = 0;

    Counter(int id, int left, int order, long long unit_ms = 1) : id(std::to_string(id)), left(left), order(order), unit_ms(unit_ms) {}

    const std::string& name() const override { return id; }
    bool               built() const override { return left == 0; }
    bool               build_step() override
    {
        now += unit_ms;
        log.push_back(std::stoi(id));
        return --left > 0;
    }
    int prebuild_order() const override { return order; }
};

// Resets the shared log and clock at the start of a case.
struct Reset
{
    Reset() { Counter::log.clear(); Counter::now = 0; }
};

const auto fake_clock    = [] { return Counter::now; };
const auto no_input = [] { return false; };

// Runs slices with an unlimited budget until nothing is pending; one task per slice.
void drain(PrebuildQueue& q)
{
    while (q.pending())
        q.run_slice(1000000, fake_clock, no_input);
}

} // namespace

TEST_CASE("Tasks run lowest order first, equal order in the order added", "[PrebuildQueue]")
{
    Reset   reset;
    Counter a{ 1, 1, 10 }, b{ 2, 1, 10 }, c{ 3, 1, 50 }, d{ 4, 1, 100 };
    PrebuildQueue q;
    q.add(c);
    q.add(a);
    q.add(b);
    q.add(d);
    REQUIRE(q.names() == "1, 2, 3, 4");
    drain(q);
    REQUIRE(Counter::log == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("A task with nothing pending is skipped, not removed", "[PrebuildQueue]")
{
    Reset   reset;
    Counter a{ 1, 0, 0 }, b{ 2, 2, 1 };
    PrebuildQueue q;
    q.add(a);
    q.add(b);
    REQUIRE(q.pending());
    auto slice = q.run_slice(1, fake_clock, no_input); // one unit of b
    REQUIRE(slice.units == 1);
    REQUIRE(Counter::log == std::vector<int>{2});

    a.left = 1; // a's work returned; it comes first again
    q.run_slice(1, fake_clock, no_input);
    REQUIRE(Counter::log == std::vector<int>{2, 1});
}

TEST_CASE("A slice with nothing pending runs no unit", "[PrebuildQueue]")
{
    Reset         reset;
    PrebuildQueue q;
    REQUIRE_FALSE(q.pending());
    auto slice = q.run_slice(40, fake_clock, no_input);
    REQUIRE(slice.units == 0);
    REQUIRE_FALSE(slice.completed);
    REQUIRE_FALSE(slice.remaining);
}

TEST_CASE("A slice stops once its budget is spent, after the unit that crossed it", "[PrebuildQueue]")
{
    Reset   reset;
    Counter a{ 1, 10, 0, 15 };
    PrebuildQueue q;
    q.add(a);
    auto slice = q.run_slice(40, fake_clock, no_input);
    REQUIRE(slice.units == 3); // units end at 15, 30 and 45 ms; the one crossing 40 is the last
    REQUIRE(slice.ms == 45);
    REQUIRE_FALSE(slice.completed);
    REQUIRE(slice.remaining);
    REQUIRE(a.left == 7);
}

TEST_CASE("A slice stops after the unit during which input arrived", "[PrebuildQueue]")
{
    Reset   reset;
    Counter a{ 1, 10, 0 };
    bool    input = false;
    PrebuildQueue q;
    q.add(a);
    auto slice = q.run_slice(40, fake_clock, [&] { input = a.left == 8; return input; });
    REQUIRE(slice.units == 2);
    REQUIRE_FALSE(slice.completed);
    REQUIRE(slice.remaining);
}

TEST_CASE("A slice reports completion, whether work remains, and each unit's time", "[PrebuildQueue]")
{
    Reset                  reset;
    Counter                a{ 1, 2, 0, 5 }, b{ 2, 1, 1 };
    std::vector<long long> unit_ms;
    PrebuildQueue          q;
    q.add(a);
    q.add(b);
    auto slice = q.run_slice(40, fake_clock, no_input, [&](const std::string& name, long long ms) {
        REQUIRE(name == "1");
        unit_ms.push_back(ms);
    });
    REQUIRE(slice.units == 2);
    REQUIRE(slice.completed);
    REQUIRE(slice.name == "1");
    REQUIRE(slice.remaining); // b
    REQUIRE(unit_ms == std::vector<long long>{5, 5});
    slice = q.run_slice(40, fake_clock, no_input);
    REQUIRE(slice.completed);
    REQUIRE_FALSE(slice.remaining);
    REQUIRE_FALSE(q.pending());
}

TEST_CASE("A unit may add a task to the queue it runs from", "[PrebuildQueue]")
{
    Reset         reset;
    PrebuildQueue q;
    Counter       later{ 2, 1, 5 };
    struct Adder : LazyBase
    {
        PrebuildQueue& q;
        Counter&       later;
        std::string    id{ "1" };
        bool           done{ false };
        Adder(PrebuildQueue& q, Counter& later) : q(q), later(later) {}
        const std::string& name() const override { return id; }
        bool               built() const override { return done; }
        bool               build_step() override
        {
            Counter::log.push_back(1);
            done = true;
            q.add(later);
            return false;
        }
        int prebuild_order() const override { return 0; }
    } first{ q, later };
    q.add(first);
    drain(q);
    REQUIRE(Counter::log == std::vector<int>{1, 2});
}

TEST_CASE("clear drops every task", "[PrebuildQueue]")
{
    Reset   reset;
    Counter a{ 1, 1, 0 };
    PrebuildQueue q;
    q.add(a);
    q.clear();
    REQUIRE_FALSE(q.pending());
    REQUIRE(q.run_slice(40, fake_clock, no_input).units == 0);
}
