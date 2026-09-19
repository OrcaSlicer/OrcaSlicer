#ifndef slic3r_SlicingAdmission_hpp_
#define slic3r_SlicingAdmission_hpp_

#include <cstdint>

namespace Slic3r {

// Internal process-wide admission for operations that use slicing-global state.
enum class SlicingAdmissionMode : std::uint8_t {
    LiveSlicing,
    IsolatedSlicing,
    ScaleChanging,
};

class SlicingAdmissionToken
{
public:
    SlicingAdmissionToken() noexcept = default;
    ~SlicingAdmissionToken();

    SlicingAdmissionToken(const SlicingAdmissionToken &)            = delete;
    SlicingAdmissionToken &operator=(const SlicingAdmissionToken &) = delete;

    SlicingAdmissionToken(SlicingAdmissionToken &&other) noexcept;
    SlicingAdmissionToken &operator=(SlicingAdmissionToken &&other) noexcept;

    explicit operator bool() const noexcept { return m_mask != 0; }
    void reset() noexcept;

private:
    friend SlicingAdmissionToken try_acquire_slicing_admission(SlicingAdmissionMode mode) noexcept;

    explicit SlicingAdmissionToken(std::uint8_t mask) noexcept : m_mask(mask) {}

    std::uint8_t m_mask {0};
};

// Never waits. An empty token reports that the requested mode conflicts with
// an operation which is already admitted.
[[nodiscard]] SlicingAdmissionToken try_acquire_slicing_admission(SlicingAdmissionMode mode) noexcept;

} // namespace Slic3r

#endif // slic3r_SlicingAdmission_hpp_
