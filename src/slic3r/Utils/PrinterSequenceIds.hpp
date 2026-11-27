#pragma once

#include <atomic>
#include <cstdint>

namespace Slic3r {

// Numbers the requests this app sends to printers. A printer echoes the number in its reply, and a
// reply numbered inside the band answers one of this app's own commands: the band of Bambu's desktop
// clients, which OPCP keeps for the Slicer. Each launch starts at a random point in the band and the
// count wraps inside it, so a printer that remembers recent numbers does not take a new launch's
// commands for repeats of the last launch's, and a long session never leaves the band.
class PrinterSequenceIds
{
public:
    static constexpr int band_begin = 20000;
    static constexpr int band_end   = 30000; // exclusive

    // The counter every printer request of this process draws from; it starts at a random point.
    static PrinterSequenceIds& shared();

    // Counts from band_begin + start modulo the band's size.
    explicit PrinterSequenceIds(std::uint32_t start);

    PrinterSequenceIds(const PrinterSequenceIds&)            = delete;
    PrinterSequenceIds& operator=(const PrinterSequenceIds&) = delete;

    // The number for the next request. Safe to call from any thread.
    int next();
    // Lets `counter++` keep reading as it did when the counter was a plain int.
    int operator++(int) { return next(); }

    static bool in_band(int id) { return id >= band_begin && id < band_end; }

private:
    std::atomic<std::uint32_t> m_count;
};

} // namespace Slic3r
