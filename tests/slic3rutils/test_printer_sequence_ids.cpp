#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/Utils/PrinterSequenceIds.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

using namespace Slic3r;

TEST_CASE("Request numbers count up by one from where they start", "[PrinterSequenceIds]")
{
    PrinterSequenceIds ids(0);
    CHECK(ids.next() == 20000);
    CHECK(ids.next() == 20001);
    CHECK(ids++ == 20002);
}

TEST_CASE("Request numbers wrap inside the band", "[PrinterSequenceIds]")
{
    // 9998 is the band's second-to-last offset; a start past the band's size folds into the band.
    const std::uint32_t start = GENERATE(9998u, 19998u);
    PrinterSequenceIds  ids(start);
    CHECK(ids.next() == 29998);
    CHECK(ids.next() == 29999);
    CHECK(ids.next() == 20000);
}

TEST_CASE("Numbers drawn from several threads at once are all different and in the band", "[PrinterSequenceIds]")
{
    constexpr int threads    = 4;
    constexpr int per_thread = 2000; // 8000 numbers, fewer than the band's 10000
    PrinterSequenceIds ids(1234);

    std::vector<std::vector<int>> drawn(threads);
    std::vector<std::thread>      workers;
    for (int t = 0; t < threads; ++t)
        workers.emplace_back([&ids, &drawn, t] {
            for (int i = 0; i < per_thread; ++i)
                drawn[t].push_back(ids.next());
        });
    for (std::thread& worker : workers)
        worker.join();

    std::set<int> unique;
    for (const std::vector<int>& numbers : drawn)
        unique.insert(numbers.begin(), numbers.end());
    CHECK(unique.size() == static_cast<std::size_t>(threads * per_thread));
    CHECK(PrinterSequenceIds::in_band(*unique.begin()));
    CHECK(PrinterSequenceIds::in_band(*unique.rbegin()));
}

TEST_CASE("The device layer numbers its commands from the shared counter", "[PrinterSequenceIds]")
{
    CHECK(&MachineObject::m_sequence_id == &PrinterSequenceIds::shared());
    CHECK(PrinterSequenceIds::in_band(MachineObject::m_sequence_id++));
}
