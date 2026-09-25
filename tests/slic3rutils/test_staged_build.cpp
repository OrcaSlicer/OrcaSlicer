#include <catch2/catch_all.hpp>

#include <vector>

#include "slic3r/GUI/StagedBuild.hpp"

using Slic3r::GUI::StagedBuild;

namespace {

// Exposes the protected queueing calls and records the order steps ran in.
struct Staged : StagedBuild
{
    std::vector<int> ran;
    void queue(int id) { add_build_step([this, id] { ran.push_back(id); }); }
    void queue_child(Staged& child) { add_build_steps_of(child); }
    void queue_nested(int id, int nested)
    {
        add_build_step([this, id, nested] {
            ran.push_back(id);
            queue(nested);
        });
    }
};

} // namespace

TEST_CASE("Steps run in the order they were queued, one per build_step", "[StagedBuild]")
{
    Staged s;
    s.queue(1);
    s.queue(2);
    s.queue(3);
    REQUIRE_FALSE(s.built());

    REQUIRE(s.build_step());
    REQUIRE(s.ran == std::vector<int>{1});
    REQUIRE(s.build_step());
    REQUIRE(s.ran == std::vector<int>{1, 2});
    REQUIRE_FALSE(s.build_step());
    REQUIRE(s.ran == std::vector<int>{1, 2, 3});
    REQUIRE(s.built());
    REQUIRE_FALSE(s.build_step());
    REQUIRE(s.ran.size() == 3);
}

TEST_CASE("A panel with no steps is built from the start", "[StagedBuild]")
{
    Staged s;
    REQUIRE(s.built());
    REQUIRE_FALSE(s.build_step());
}

TEST_CASE("A step may queue another step, which runs after the ones already queued", "[StagedBuild]")
{
    Staged s;
    s.queue_nested(1, 3);
    s.queue(2);

    REQUIRE(s.build_step());
    REQUIRE_FALSE(s.built());
    REQUIRE(s.build_step());
    REQUIRE_FALSE(s.build_step());
    REQUIRE(s.ran == std::vector<int>{1, 2, 3});
    REQUIRE(s.built());
}

TEST_CASE("A parent waits for steps a child queues after being adopted", "[StagedBuild]")
{
    Staged child;
    child.queue_nested(1, 2); // step 1 queues step 2 while it runs
    Staged parent;
    parent.queue_child(child); // one forwarder, for step 1
    parent.queue(10);

    REQUIRE(parent.build_step()); // child step 1, which queues step 2
    REQUIRE(parent.build_step()); // 10; own steps exhausted, the child still has 2
    REQUIRE_FALSE(parent.built());
    REQUIRE_FALSE(parent.build_step()); // child step 2
    REQUIRE(child.ran == std::vector<int>{1, 2});
    REQUIRE(parent.ran == std::vector<int>{10});
    REQUIRE(parent.built());
}

TEST_CASE("A child's remaining steps are forwarded one per parent step", "[StagedBuild]")
{
    Staged child;
    child.queue(1);
    child.queue(2);
    child.queue(3);
    REQUIRE(child.build_step()); // the parent adopts only what is left

    Staged parent;
    parent.queue_child(child);
    parent.queue(10);

    REQUIRE(parent.build_step());
    REQUIRE(child.ran == std::vector<int>{1, 2});
    REQUIRE(parent.build_step());
    REQUIRE(child.ran == std::vector<int>{1, 2, 3});
    REQUIRE(child.built());
    REQUIRE_FALSE(parent.build_step());
    REQUIRE(parent.ran == std::vector<int>{10});
    REQUIRE(parent.built());
}
