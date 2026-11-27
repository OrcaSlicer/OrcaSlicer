#include "PrinterSequenceIds.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <random>

namespace Slic3r {

namespace {

constexpr std::uint32_t band_size = PrinterSequenceIds::band_end - PrinterSequenceIds::band_begin;

std::uint32_t random_start()
{
    try {
        return std::random_device{}();
    } catch (const std::exception&) {
        // No entropy source: the clock still differs from one launch to the next.
        return static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
}

} // namespace

PrinterSequenceIds& PrinterSequenceIds::shared()
{
    static PrinterSequenceIds ids(random_start());
    return ids;
}

PrinterSequenceIds::PrinterSequenceIds(std::uint32_t start) : m_count(start % band_size) {}

int PrinterSequenceIds::next()
{
    const std::uint32_t n = m_count.fetch_add(1, std::memory_order_relaxed);
    return band_begin + static_cast<int>(n % band_size);
}

} // namespace Slic3r
