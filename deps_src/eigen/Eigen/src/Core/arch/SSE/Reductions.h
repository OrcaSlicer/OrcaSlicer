// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 Charlie Schlosser <cs.schlosser@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_REDUCTIONS_SSE_H
#define EIGEN_REDUCTIONS_SSE_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

template <typename Packet>
struct sse_add_wrapper {
  static EIGEN_STRONG_INLINE Packet packetOp(const Packet& a, const Packet& b) { return padd<Packet>(a, b); }
};

template <typename Packet>
struct sse_mul_wrapper {
  static EIGEN_STRONG_INLINE Packet packetOp(const Packet& a, const Packet& b) { return pmul<Packet>(a, b); }
};

template <typename Packet>
struct sse_min_wrapper {
  static EIGEN_STRONG_INLINE Packet packetOp(const Packet& a, const Packet& b) { return pmin<Packet>(a, b); }
};

template <int NaNPropagation, typename Packet>
struct sse_min_prop_wrapper {
  static EIGEN_STRONG_INLINE Packet packetOp(const Packet& a, const Packet& b) {
    return pmin<NaNPropagation, Packet>(a, b);
  }
};

template <typename Packet>
struct sse_max_wrapper {
  static EIGEN_STRONG_INLINE Packet packetOp(const Packet& a, const Packet& b) { return pmax<Packet>(a, b); }
};

template <int NaNPropagation, typename Packet>
struct sse_max_prop_wrapper {
  static EIGEN_STRONG_INLINE Packet packetOp(const Packet& a, const Packet& b) {
    return pmax<NaNPropagation, Packet>(a, b);
  }
};

template <typename Packet, typename Op>
struct sse_predux_common;

template <typename Packet>
struct sse_predux_impl : sse_predux_common<Packet, sse_add_wrapper<Packet>> {};

template <typename Packet>
struct sse_predux_mul_impl : sse_predux_common<Packet, sse_mul_wrapper<Packet>> {};

template <typename Packet>
struct sse_predux_min_impl : sse_predux_common<Packet, sse_min_wrapper<Packet>> {};

template <int NaNPropagation, typename Packet>
struct sse_predux_min_prop_impl : sse_predux_common<Packet, sse_min_prop_wrapper<NaNPropagation, Packet>> {};

template <typename Packet>
struct sse_predux_max_impl : sse_predux_common<Packet, sse_max_wrapper<Packet>> {};

template <int NaNPropagation, typename Packet>
struct sse_predux_max_prop_impl : sse_predux_common<Packet, sse_max_prop_wrapper<NaNPropagation, Packet>> {};

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet16b -- -- -- -- -- -- -- -- -- -- -- -- */

template <>
EIGEN_STRONG_INLINE bool predux(const Packet16b& a) {
  Packet4i tmp = _mm_or_si128(a, _mm_unpackhi_epi64(a, a));
  return (pfirst(tmp) != 0) || (pfirst<Packet4i>(_mm_shuffle_epi32(tmp, 1)) != 0);
}

template <>
EIGEN_STRONG_INLINE bool predux_mul(const Packet16b& a) {
  Packet4i tmp = _mm_and_si128(a, _mm_unpackhi_epi64(a, a));
  return ((pfirst<Packet4i>(tmp) == 0x01010101) && (pfirst<Packet4i>(_mm_shuffle_epi32(tmp, 1)) == 0x01010101));
}

template <>
EIGEN_STRONG_INLINE bool predux_min(const Packet16b& a) {
  return predux_mul(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_max(const Packet16b& a) {
  return predux(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet16b& a) {
  return predux(a);
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet4i -- -- -- -- -- -- -- -- -- -- -- -- */

template <typename Op>
struct sse_predux_common<Packet4i, Op> {
  static EIGEN_STRONG_INLINE int run(const Packet4i& a) {
    Packet4i tmp;
    tmp = Op::packetOp(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(0, 1, 2, 3)));
    tmp = Op::packetOp(tmp, _mm_unpackhi_epi32(tmp, tmp));
    return _mm_cvtsi128_si32(tmp);
  }
};

template <>
EIGEN_STRONG_INLINE int predux(const Packet4i& a) {
  return sse_predux_impl<Packet4i>::run(a);
}

template <>
EIGEN_STRONG_INLINE int predux_mul(const Packet4i& a) {
  return sse_predux_mul_impl<Packet4i>::run(a);
}

#ifdef EIGEN_VECTORIZE_SSE4_1
template <>
EIGEN_STRONG_INLINE int predux_min(const Packet4i& a) {
  return sse_predux_min_impl<Packet4i>::run(a);
}

template <>
EIGEN_STRONG_INLINE int predux_max(const Packet4i& a) {
  return sse_predux_max_impl<Packet4i>::run(a);
}
#endif

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet4i& a) {
  return _mm_movemask_ps(_mm_castsi128_ps(a)) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet4ui -- -- -- -- -- -- -- -- -- -- -- -- */

template <typename Op>
struct sse_predux_common<Packet4ui, Op> {
  static EIGEN_STRONG_INLINE uint32_t run(const Packet4ui& a) {
    Packet4ui tmp;
    tmp = Op::packetOp(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(0, 1, 2, 3)));
    tmp = Op::packetOp(tmp, _mm_unpackhi_epi32(tmp, tmp));
    return static_cast<uint32_t>(_mm_cvtsi128_si32(tmp));
  }
};

template <>
EIGEN_STRONG_INLINE uint32_t predux(const Packet4ui& a) {
  return sse_predux_impl<Packet4ui>::run(a);
}

template <>
EIGEN_STRONG_INLINE uint32_t predux_mul(const Packet4ui& a) {
  return sse_predux_mul_impl<Packet4ui>::run(a);
}

#ifdef EIGEN_VECTORIZE_SSE4_1
template <>
EIGEN_STRONG_INLINE uint32_t predux_min(const Packet4ui& a) {
  return sse_predux_min_impl<Packet4ui>::run(a);
}

template <>
EIGEN_STRONG_INLINE uint32_t predux_max(const Packet4ui& a) {
  return sse_predux_max_impl<Packet4ui>::run(a);
}
#endif

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet4ui& a) {
  return _mm_movemask_ps(_mm_castsi128_ps(a)) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet2l -- -- -- -- -- -- -- -- -- -- -- -- */

template <typename Op>
struct sse_predux_common<Packet2l, Op> {
  static EIGEN_STRONG_INLINE int64_t run(const Packet2l& a) {
    Packet2l tmp;
    tmp = Op::packetOp(a, _mm_unpackhi_epi64(a, a));
    return pfirst(tmp);
  }
};

template <>
EIGEN_STRONG_INLINE int64_t predux(const Packet2l& a) {
  return sse_predux_impl<Packet2l>::run(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet2l& a) {
  return _mm_movemask_pd(_mm_castsi128_pd(a)) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet4f -- -- -- -- -- -- -- -- -- -- -- -- */

template <typename Op>
struct sse_predux_common<Packet4f, Op> {
  static EIGEN_STRONG_INLINE float run(const Packet4f& a) {
    Packet4f tmp;
    tmp = Op::packetOp(a, _mm_movehl_ps(a, a));
#ifdef EIGEN_VECTORIZE_SSE3
    tmp = Op::packetOp(tmp, _mm_movehdup_ps(tmp));
#else
    tmp = Op::packetOp(tmp, _mm_shuffle_ps(tmp, tmp, 1));
#endif
    return _mm_cvtss_f32(tmp);
  }
};

template <>
EIGEN_STRONG_INLINE float predux(const Packet4f& a) {
  return sse_predux_impl<Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_mul(const Packet4f& a) {
  return sse_predux_mul_impl<Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_min(const Packet4f& a) {
  return sse_predux_min_impl<Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_min<PropagateNumbers>(const Packet4f& a) {
  return sse_predux_min_prop_impl<PropagateNumbers, Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_min<PropagateNaN>(const Packet4f& a) {
  return sse_predux_min_prop_impl<PropagateNaN, Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_max(const Packet4f& a) {
  return sse_predux_max_impl<Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_max<PropagateNumbers>(const Packet4f& a) {
  return sse_predux_max_prop_impl<PropagateNumbers, Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE float predux_max<PropagateNaN>(const Packet4f& a) {
  return sse_predux_max_prop_impl<PropagateNaN, Packet4f>::run(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet4f& a) {
  return _mm_movemask_ps(a) != 0x0;
}

/* -- -- -- -- -- -- -- -- -- -- -- -- Packet2d -- -- -- -- -- -- -- -- -- -- -- -- */

template <typename Op>
struct sse_predux_common<Packet2d, Op> {
  static EIGEN_STRONG_INLINE double run(const Packet2d& a) {
    Packet2d tmp;
    tmp = Op::packetOp(a, _mm_unpackhi_pd(a, a));
    return _mm_cvtsd_f64(tmp);
  }
};

template <>
EIGEN_STRONG_INLINE double predux(const Packet2d& a) {
  return sse_predux_impl<Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_mul(const Packet2d& a) {
  return sse_predux_mul_impl<Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_min(const Packet2d& a) {
  return sse_predux_min_impl<Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_min<PropagateNumbers>(const Packet2d& a) {
  return sse_predux_min_prop_impl<PropagateNumbers, Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_min<PropagateNaN>(const Packet2d& a) {
  return sse_predux_min_prop_impl<PropagateNaN, Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_max(const Packet2d& a) {
  return sse_predux_max_impl<Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_max<PropagateNumbers>(const Packet2d& a) {
  return sse_predux_max_prop_impl<PropagateNumbers, Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE double predux_max<PropagateNaN>(const Packet2d& a) {
  return sse_predux_max_prop_impl<PropagateNaN, Packet2d>::run(a);
}

template <>
EIGEN_STRONG_INLINE bool predux_any(const Packet2d& a) {
  return _mm_movemask_pd(a) != 0x0;
}

}  // end namespace internal

}  // end namespace Eigen

#endif  // EIGEN_REDUCTIONS_SSE_H
