// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 Charlie Schlosser <cs.schlosser@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_REDUCTIONS_AVX512_H
#define EIGEN_REDUCTIONS_AVX512_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet16i -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE int predux(const Packet16i& a) {
  return _mm512_reduce_add_epi32(a);
}

template <>
EIGEN_STRONG_INLINE int predux_mul(const Packet16i& a) {
  return _mm512_reduce_mul_epi32(a);
}

template <>
EIGEN_STRONG_INLINE int predux_min(const Packet16i& a) {
  return _mm512_reduce_min_epi32(a);
}

template <>
EIGEN_STRONG_INLINE int predux_max(const Packet16i& a) {
  return _mm512_reduce_max_epi32(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet16i& a) {
  return _mm512_reduce_or_epi32(a) != 0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8l -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE int64_t predux(const Packet8l& a) {
  return _mm512_reduce_add_epi64(a);
}

#if EIGEN_COMP_MSVC
// MSVC's _mm512_reduce_mul_epi64 is borked, at least up to and including 1939.
//    alignas(64) int64_t data[] = { 1,1,-1,-1,1,-1,-1,-1 };
//    int64_t out = _mm512_reduce_mul_epi64(_mm512_load_epi64(data));
// produces garbage: 4294967295.  It seems to happen whenever the output is supposed to be negative.
// Fall back to a manual approach:
template <>
EIGEN_STRONG_INLINE int64_t predux_mul(const Packet8l& a) {
  Packet4l lane0 = _mm512_extracti64x4_epi64(a, 0);
  Packet4l lane1 = _mm512_extracti64x4_epi64(a, 1);
  return predux_mul(pmul(lane0, lane1));
}
#else
template <>
EIGEN_STRONG_INLINE int64_t predux_mul<Packet8l>(const Packet8l& a) {
  return _mm512_reduce_mul_epi64(a);
}
#endif

template <>
EIGEN_STRONG_INLINE int64_t predux_min(const Packet8l& a) {
  return _mm512_reduce_min_epi64(a);
}

template <>
EIGEN_STRONG_INLINE int64_t predux_max(const Packet8l& a) {
  return _mm512_reduce_max_epi64(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8l& a) {
  return _mm512_reduce_or_epi64(a) != 0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet16f -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE float predux(const Packet16f& a) {
  return _mm512_reduce_add_ps(a);
}

template <>
EIGEN_STRONG_INLINE float predux_mul(const Packet16f& a) {
  return _mm512_reduce_mul_ps(a);
}

template <>
EIGEN_STRONG_INLINE float predux_min(const Packet16f& a) {
  return _mm512_reduce_min_ps(a);
}

template <>
EIGEN_STRONG_INLINE float predux_min<PropagateNumbers>(const Packet16f& a) {
  Packet8f lane0 = _mm512_extractf32x8_ps(a, 0);
  Packet8f lane1 = _mm512_extractf32x8_ps(a, 1);
  return predux_min<PropagateNumbers>(pmin<PropagateNumbers>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE float predux_min<PropagateNaN>(const Packet16f& a) {
  Packet8f lane0 = _mm512_extractf32x8_ps(a, 0);
  Packet8f lane1 = _mm512_extractf32x8_ps(a, 1);
  return predux_min<PropagateNaN>(pmin<PropagateNaN>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE float predux_max(const Packet16f& a) {
  return _mm512_reduce_max_ps(a);
}

template <>
EIGEN_STRONG_INLINE float predux_max<PropagateNumbers>(const Packet16f& a) {
  Packet8f lane0 = _mm512_extractf32x8_ps(a, 0);
  Packet8f lane1 = _mm512_extractf32x8_ps(a, 1);
  return predux_max<PropagateNumbers>(pmax<PropagateNumbers>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE float predux_max<PropagateNaN>(const Packet16f& a) {
  Packet8f lane0 = _mm512_extractf32x8_ps(a, 0);
  Packet8f lane1 = _mm512_extractf32x8_ps(a, 1);
  return predux_max<PropagateNaN>(pmax<PropagateNaN>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet16f& a) {
  return _mm512_reduce_or_epi32(_mm512_castps_si512(a)) != 0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8d -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE double predux(const Packet8d& a) {
  return _mm512_reduce_add_pd(a);
}

template <>
EIGEN_STRONG_INLINE double predux_mul(const Packet8d& a) {
  return _mm512_reduce_mul_pd(a);
}

template <>
EIGEN_STRONG_INLINE double predux_min(const Packet8d& a) {
  return _mm512_reduce_min_pd(a);
}

template <>
EIGEN_STRONG_INLINE double predux_min<PropagateNumbers>(const Packet8d& a) {
  Packet4d lane0 = _mm512_extractf64x4_pd(a, 0);
  Packet4d lane1 = _mm512_extractf64x4_pd(a, 1);
  return predux_min<PropagateNumbers>(pmin<PropagateNumbers>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE double predux_min<PropagateNaN>(const Packet8d& a) {
  Packet4d lane0 = _mm512_extractf64x4_pd(a, 0);
  Packet4d lane1 = _mm512_extractf64x4_pd(a, 1);
  return predux_min<PropagateNaN>(pmin<PropagateNaN>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE double predux_max(const Packet8d& a) {
  return _mm512_reduce_max_pd(a);
}

template <>
EIGEN_STRONG_INLINE double predux_max<PropagateNumbers>(const Packet8d& a) {
  Packet4d lane0 = _mm512_extractf64x4_pd(a, 0);
  Packet4d lane1 = _mm512_extractf64x4_pd(a, 1);
  return predux_max<PropagateNumbers>(pmax<PropagateNumbers>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE double predux_max<PropagateNaN>(const Packet8d& a) {
  Packet4d lane0 = _mm512_extractf64x4_pd(a, 0);
  Packet4d lane1 = _mm512_extractf64x4_pd(a, 1);
  return predux_max<PropagateNaN>(pmax<PropagateNaN>(lane0, lane1));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8d& a) {
  return _mm512_reduce_or_epi64(_mm512_castpd_si512(a)) != 0;
}

#ifndef EIGEN_VECTORIZE_AVX512FP16
/* -- -- -- -- -- -- -- -- -- -- -- -- Packet16h -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE half predux(const Packet16h& from) {
  return half(predux(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_mul(const Packet16h& from) {
  return half(predux_mul(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_min(const Packet16h& from) {
  return half(predux_min(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_min<PropagateNumbers>(const Packet16h& from) {
  return half(predux_min<PropagateNumbers>(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_min<PropagateNaN>(const Packet16h& from) {
  return half(predux_min<PropagateNaN>(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_max(const Packet16h& from) {
  return half(predux_max(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_max<PropagateNumbers>(const Packet16h& from) {
  return half(predux_max<PropagateNumbers>(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE half predux_max<PropagateNaN>(const Packet16h& from) {
  return half(predux_max<PropagateNaN>(half2float(from)));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet16h& a) {
  return predux_any<Packet8i>(a.m_val);
}
#endif

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet16bf -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE bfloat16 predux(const Packet16bf& from) {
  return static_cast<bfloat16>(predux<Packet16f>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_mul(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_mul<Packet16f>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_min(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_min<Packet16f>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_min<PropagateNumbers>(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_min<PropagateNumbers>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_min<PropagateNaN>(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_min<PropagateNaN>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_max(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_max(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_max<PropagateNumbers>(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_max<PropagateNumbers>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_max<PropagateNaN>(const Packet16bf& from) {
  return static_cast<bfloat16>(predux_max<PropagateNaN>(Bf16ToF32(from)));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet16bf& a) {
  return predux_any<Packet8i>(a.m_val);
}

}  // end namespace internal
}  // end namespace Eigen

#endif  // EIGEN_REDUCTIONS_AVX512_H
