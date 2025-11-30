// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 The Eigen Authors.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_MATH_FUNCTIONS_FP16_AVX512_H
#define EIGEN_MATH_FUNCTIONS_FP16_AVX512_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {
namespace internal {

EIGEN_STRONG_INLINE Packet32h combine2Packet16h(const Packet16h& a, const Packet16h& b) {
  __m512i result = _mm512_castsi256_si512(_mm256_castph_si256(a));
  result = _mm512_inserti64x4(result, _mm256_castph_si256(b), 1);
  return _mm512_castsi512_ph(result);
}

EIGEN_STRONG_INLINE void extract2Packet16h(const Packet32h& x, Packet16h& a, Packet16h& b) {
  a = _mm256_castsi256_ph(_mm512_castsi512_si256(_mm512_castph_si512(x)));
  b = _mm256_castsi256_ph(_mm512_extracti64x4_epi64(_mm512_castph_si512(x), 1));
}

#define _EIGEN_GENERATE_FP16_MATH_FUNCTION(func)                      \
  template <>                                                         \
  EIGEN_STRONG_INLINE Packet8h func<Packet8h>(const Packet8h& a) {    \
    return float2half(func(half2float(a)));                           \
  }                                                                   \
                                                                      \
  template <>                                                         \
  EIGEN_STRONG_INLINE Packet16h func<Packet16h>(const Packet16h& a) { \
    return float2half(func(half2float(a)));                           \
  }                                                                   \
                                                                      \
  template <>                                                         \
  EIGEN_STRONG_INLINE Packet32h func<Packet32h>(const Packet32h& a) { \
    Packet16h low;                                                    \
    Packet16h high;                                                   \
    extract2Packet16h(a, low, high);                                  \
    return combine2Packet16h(func(low), func(high));                  \
  }

_EIGEN_GENERATE_FP16_MATH_FUNCTION(psin)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(pcos)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(plog)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(plog2)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(plog1p)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(pexp)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(pexpm1)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(pexp2)
_EIGEN_GENERATE_FP16_MATH_FUNCTION(ptanh)
#undef _EIGEN_GENERATE_FP16_MATH_FUNCTION

// pfrexp
template <>
EIGEN_STRONG_INLINE Packet32h pfrexp<Packet32h>(const Packet32h& a, Packet32h& exponent) {
  return pfrexp_generic(a, exponent);
}

// pldexp
template <>
EIGEN_STRONG_INLINE Packet32h pldexp<Packet32h>(const Packet32h& a, const Packet32h& exponent) {
  return pldexp_generic(a, exponent);
}

}  // end namespace internal
}  // end namespace Eigen

#endif  // EIGEN_MATH_FUNCTIONS_FP16_AVX512_H