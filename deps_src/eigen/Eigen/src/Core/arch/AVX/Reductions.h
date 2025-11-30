// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 Charlie Schlosser <cs.schlosser@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_REDUCTIONS_AVX_H
#define EIGEN_REDUCTIONS_AVX_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8i -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE int predux(const Packet8i& a) {
  Packet4i lo = _mm256_castsi256_si128(a);
  Packet4i hi = _mm256_extractf128_si256(a, 1);
  return predux(padd(lo, hi));
}

template <>
EIGEN_STRONG_INLINE int predux_mul(const Packet8i& a) {
  Packet4i lo = _mm256_castsi256_si128(a);
  Packet4i hi = _mm256_extractf128_si256(a, 1);
  return predux_mul(pmul(lo, hi));
}

template <>
EIGEN_STRONG_INLINE int predux_min(const Packet8i& a) {
  Packet4i lo = _mm256_castsi256_si128(a);
  Packet4i hi = _mm256_extractf128_si256(a, 1);
  return predux_min(pmin(lo, hi));
}

template <>
EIGEN_STRONG_INLINE int predux_max(const Packet8i& a) {
  Packet4i lo = _mm256_castsi256_si128(a);
  Packet4i hi = _mm256_extractf128_si256(a, 1);
  return predux_max(pmax(lo, hi));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8i& a) {
#ifdef EIGEN_VECTORIZE_AVX2
  return _mm256_movemask_epi8(a) != 0x0;
#else
  return _mm256_movemask_ps(_mm256_castsi256_ps(a)) != 0x0;
#endif
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8ui -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE uint32_t predux(const Packet8ui& a) {
  Packet4ui lo = _mm256_castsi256_si128(a);
  Packet4ui hi = _mm256_extractf128_si256(a, 1);
  return predux(padd(lo, hi));
}

template <>
EIGEN_STRONG_INLINE uint32_t predux_mul(const Packet8ui& a) {
  Packet4ui lo = _mm256_castsi256_si128(a);
  Packet4ui hi = _mm256_extractf128_si256(a, 1);
  return predux_mul(pmul(lo, hi));
}

template <>
EIGEN_STRONG_INLINE uint32_t predux_min(const Packet8ui& a) {
  Packet4ui lo = _mm256_castsi256_si128(a);
  Packet4ui hi = _mm256_extractf128_si256(a, 1);
  return predux_min(pmin(lo, hi));
}

template <>
EIGEN_STRONG_INLINE uint32_t predux_max(const Packet8ui& a) {
  Packet4ui lo = _mm256_castsi256_si128(a);
  Packet4ui hi = _mm256_extractf128_si256(a, 1);
  return predux_max(pmax(lo, hi));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8ui& a) {
#ifdef EIGEN_VECTORIZE_AVX2
  return _mm256_movemask_epi8(a) != 0x0;
#else
  return _mm256_movemask_ps(_mm256_castsi256_ps(a)) != 0x0;
#endif
}

#ifdef EIGEN_VECTORIZE_AVX2

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet4l -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE int64_t predux(const Packet4l& a) {
  Packet2l lo = _mm256_castsi256_si128(a);
  Packet2l hi = _mm256_extractf128_si256(a, 1);
  return predux(padd(lo, hi));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet4l& a) {
  return _mm256_movemask_pd(_mm256_castsi256_pd(a)) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet4ul -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE uint64_t predux(const Packet4ul& a) {
  return static_cast<uint64_t>(predux(Packet4l(a)));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet4ul& a) {
  return _mm256_movemask_pd(_mm256_castsi256_pd(a)) != 0x0;
}

#endif

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8f -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE float predux(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux(padd(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_mul(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_mul(pmul(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_min(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_min(pmin(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_min<PropagateNumbers>(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_min<PropagateNumbers>(pmin<PropagateNumbers>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_min<PropagateNaN>(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_min<PropagateNaN>(pmin<PropagateNaN>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_max(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_max(pmax(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_max<PropagateNumbers>(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_max<PropagateNumbers>(pmax<PropagateNumbers>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE float predux_max<PropagateNaN>(const Packet8f& a) {
  Packet4f lo = _mm256_castps256_ps128(a);
  Packet4f hi = _mm256_extractf128_ps(a, 1);
  return predux_max<PropagateNaN>(pmax<PropagateNaN>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8f& a) {
  return _mm256_movemask_ps(a) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet4d -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE double predux(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux(padd(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_mul(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_mul(pmul(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_min(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_min(pmin(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_min<PropagateNumbers>(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_min<PropagateNumbers>(pmin<PropagateNumbers>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_min<PropagateNaN>(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_min<PropagateNaN>(pmin<PropagateNaN>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_max(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_max(pmax(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_max<PropagateNumbers>(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_max<PropagateNumbers>(pmax<PropagateNumbers>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE double predux_max<PropagateNaN>(const Packet4d& a) {
  Packet2d lo = _mm256_castpd256_pd128(a);
  Packet2d hi = _mm256_extractf128_pd(a, 1);
  return predux_max<PropagateNaN>(pmax<PropagateNaN>(lo, hi));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet4d& a) {
  return _mm256_movemask_pd(a) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8h -- -- -- -- -- -- -- -- -- -- -- -- */
#ifndef EIGEN_VECTORIZE_AVX512FP16

template <>
EIGEN_STRONG_INLINE half predux(const Packet8h& a) {
  return static_cast<half>(predux(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_mul(const Packet8h& a) {
  return static_cast<half>(predux_mul(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_min(const Packet8h& a) {
  return static_cast<half>(predux_min(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_min<PropagateNumbers>(const Packet8h& a) {
  return static_cast<half>(predux_min<PropagateNumbers>(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_min<PropagateNaN>(const Packet8h& a) {
  return static_cast<half>(predux_min<PropagateNaN>(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_max(const Packet8h& a) {
  return static_cast<half>(predux_max(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_max<PropagateNumbers>(const Packet8h& a) {
  return static_cast<half>(predux_max<PropagateNumbers>(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE half predux_max<PropagateNaN>(const Packet8h& a) {
  return static_cast<half>(predux_max<PropagateNaN>(half2float(a)));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8h& a) {
  return _mm_movemask_epi8(a) != 0;
}
#endif  // EIGEN_VECTORIZE_AVX512FP16

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet8bf -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE bfloat16 predux(const Packet8bf& a) {
  return static_cast<bfloat16>(predux<Packet8f>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_mul(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_mul<Packet8f>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_min(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_min(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_min<PropagateNumbers>(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_min<PropagateNumbers>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_min<PropagateNaN>(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_min<PropagateNaN>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_max(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_max<Packet8f>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_max<PropagateNumbers>(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_max<PropagateNumbers>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bfloat16 predux_max<PropagateNaN>(const Packet8bf& a) {
  return static_cast<bfloat16>(predux_max<PropagateNaN>(Bf16ToF32(a)));
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet8bf& a) {
  return _mm_movemask_epi8(a) != 0;
}

}  // end namespace internal
}  // end namespace Eigen

#endif  // EIGEN_REDUCTIONS_AVX_H
