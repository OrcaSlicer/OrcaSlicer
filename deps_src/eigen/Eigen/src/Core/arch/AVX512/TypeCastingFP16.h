// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 The Eigen Authors.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_TYPE_CASTING_FP16_AVX512_H
#define EIGEN_TYPE_CASTING_FP16_AVX512_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {
namespace internal {

template <>
EIGEN_STRONG_INLINE Packet32s preinterpret<Packet32s, Packet32h>(const Packet32h& a) {
  return _mm512_castph_si512(a);
}
template <>
EIGEN_STRONG_INLINE Packet16s preinterpret<Packet16s, Packet16h>(const Packet16h& a) {
  return _mm256_castph_si256(a);
}
template <>
EIGEN_STRONG_INLINE Packet8s preinterpret<Packet8s, Packet8h>(const Packet8h& a) {
  return _mm_castph_si128(a);
}

template <>
EIGEN_STRONG_INLINE Packet32h preinterpret<Packet32h, Packet32s>(const Packet32s& a) {
  return _mm512_castsi512_ph(a);
}
template <>
EIGEN_STRONG_INLINE Packet16h preinterpret<Packet16h, Packet16s>(const Packet16s& a) {
  return _mm256_castsi256_ph(a);
}
template <>
EIGEN_STRONG_INLINE Packet8h preinterpret<Packet8h, Packet8s>(const Packet8s& a) {
  return _mm_castsi128_ph(a);
}

template <>
EIGEN_STRONG_INLINE Packet16f pcast<Packet16h, Packet16f>(const Packet16h& a) {
  return half2float(a);
}
template <>
EIGEN_STRONG_INLINE Packet8f pcast<Packet8h, Packet8f>(const Packet8h& a) {
  return half2float(a);
}

template <>
EIGEN_STRONG_INLINE Packet16h pcast<Packet16f, Packet16h>(const Packet16f& a) {
  return float2half(a);
}
template <>
EIGEN_STRONG_INLINE Packet8h pcast<Packet8f, Packet8h>(const Packet8f& a) {
  return float2half(a);
}

template <>
EIGEN_STRONG_INLINE Packet16f pcast<Packet32h, Packet16f>(const Packet32h& a) {
  // Discard second-half of input.
  Packet16h low = _mm256_castpd_ph(_mm512_extractf64x4_pd(_mm512_castph_pd(a), 0));
  return _mm512_cvtxph_ps(low);
}
template <>
EIGEN_STRONG_INLINE Packet8f pcast<Packet16h, Packet8f>(const Packet16h& a) {
  // Discard second-half of input.
  Packet8h low = _mm_castps_ph(_mm256_extractf32x4_ps(_mm256_castph_ps(a), 0));
  return _mm256_cvtxph_ps(low);
}
template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet8h, Packet4f>(const Packet8h& a) {
  Packet8f full = _mm256_cvtxph_ps(a);
  // Discard second-half of input.
  return _mm256_extractf32x4_ps(full, 0);
}

template <>
EIGEN_STRONG_INLINE Packet32h pcast<Packet16f, Packet32h>(const Packet16f& a, const Packet16f& b) {
  __m512 result = _mm512_castsi512_ps(_mm512_castsi256_si512(_mm256_castph_si256(_mm512_cvtxps_ph(a))));
  result = _mm512_insertf32x8(result, _mm256_castph_ps(_mm512_cvtxps_ph(b)), 1);
  return _mm512_castps_ph(result);
}
template <>
EIGEN_STRONG_INLINE Packet16h pcast<Packet8f, Packet16h>(const Packet8f& a, const Packet8f& b) {
  __m256 result = _mm256_castsi256_ps(_mm256_castsi128_si256(_mm_castph_si128(_mm256_cvtxps_ph(a))));
  result = _mm256_insertf32x4(result, _mm_castph_ps(_mm256_cvtxps_ph(b)), 1);
  return _mm256_castps_ph(result);
}
template <>
EIGEN_STRONG_INLINE Packet8h pcast<Packet4f, Packet8h>(const Packet4f& a, const Packet4f& b) {
  __m256 result = _mm256_castsi256_ps(_mm256_castsi128_si256(_mm_castps_si128(a)));
  result = _mm256_insertf128_ps(result, b, 1);
  return _mm256_cvtxps_ph(result);
}

template <>
EIGEN_STRONG_INLINE Packet32s pcast<Packet32h, Packet32s>(const Packet32h& a) {
  return _mm512_cvtph_epi16(a);
}
template <>
EIGEN_STRONG_INLINE Packet16s pcast<Packet16h, Packet16s>(const Packet16h& a) {
  return _mm256_cvtph_epi16(a);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet8h, Packet8s>(const Packet8h& a) {
  return _mm_cvtph_epi16(a);
}

template <>
EIGEN_STRONG_INLINE Packet32h pcast<Packet32s, Packet32h>(const Packet32s& a) {
  return _mm512_cvtepi16_ph(a);
}
template <>
EIGEN_STRONG_INLINE Packet16h pcast<Packet16s, Packet16h>(const Packet16s& a) {
  return _mm256_cvtepi16_ph(a);
}
template <>
EIGEN_STRONG_INLINE Packet8h pcast<Packet8s, Packet8h>(const Packet8s& a) {
  return _mm_cvtepi16_ph(a);
}

}  // namespace internal
}  // namespace Eigen

#endif  // EIGEN_TYPE_CASTING_FP16_AVX512_H
