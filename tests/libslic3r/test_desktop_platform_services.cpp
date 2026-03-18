#include <catch2/catch_all.hpp>

#include "portability/platform/DesktopPlatformServices.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

TEST_CASE("DesktopPlatformServices::post_background schedules asynchronously", "[platform][desktop]")
{
    using namespace std::chrono_literals;

    Slic3r::portability::platform::DesktopPlatformServices services;

    std::promise<void> release_task;
    std::future<void>  release_future = release_task.get_future();
    std::atomic<bool>  task_started {false};
    std::atomic<bool>  task_finished {false};

    const auto before = std::chrono::steady_clock::now();
    services.post_background([&release_future, &task_started, &task_finished] {
        task_started.store(true, std::memory_order_release);
        release_future.wait();
        task_finished.store(true, std::memory_order_release);
    });
    const auto elapsed = std::chrono::steady_clock::now() - before;

    REQUIRE(elapsed < 100ms);
    REQUIRE_FALSE(task_finished.load(std::memory_order_acquire));

    const auto wait_started_deadline = std::chrono::steady_clock::now() + 1s;
    while (!task_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < wait_started_deadline)
        std::this_thread::sleep_for(1ms);

    REQUIRE(task_started.load(std::memory_order_acquire));

    release_task.set_value();

    const auto wait_finished_deadline = std::chrono::steady_clock::now() + 1s;
    while (!task_finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < wait_finished_deadline)
        std::this_thread::sleep_for(1ms);

    REQUIRE(task_finished.load(std::memory_order_acquire));
}
