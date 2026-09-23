#include <catch2/catch_all.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include "slic3r/GUI/Lazy.hpp"

using Slic3r::GUI::Lazy;
using Slic3r::GUI::LazyBase;
using Slic3r::GUI::LazyInstance;
using Slic3r::GUI::StagedBuild;

namespace {

struct Plain
{
    int value{ 1 };
};

struct One : LazyInstance<One>
{
    int value{ 2 };
};

// Two steps after the constructor.
struct Staged : StagedBuild, LazyInstance<Staged>
{
    std::vector<int> ran;
    Staged()
    {
        add_build_step([this] { ran.push_back(1); });
        add_build_step([this] { ran.push_back(2); });
    }
    void add_step(std::function<void()> step) { add_build_step(std::move(step)); }
};

// Owns what the factories make, since a Lazy does not.
template <class T>
struct Made
{
    std::vector<std::unique_ptr<T>> objects;
    T*                              make()
    {
        objects.push_back(std::make_unique<T>());
        return objects.back().get();
    }
    typename Lazy<T>::Factory factory()
    {
        return [this] { return make(); };
    }
};

} // namespace

TEST_CASE("The factory runs on the first unit, not at construction", "[Lazy]")
{
    Made<Plain> made;
    Lazy<Plain> lazy("plain", 0, made.factory());
    REQUIRE(made.objects.empty());
    REQUIRE_FALSE(lazy.built());
    REQUIRE(lazy.pending());
    REQUIRE(lazy.get() == nullptr);

    REQUIRE_FALSE(lazy.build_step()); // the only unit
    REQUIRE(made.objects.size() == 1);
    REQUIRE(lazy.built());
    REQUIRE_FALSE(lazy.pending());
    REQUIRE(lazy.get() == made.objects[0].get());
    REQUIRE_FALSE(lazy.build_step());
    REQUIRE(made.objects.size() == 1);
}

TEST_CASE("A staged type takes one unit for the constructor and one per step", "[Lazy]")
{
    Made<Staged> made;
    Lazy<Staged> lazy("staged", 0, made.factory());
    REQUIRE(lazy.build_step());
    REQUIRE(made.objects.size() == 1);
    REQUIRE(lazy.get() == nullptr); // exists but incomplete
    REQUIRE(lazy.build_step());
    REQUIRE(made.objects[0]->ran == std::vector<int>{1});
    REQUIRE_FALSE(lazy.build_step());
    REQUIRE(made.objects[0]->ran == std::vector<int>{1, 2});
    REQUIRE(lazy.get() == made.objects[0].get());
}

TEST_CASE("ensure builds whatever is left and is a no-op afterwards", "[Lazy]")
{
    Made<Staged> made;
    Lazy<Staged> lazy("staged", 0, made.factory());
    lazy.build_step();
    Staged* s = lazy.ensure();
    REQUIRE(s == made.objects[0].get());
    REQUIRE(s->ran == std::vector<int>{1, 2});
    REQUIRE(lazy.ensure() == s);
    REQUIRE(made.objects.size() == 1);
}

TEST_CASE("when_built waits for completion, then runs at once", "[Lazy]")
{
    Made<Staged> made;
    Lazy<Staged> lazy("staged", 0, made.factory());
    std::vector<int> seen;
    lazy.when_built([&](Staged& s) { seen.push_back(int(s.ran.size())); });
    lazy.build_step();
    lazy.build_step();
    REQUIRE(seen.empty());
    lazy.build_step();
    REQUIRE(seen == std::vector<int>{2});
    lazy.when_built([&](Staged&) { seen.push_back(9); });
    REQUIRE(seen == std::vector<int>{2, 9});
}

TEST_CASE("A LazyInstance type reaches its holder through the statics", "[Lazy]")
{
    REQUIRE(One::if_built() == nullptr);
    REQUIRE(One::ensure() == nullptr);
    Made<One> made;
    {
        Lazy<One> lazy("one", 0, made.factory());
        REQUIRE(One::if_built() == nullptr);
        One* one = One::ensure();
        REQUIRE(one == made.objects[0].get());
        REQUIRE(One::if_built() == one);
        int seen = 0;
        One::when_built([&](One& o) { seen = o.value; });
        REQUIRE(seen == 2);
    }
    REQUIRE(One::if_built() == nullptr);
}

TEST_CASE("A newer holder replaces the registration; the older one leaves it alone", "[Lazy]")
{
    Made<One> made;
    auto      first = std::make_unique<Lazy<One>>("first", 0, made.factory());
    first->ensure();
    Lazy<One> second("second", 0, made.factory());
    REQUIRE(One::if_built() == nullptr); // the new holder has not built yet
    second.ensure();
    REQUIRE(One::if_built() == made.objects[1].get());
    first.reset();
    REQUIRE(One::if_built() == made.objects[1].get());
}

TEST_CASE("The holder reports the name and order it was given", "[Lazy]")
{
    Made<Plain> made;
    Lazy<Plain> lazy("plain", 7, made.factory());
    LazyBase&   base = lazy;
    REQUIRE(base.name() == "plain");
    REQUIRE(base.prebuild_order() == 7);
}

TEST_CASE("A unit that re-enters the holder builds nothing twice", "[Lazy]")
{
    Made<Plain>  made;
    Lazy<Plain>* self         = nullptr;
    int          nested_units = 0;
    Lazy<Plain>  lazy("plain", 0, [&] {
        if (self->build_step()) // as if the constructor pumped the event loop into a slice
            ++nested_units;
        return made.make();
    });
    self = &lazy;
    REQUIRE_FALSE(lazy.build_step());
    REQUIRE(nested_units == 0);
    REQUIRE(made.objects.size() == 1);
    REQUIRE(lazy.built());
}

TEST_CASE("A factory that returns null leaves the holder unbuilt and not pending", "[Lazy]")
{
    int         calls = 0;
    Lazy<Plain> lazy("plain", 0, [&] { ++calls; return static_cast<Plain*>(nullptr); });
    REQUIRE_FALSE(lazy.build_step());
    REQUIRE_FALSE(lazy.built());
    REQUIRE_FALSE(lazy.pending());
    REQUIRE(lazy.get() == nullptr);
    REQUIRE_FALSE(lazy.build_step()); // not retried
    REQUIRE(calls == 1);
}

TEST_CASE("ensure returns null for a factory that returned null", "[Lazy]")
{
    Lazy<Plain> lazy("plain", 0, [] { return static_cast<Plain*>(nullptr); });
    REQUIRE(lazy.ensure() == nullptr);
    REQUIRE_FALSE(lazy.built());
}

TEST_CASE("A nested ensure inside the factory returns null", "[Lazy]")
{
    Made<Plain>  made;
    Lazy<Plain>* self   = nullptr;
    Plain*       nested = reinterpret_cast<Plain*>(1);
    Lazy<Plain>  lazy("plain", 0, [&] {
        nested = self->ensure(); // as if the constructor pumped the event loop into a caller
        return made.make();
    });
    self = &lazy;
    Plain* built = lazy.ensure();
    REQUIRE(built == made.objects[0].get());
    REQUIRE(nested == nullptr);
}

TEST_CASE("A nested ensure during a staged step returns null", "[Lazy]")
{
    Made<Staged>  made;
    Lazy<Staged>* self   = nullptr;
    Staged*       nested = reinterpret_cast<Staged*>(1);
    Lazy<Staged>  lazy("staged", 0, [&] {
        Staged* s = made.make();
        s->add_step([&] { nested = self->ensure(); }); // as if a step pumped the event loop into a caller
        return s;
    });
    self = &lazy;
    Staged* built = lazy.ensure();
    REQUIRE(built == made.objects[0].get());
    REQUIRE(nested == nullptr);
}

TEST_CASE("A unit that throws leaves the holder free to build the rest", "[Lazy]")
{
    Made<Staged> made;
    bool         thrown = false;
    Lazy<Staged> lazy("staged", 0, [&] {
        Staged* s = made.make();
        s->add_step([&] { thrown = true; throw std::runtime_error("step"); });
        return s;
    });
    lazy.build_step();
    lazy.build_step();
    lazy.build_step();
    REQUIRE_THROWS(lazy.build_step());
    REQUIRE(thrown);
    REQUIRE(lazy.pending());
    REQUIRE_FALSE(lazy.build_step()); // the next unit runs
    REQUIRE(lazy.built());
}
