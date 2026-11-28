#include <catch2/catch_test_macros.hpp>

#include "libslic3r/DropOldestQueue.hpp"

#include <chrono>
#include <thread>

using Slic3r::DropOldestQueue;
using namespace std::chrono_literals;

TEST_CASE("A full queue drops its oldest items so it never holds more than its capacity", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(3);
    for (int i = 1; i <= 10; ++i)
        queue.push(i);

    CHECK(queue.size() == 3);

    // The newest items survive, in order.
    int value = 0;
    REQUIRE(queue.pop(value, 0ms));
    CHECK(value == 8);
    REQUIRE(queue.pop(value, 0ms));
    CHECK(value == 9);
    REQUIRE(queue.pop(value, 0ms));
    CHECK(value == 10);
    CHECK_FALSE(queue.pop(value, 0ms));
}

TEST_CASE("Popping an empty queue gives up after the timeout", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(2);
    int value = 0;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(queue.pop(value, 20ms));
    CHECK(std::chrono::steady_clock::now() - start >= 20ms);
}

TEST_CASE("A waiting pop returns the item pushed from another thread", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(2);
    std::thread producer([&queue] {
        std::this_thread::sleep_for(10ms);
        queue.push(42);
    });
    int        value = 0;
    const auto start = std::chrono::steady_clock::now();
    // Generous timeout: only a missed wake-up would run into it, which the elapsed-time check catches.
    const bool got     = queue.pop(value, 5s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    producer.join();
    REQUIRE(got);
    CHECK(value == 42);
    CHECK(elapsed < 2s);
}

TEST_CASE("Lowering the capacity drops the oldest items at once", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(5);
    for (int i = 1; i <= 5; ++i)
        queue.push(i);

    queue.set_capacity(2);

    CHECK(queue.size() == 2);
    int value = 0;
    REQUIRE(queue.pop(value, 0ms));
    CHECK(value == 4);
}

TEST_CASE("A capacity of zero is treated as one", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(0);
    queue.push(1);
    queue.push(2);
    CHECK(queue.size() == 1);
}

TEST_CASE("Setting a capacity of zero keeps one item", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(3);
    queue.push(1);
    queue.push(2);
    queue.set_capacity(0);
    CHECK(queue.size() == 1);
    queue.push(3);
    CHECK(queue.size() == 1);
}

TEST_CASE("Clearing the queue empties it", "[DropOldestQueue]")
{
    DropOldestQueue<int> queue(3);
    queue.push(1);
    queue.push(2);
    queue.clear();
    CHECK(queue.size() == 0);
}
