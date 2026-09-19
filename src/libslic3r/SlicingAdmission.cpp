#include "SlicingAdmission.hpp"

#include <atomic>
#include <utility>

namespace Slic3r {
namespace {

constexpr std::uint8_t live_slicing_mask     = 1u << 0;
constexpr std::uint8_t isolated_slicing_mask = 1u << 1;
constexpr std::uint8_t scale_changing_mask   = 1u << 2;

std::atomic<std::uint8_t> slicing_admission_state {0};

constexpr std::uint8_t mode_mask(SlicingAdmissionMode mode) noexcept
{
    switch (mode) {
    case SlicingAdmissionMode::LiveSlicing:     return live_slicing_mask;
    case SlicingAdmissionMode::IsolatedSlicing: return isolated_slicing_mask;
    case SlicingAdmissionMode::ScaleChanging:   return scale_changing_mask;
    }
    return 0;
}

constexpr std::uint8_t conflict_mask(SlicingAdmissionMode mode) noexcept
{
    switch (mode) {
    case SlicingAdmissionMode::LiveSlicing:
        return live_slicing_mask | isolated_slicing_mask;
    case SlicingAdmissionMode::IsolatedSlicing:
        return live_slicing_mask | isolated_slicing_mask | scale_changing_mask;
    case SlicingAdmissionMode::ScaleChanging:
        // Live slicing already supports ordinary GUI bed updates. Only an
        // isolated job freezes process-global scale data.
        return isolated_slicing_mask | scale_changing_mask;
    }
    return 0xff;
}

} // namespace

SlicingAdmissionToken::~SlicingAdmissionToken()
{
    reset();
}

SlicingAdmissionToken::SlicingAdmissionToken(SlicingAdmissionToken &&other) noexcept
    : m_mask(std::exchange(other.m_mask, 0))
{
}

SlicingAdmissionToken &SlicingAdmissionToken::operator=(SlicingAdmissionToken &&other) noexcept
{
    if (this != &other) {
        reset();
        m_mask = std::exchange(other.m_mask, 0);
    }
    return *this;
}

void SlicingAdmissionToken::reset() noexcept
{
    if (m_mask != 0) {
        slicing_admission_state.fetch_and(static_cast<std::uint8_t>(~m_mask), std::memory_order_release);
        m_mask = 0;
    }
}

SlicingAdmissionToken try_acquire_slicing_admission(SlicingAdmissionMode mode) noexcept
{
    const std::uint8_t requested = mode_mask(mode);
    if (requested == 0)
        return {};

    std::uint8_t observed = slicing_admission_state.load(std::memory_order_acquire);
    for (;;) {
        if ((observed & conflict_mask(mode)) != 0)
            return {};

        const std::uint8_t desired = observed | requested;
        if (slicing_admission_state.compare_exchange_weak(
                observed, desired, std::memory_order_acq_rel, std::memory_order_acquire))
            return SlicingAdmissionToken(requested);
    }
}

} // namespace Slic3r
