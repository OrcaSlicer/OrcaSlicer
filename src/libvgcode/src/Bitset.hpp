///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_BITSET_HPP
#define VGCODE_BITSET_HPP

#include <atomic>
#include <vector>

namespace libvgcode {

// By default, types are not atomic,
template<typename T> auto constexpr is_atomic = false;

// but std::atomic<T> types are,
template<typename T> auto constexpr is_atomic<std::atomic<T>> = true;

template<typename T = unsigned long long>
struct BitSet
{
    BitSet() = default;
    BitSet(std::size_t size) : size(size), blocks(1 + (size / (sizeof(T) * 8))) { clear(); }

    void clear() {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            blocks[i] &= T(0);
        }
    }

    void setAll() {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            blocks[i] |= ~T(0);
        }
    }

    //return true if bit changed
    bool set(std::size_t index) {
        const auto [block_idx, bit_idx] = get_coords(index);
        const T mask = (T(1) << bit_idx);
        bool flip = mask ^ blocks[block_idx];
        blocks[block_idx] |= mask;
        return flip;
    }

    //return true if bit changed
    bool reset(std::size_t index) {
        const auto [block_idx, bit_idx] = get_coords(index);
        const T mask = (T(1) << bit_idx);
        const bool flip = mask ^ blocks[block_idx];
        blocks[block_idx] &= (~mask);
        return flip;
    }

    bool operator [] (std::size_t index) const {
        const auto [block_idx, bit_idx] = get_coords(index);
        return ((blocks[block_idx] >> bit_idx) & 1) != 0;
    }

    template<typename U>
    BitSet& operator &= (const BitSet<U>& other) {
        static_assert(sizeof(T) == sizeof(U), "Type1 and Type2 must be of the same size.");
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            blocks[i] &= other.blocks[i];
        }
        return *this;
    }

    // Atomic set operation (enabled only for atomic types), return true if bit changed
    template<typename U = T>
    inline typename std::enable_if<is_atomic<U>, bool>::type set_atomic(std::size_t index) {
        const auto [block_idx, bit_idx] = get_coords(index);
        const T mask = static_cast<T>(1) << bit_idx;
        const T oldval = blocks[block_idx].fetch_or(mask, std::memory_order_relaxed);
        return oldval ^ (oldval or mask);
    }

    // Atomic reset operation (enabled only for atomic types), return true if bit changed
    template<typename U = T>
    inline typename std::enable_if<is_atomic<U>, bool>::type reset_atomic(std::size_t index) {
        const auto [block_idx, bit_idx] = get_coords(index);
        const T mask = ~(static_cast<T>(1) << bit_idx);
        const T oldval = blocks[block_idx].fetch_and(mask, std::memory_order_relaxed);
        return oldval ^ (oldval and mask);
    }

    std::pair<std::size_t, std::size_t> get_coords(std::size_t index) const {
        const std::size_t block_idx = index / (sizeof(T) * 8);
        const std::size_t bit_idx = index % (sizeof(T) * 8);
        return { block_idx, bit_idx };
    }

    std::size_t size_in_bytes_cpu() const {
        return blocks.size() * sizeof(T);
    }

    std::size_t size{ 0 };
    std::vector<T> blocks{ 0 };
};

} // namespace libvgcode

#endif // VGCODE_BITSET_HPP
