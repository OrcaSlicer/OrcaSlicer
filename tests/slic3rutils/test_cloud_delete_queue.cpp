#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <catch2/catch_test_macros.hpp>
#include "slic3r/Utils/CloudDeleteQueue.hpp"
#include "test_utils.hpp"

using Slic3r::CloudDeleteQueue;
using namespace std::chrono_literals;

namespace {

const CloudDeleteQueue::Clock::time_point T0 = CloudDeleteQueue::Clock::time_point{} + 1000h;

// Puts the queue in user A's session with one delete of A's queued.
void start_with_a_delete(CloudDeleteQueue& queue)
{
    queue.start_session("A");
    REQUIRE(queue.add(queue.generation(), "u1", "/a/filament/PETG.info", "A"));
}

} // namespace

TEST_CASE("A delete is sent only while its account is signed in", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    REQUIRE(queue.add(queue.generation(), "u2", "/a/filament/PLA.info", "B"));

    const CloudDeleteQueue::Batch batch = queue.due(T0);
    REQUIRE(batch.entries.size() == 1);
    CHECK(batch.entries[0].setting_id == "u1");
    CHECK(queue.size() == 2);
}

TEST_CASE("Nothing is sent while signed out", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    queue.start_session("");
    REQUIRE(queue.add(queue.generation(), "u1", "/default/filament/PETG.info", ""));
    CHECK(queue.due(T0).entries.empty());
}

TEST_CASE("Switching account drops pending deletes and starts a new generation", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    const uint64_t generation = queue.generation();

    queue.start_session("B");

    CHECK(queue.size() == 0);
    CHECK(queue.generation() != generation);
    CHECK(queue.due(T0).entries.empty());
}

TEST_CASE("Starting the signed-in account's session again keeps its pending deletes", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    const uint64_t generation = queue.generation();

    queue.start_session("A");

    CHECK(queue.size() == 1);
    CHECK(queue.generation() == generation);
}

TEST_CASE("An add that started before an account switch is refused", "[CloudDeleteQueue]")
{
    // A scan for A's orphaned .info files reads the generation, then the user switches to B before it adds.
    CloudDeleteQueue queue;
    queue.start_session("A");
    const uint64_t scan_generation = queue.generation();

    queue.start_session("B");

    CHECK_FALSE(queue.add(scan_generation, "u1", "/a/filament/PETG.info", "A"));
    CHECK(queue.size() == 0);
}

TEST_CASE("A response that arrives after an account switch is ignored", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    const CloudDeleteQueue::Batch batch = queue.due(T0);

    // Switched to B and back to A while the request was in flight; A's scan queued the delete again.
    queue.start_session("B");
    queue.start_session("A");
    REQUIRE(queue.add(queue.generation(), "u1", "/a/filament/PETG.info", "A"));

    CHECK_FALSE(queue.is_current(batch.generation));
    CHECK_FALSE(queue.finish(batch.generation, "u1", true, T0).has_value());
    CHECK(queue.size() == 1);
}

TEST_CASE("A delete the cloud confirms leaves the queue and hands back its info file", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    const CloudDeleteQueue::Batch batch = queue.due(T0);

    const std::optional<std::string> info_path = queue.finish(batch.generation, "u1", true, T0);

    REQUIRE(info_path.has_value());
    CHECK(*info_path == "/a/filament/PETG.info");
    CHECK(queue.size() == 0);
}

TEST_CASE("A failed delete waits one minute, then twice as long after each failure, up to an hour", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    const uint64_t generation = queue.generation();

    auto now = T0;
    for (const auto expected_wait : {1min, 2min, 4min, 8min, 16min, 32min, 60min, 60min}) {
        CHECK_FALSE(queue.finish(generation, "u1", false, now).has_value());
        CHECK(queue.due(now + expected_wait - 1s).entries.empty());
        CHECK(queue.due(now + expected_wait).entries.size() == 1);
        now += expected_wait;
    }
    CHECK(queue.size() == 1);
}

TEST_CASE("Queueing a pending delete again keeps its wait", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    queue.finish(queue.generation(), "u1", false, T0);

    REQUIRE(queue.add(queue.generation(), "u1", "/a/filament/PETG.info", "A"));

    CHECK(queue.size() == 1);
    CHECK(queue.due(T0 + 30s).entries.empty());
}

TEST_CASE("An empty setting id is never queued", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    queue.start_session("A");
    CHECK_FALSE(queue.add(queue.generation(), "", "/a/filament/PETG.info", "A"));
    CHECK(queue.size() == 0);
}

TEST_CASE("A pass sends the signed-in account's due deletes and hands back the confirmed ones", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    REQUIRE(queue.add(queue.generation(), "u2", "/a/filament/PLA.info", "A"));
    REQUIRE(queue.add(queue.generation(), "u3", "/b/filament/ABS.info", "B"));

    std::vector<std::string> sent;
    std::vector<std::string> removed;
    queue.send_due(
        [] { return T0; }, [] { return std::string("A"); },
        [&](const CloudDeleteQueue::Entry& entry) {
            sent.push_back(entry.setting_id);
            return entry.setting_id == "u1";
        },
        [&](const CloudDeleteQueue::Entry& entry) { removed.push_back(entry.info_path); });

    CHECK(sent == std::vector<std::string>{"u1", "u2"});
    CHECK(removed == std::vector<std::string>{"/a/filament/PETG.info"});
    // u2 waits for its retry and u3 for B's session.
    CHECK(queue.size() == 2);
    CHECK(queue.due(T0).entries.empty());
}

TEST_CASE("A pass stops when the account changes during a request", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    REQUIRE(queue.add(queue.generation(), "u2", "/a/filament/PLA.info", "A"));

    std::vector<std::string> sent;
    bool                     confirmed = false;
    queue.send_due(
        [] { return T0; }, [] { return std::string("A"); },
        [&](const CloudDeleteQueue::Entry& entry) {
            sent.push_back(entry.setting_id);
            queue.start_session("B");
            return true;
        },
        [&](const CloudDeleteQueue::Entry&) { confirmed = true; });

    CHECK(sent == std::vector<std::string>{"u1"});
    CHECK_FALSE(confirmed);
}

TEST_CASE("A pass ignores an answer that came back after the token changed account", "[CloudDeleteQueue]")
{
    // The agent signed in as B before the queue started B's session.
    CloudDeleteQueue queue;
    start_with_a_delete(queue);

    std::string token_user = "A";
    bool        confirmed  = false;
    queue.send_due(
        [] { return T0; }, [&] { return token_user; },
        [&](const CloudDeleteQueue::Entry&) {
            token_user = "B";
            return true;
        },
        [&](const CloudDeleteQueue::Entry&) { confirmed = true; });

    CHECK_FALSE(confirmed);
    // Neither confirmed nor counted as a failure: the delete is still due for A.
    CHECK(queue.due(T0).entries.size() == 1);
}

TEST_CASE("A pass sends nothing while the token belongs to another account", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);

    int requests = 0;
    queue.send_due(
        [] { return T0; }, [] { return std::string("B"); },
        [&](const CloudDeleteQueue::Entry&) {
            ++requests;
            return true;
        },
        [](const CloudDeleteQueue::Entry&) {});

    CHECK(requests == 0);
    CHECK(queue.size() == 1);
}

TEST_CASE("A pass rechecks the token's account before every request", "[CloudDeleteQueue]")
{
    // The agent signs in as B between two of A's requests, before the queue starts B's session.
    CloudDeleteQueue queue;
    start_with_a_delete(queue);
    REQUIRE(queue.add(queue.generation(), "u2", "/a/filament/PLA.info", "A"));

    std::string              token_user = "A";
    std::vector<std::string> sent;
    queue.send_due(
        [] { return T0; }, [&] { return token_user; },
        [&](const CloudDeleteQueue::Entry& entry) {
            sent.push_back(entry.setting_id);
            return true;
        },
        [&](const CloudDeleteQueue::Entry&) { token_user = "B"; });

    CHECK(sent == std::vector<std::string>{"u1"});
    CHECK(queue.size() == 1);
}

TEST_CASE("A failure's wait counts from when its request finished", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    start_with_a_delete(queue);

    // The request takes ten minutes to fail.
    auto clock = T0;
    queue.send_due(
        [&] { return clock; }, [] { return std::string("A"); },
        [&](const CloudDeleteQueue::Entry&) {
            clock += 10min;
            return false;
        },
        [](const CloudDeleteQueue::Entry&) {});

    CHECK(queue.due(T0 + 10min + 59s).entries.empty());
    CHECK(queue.due(T0 + 11min).entries.size() == 1);
}

TEST_CASE("A delete without an owner belongs to the session's account", "[CloudDeleteQueue]")
{
    CloudDeleteQueue queue;
    queue.start_session("A");
    REQUIRE(queue.add(queue.generation(), "u1", "/a/filament/PETG.info", ""));

    const CloudDeleteQueue::Batch batch = queue.due(T0);
    REQUIRE(batch.entries.size() == 1);
    CHECK(batch.entries[0].owner == "A");
}

namespace {

void write_file(const boost::filesystem::path& path, const std::string& text)
{
    boost::filesystem::create_directories(path.parent_path());
    boost::filesystem::ofstream(path) << text;
}

} // namespace

TEST_CASE("A scan queues the delete each orphaned .info file records", "[CloudDeleteQueue]")
{
    ScopedTemporaryDir dir("orca-delete-scan");
    const auto         user_dir = dir.path() / "user" / "A";
    // Deleted presets: their .json is gone and the .info stays until the cloud confirms.
    write_file(user_dir / "filament" / "PETG.info", "sync_info = delete\nuser_id = A\nsetting_id = u1\n");
    write_file(user_dir / "process" / "Draft.info", "sync_info = delete\nsetting_id = u2\n");
    // A custom printer, which as a preset with no parent is kept in base/.
    write_file(user_dir / "machine" / "base" / "My Printer.info", "sync_info = delete\nuser_id = A\nsetting_id = u4\n");
    // A preset that still exists, and .info files with no setting_id or the "null" one older versions wrote.
    write_file(user_dir / "filament" / "PLA.info", "user_id = A\nsetting_id = u3\n");
    write_file(user_dir / "filament" / "PLA.json", "{}");
    write_file(user_dir / "machine" / "Voron.info", "user_id = A\n");
    write_file(user_dir / "machine" / "Ender.info", "user_id = A\nsetting_id = null\n");

    CloudDeleteQueue queue;
    queue.start_session("A");
    REQUIRE(queue_orphaned_info_files(queue, queue.generation(), user_dir.string()));

    const CloudDeleteQueue::Batch batch = queue.due(T0);
    REQUIRE(batch.entries.size() == 3);
    CHECK(batch.entries[0].setting_id == "u1");
    CHECK(batch.entries[0].info_path == (user_dir / "filament" / "PETG.info").string());
    // Without a user_id, the .info belongs to the account whose folder holds it.
    CHECK(batch.entries[1].setting_id == "u2");
    CHECK(batch.entries[1].owner == "A");
    CHECK(batch.entries[2].setting_id == "u4");
    CHECK(batch.entries[2].info_path == (user_dir / "machine" / "base" / "My Printer.info").string());
}

TEST_CASE("A scan that started before an account switch queues nothing", "[CloudDeleteQueue]")
{
    ScopedTemporaryDir dir("orca-delete-scan");
    const auto         user_dir = dir.path() / "user" / "A";
    write_file(user_dir / "filament" / "PETG.info", "user_id = A\nsetting_id = u1\n");

    CloudDeleteQueue queue;
    queue.start_session("A");
    const uint64_t scan_generation = queue.generation();
    queue.start_session("B");

    CHECK_FALSE(queue_orphaned_info_files(queue, scan_generation, user_dir.string()));
    CHECK(queue.size() == 0);
}
