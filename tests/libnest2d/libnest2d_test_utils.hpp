#pragma once

// Shared setup for the libnest2d test suite.
//
// The no-fit-polygon numeric backend specialised below changes how NFP is
// computed for the whole program, so every translation unit that instantiates
// NFP (the geometry, nfp and placer tests) must see the same definition. Keep
// it here and include this header from every libnest2d test file.

#include <cstdint>

#include <libnest2d/libnest2d.hpp>
#include <libnest2d/utils/rotcalipers.hpp>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include "boost/multiprecision/integer.hpp"
#include "boost/rational.hpp"

namespace libnest2d {

#if !defined(_MSC_VER) && defined(__SIZEOF_INT128__) && !defined(__APPLE__)
using LargeInt = __int128;
#else
using LargeInt = boost::multiprecision::int128_t;
template<> struct _NumTag<LargeInt> { using Type = ScalarTag; };
#endif
template<class T> struct _NumTag<boost::rational<T>> { using Type = RationalTag; };

using RectangleItem = libnest2d::Rectangle;

namespace nfp {

// Use exact rational arithmetic for the convex NFP so the tests are not at the
// mercy of floating-point rounding.
template<class S>
struct NfpImpl<S, NfpLevel::CONVEX_ONLY> {
    NfpResult<S> operator()(const S &sh, const S &other) {
        return nfpConvexOnly<S, boost::rational<LargeInt>>(sh, other);
    }
};

} // namespace nfp
} // namespace libnest2d
