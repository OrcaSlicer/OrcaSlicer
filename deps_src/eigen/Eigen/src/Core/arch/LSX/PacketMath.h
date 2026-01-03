// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2023 Zang Ruochen <zangruochen@loongson.cn>
// Copyright (C) 2024 XiWei Gu <guxiwei-hf@loongson.cn>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_PACKET_MATH_LSX_H
#define EIGEN_PACKET_MATH_LSX_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

#ifndef EIGEN_CACHEFRIENDLY_PRODUCT_THRESHOLD
#define EIGEN_CACHEFRIENDLY_PRODUCT_THRESHOLD 8
#endif

#ifndef EIGEN_ARCH_DEFAULT_NUMBER_OF_REGISTERS
#if EIGEN_ARCH_LOONGARCH64
#define EIGEN_ARCH_DEFAULT_NUMBER_OF_REGISTERS 32
#endif
#endif

#ifndef EIGEN_HAS_SINGLE_INSTRUCTION_MADD
#define EIGEN_HAS_SINGLE_INSTRUCTION_MADD
#endif

typedef __m128 Packet4f;
typedef __m128d Packet2d;

typedef eigen_packet_wrapper<__m128i, 0> Packet16c;
typedef eigen_packet_wrapper<__m128i, 1> Packet8s;
typedef eigen_packet_wrapper<__m128i, 2> Packet4i;
typedef eigen_packet_wrapper<__m128i, 3> Packet2l;
typedef eigen_packet_wrapper<__m128i, 4> Packet16uc;
typedef eigen_packet_wrapper<__m128i, 5> Packet8us;
typedef eigen_packet_wrapper<__m128i, 6> Packet4ui;
typedef eigen_packet_wrapper<__m128i, 7> Packet2ul;

template <>
struct is_arithmetic<__m128> {
  enum { value = true };
};
template <>
struct is_arithmetic<__m128i> {
  enum { value = true };
};
template <>
struct is_arithmetic<__m128d> {
  enum { value = true };
};
template <>
struct is_arithmetic<Packet16c> {
  enum { value = true };
};
template <>
struct is_arithmetic<Packet8s> {
  enum { value = true };
};
template <>
struct is_arithmetic<Packet4i> {
  enum { value = true };
};
template <>
struct is_arithmetic<Packet2l> {
  enum { value = true };
};
template <>
struct is_arithmetic<Packet16uc> {
  enum { value = false };
};
template <>
struct is_arithmetic<Packet8us> {
  enum { value = false };
};
template <>
struct is_arithmetic<Packet4ui> {
  enum { value = false };
};
template <>
struct is_arithmetic<Packet2ul> {
  enum { value = false };
};

EIGEN_ALWAYS_INLINE Packet4f make_packet4f(float a, float b, float c, float d) {
  float from[4] = {a, b, c, d};
  return (Packet4f)__lsx_vld(from, 0);
}

EIGEN_STRONG_INLINE Packet4f shuffle1(const Packet4f& m, int mask) {
  const float* a = reinterpret_cast<const float*>(&m);
  Packet4f res =
      make_packet4f(*(a + (mask & 3)), *(a + ((mask >> 2) & 3)), *(a + ((mask >> 4) & 3)), *(a + ((mask >> 6) & 3)));
  return res;
}

template <bool interleave>
EIGEN_STRONG_INLINE Packet4f shuffle2(const Packet4f& m, const Packet4f& n, int mask) {
  const float* a = reinterpret_cast<const float*>(&m);
  const float* b = reinterpret_cast<const float*>(&n);
  Packet4f res =
      make_packet4f(*(a + (mask & 3)), *(a + ((mask >> 2) & 3)), *(b + ((mask >> 4) & 3)), *(b + ((mask >> 6) & 3)));
  return res;
}

template <>
EIGEN_STRONG_INLINE Packet4f shuffle2<true>(const Packet4f& m, const Packet4f& n, int mask) {
  const float* a = reinterpret_cast<const float*>(&m);
  const float* b = reinterpret_cast<const float*>(&n);
  Packet4f res =
      make_packet4f(*(a + (mask & 3)), *(b + ((mask >> 2) & 3)), *(a + ((mask >> 4) & 3)), *(b + ((mask >> 6) & 3)));
  return res;
}

EIGEN_STRONG_INLINE static int eigen_lsx_shuffle_mask(int p, int q, int r, int s) {
  return ((s) << 6 | (r) << 4 | (q) << 2 | (p));
}

EIGEN_STRONG_INLINE Packet4f vec4f_swizzle1(const Packet4f& a, int p, int q, int r, int s) {
  return shuffle1(a, eigen_lsx_shuffle_mask(p, q, r, s));
}
EIGEN_STRONG_INLINE Packet4f vec4f_swizzle2(const Packet4f& a, const Packet4f& b, int p, int q, int r, int s) {
  return shuffle2<false>(a, b, eigen_lsx_shuffle_mask(p, q, r, s));
}
EIGEN_STRONG_INLINE Packet4f vec4f_movelh(const Packet4f& a, const Packet4f& b) {
  return shuffle2<false>(a, b, eigen_lsx_shuffle_mask(0, 1, 0, 1));
}
EIGEN_STRONG_INLINE Packet4f vec4f_movehl(const Packet4f& a, const Packet4f& b) {
  return shuffle2<false>(b, a, eigen_lsx_shuffle_mask(2, 3, 2, 3));
}
EIGEN_STRONG_INLINE Packet4f vec4f_unpacklo(const Packet4f& a, const Packet4f& b) {
  return shuffle2<true>(a, b, eigen_lsx_shuffle_mask(0, 0, 1, 1));
}
EIGEN_STRONG_INLINE Packet4f vec4f_unpackhi(const Packet4f& a, const Packet4f& b) {
  return shuffle2<true>(a, b, eigen_lsx_shuffle_mask(2, 2, 3, 3));
}

EIGEN_ALWAYS_INLINE Packet2d make_packet2d(double a, double b) {
  double from[2] = {a, b};
  return (Packet2d)__lsx_vld(from, 0);
}

EIGEN_STRONG_INLINE Packet2d shuffle(const Packet2d& m, const Packet2d& n, int mask) {
  const double* a = reinterpret_cast<const double*>(&m);
  const double* b = reinterpret_cast<const double*>(&n);
  Packet2d res = make_packet2d(*(a + (mask & 1)), *(b + ((mask >> 1) & 1)));
  return res;
}

EIGEN_STRONG_INLINE Packet2d vec2d_swizzle2(const Packet2d& a, const Packet2d& b, int mask) {
  return shuffle(a, b, mask);
}
EIGEN_STRONG_INLINE Packet2d vec2d_unpacklo(const Packet2d& a, const Packet2d& b) { return shuffle(a, b, 0); }
EIGEN_STRONG_INLINE Packet2d vec2d_unpackhi(const Packet2d& a, const Packet2d& b) { return shuffle(a, b, 3); }

template <>
struct packet_traits<int8_t> : default_packet_traits {
  typedef Packet16c type;
  typedef Packet16c half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 16,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasCmp = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<int16_t> : default_packet_traits {
  typedef Packet8s type;
  typedef Packet8s half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 8,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasCmp = 1,
    HasDiv = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<int32_t> : default_packet_traits {
  typedef Packet4i type;
  typedef Packet4i half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 4,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasCmp = 1,
    HasDiv = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<int64_t> : default_packet_traits {
  typedef Packet2l type;
  typedef Packet2l half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 2,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasCmp = 1,
    HasDiv = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<uint8_t> : default_packet_traits {
  typedef Packet16uc type;
  typedef Packet16uc half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 16,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasNegate = 0,
    HasCmp = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<uint16_t> : default_packet_traits {
  typedef Packet8us type;
  typedef Packet8us half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 8,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasNegate = 0,
    HasCmp = 1,
    HasDiv = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<uint32_t> : default_packet_traits {
  typedef Packet4ui type;
  typedef Packet4ui half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 4,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasNegate = 0,
    HasCmp = 1,
    HasDiv = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<uint64_t> : default_packet_traits {
  typedef Packet2ul type;
  typedef Packet2ul half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 2,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasNegate = 0,
    HasCmp = 1,
    HasDiv = 1,
    HasBlend = 0
  };
};

template <>
struct packet_traits<float> : default_packet_traits {
  typedef Packet4f type;
  typedef Packet4f half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 4,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasBlend = 0,
    HasSign = 0,
    HasDiv = 1,
    HasExp = 1,
    HasSqrt = 1,
    HasLog = 1,
    HasRsqrt = 1
  };
};

template <>
struct packet_traits<double> : default_packet_traits {
  typedef Packet2d type;
  typedef Packet2d half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 2,

    HasAbs2 = 0,
    HasSetLinear = 0,
    HasBlend = 0,
    HasSign = 0,
    HasDiv = 1,
    HasSqrt = 1,
    HasLog = 1,
    HasRsqrt = 1
  };
};

template <>
struct unpacket_traits<Packet16c> {
  typedef int8_t type;
  typedef Packet16c half;
  enum {
    size = 16,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet8s> {
  typedef int16_t type;
  typedef Packet8s half;
  enum {
    size = 8,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet4i> {
  typedef int32_t type;
  typedef Packet4i half;
  enum {
    size = 4,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet2l> {
  typedef int64_t type;
  typedef Packet2l half;
  enum {
    size = 2,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet16uc> {
  typedef uint8_t type;
  typedef Packet16uc half;
  enum {
    size = 16,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet8us> {
  typedef uint16_t type;
  typedef Packet8us half;
  enum {
    size = 8,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet4ui> {
  typedef uint32_t type;
  typedef Packet4ui half;
  enum {
    size = 4,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet2ul> {
  typedef uint64_t type;
  typedef Packet2ul half;
  enum {
    size = 2,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet4f> {
  typedef float type;
  typedef Packet4f half;
  typedef Packet4i integer_packet;
  enum {
    size = 4,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};
template <>
struct unpacket_traits<Packet2d> {
  typedef double type;
  typedef Packet2d half;
  typedef Packet2l integer_packet;
  enum {
    size = 2,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};

template <>
EIGEN_STRONG_INLINE Packet16c pset1<Packet16c>(const int8_t& from) {
  return __lsx_vreplgr2vr_b(from);
}
template <>
EIGEN_STRONG_INLINE Packet8s pset1<Packet8s>(const int16_t& from) {
  return __lsx_vreplgr2vr_h(from);
}
template <>
EIGEN_STRONG_INLINE Packet4i pset1<Packet4i>(const int32_t& from) {
  return __lsx_vreplgr2vr_w(from);
}
template <>
EIGEN_STRONG_INLINE Packet2l pset1<Packet2l>(const int64_t& from) {
  return __lsx_vreplgr2vr_d(from);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pset1<Packet16uc>(const uint8_t& from) {
  return __lsx_vreplgr2vr_b(from);
}
template <>
EIGEN_STRONG_INLINE Packet8us pset1<Packet8us>(const uint16_t& from) {
  return __lsx_vreplgr2vr_h(from);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pset1<Packet4ui>(const uint32_t& from) {
  return __lsx_vreplgr2vr_w(from);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pset1<Packet2ul>(const uint64_t& from) {
  return __lsx_vreplgr2vr_d(from);
}
template <>
EIGEN_STRONG_INLINE Packet4f pset1<Packet4f>(const float& from) {
  Packet4f v = {from, from, from, from};
  return v;
}
template <>
EIGEN_STRONG_INLINE Packet2d pset1<Packet2d>(const double& from) {
  Packet2d v = {from, from};
  return v;
}

template <>
EIGEN_STRONG_INLINE Packet4f pset1frombits<Packet4f>(uint32_t from) {
  return reinterpret_cast<__m128>((__m128i)pset1<Packet4ui>(from));
}
template <>
EIGEN_STRONG_INLINE Packet2d pset1frombits<Packet2d>(uint64_t from) {
  return reinterpret_cast<__m128d>((__m128i)pset1<Packet2ul>(from));
}

template <>
EIGEN_STRONG_INLINE Packet16c plset<Packet16c>(const int8_t& a) {
  const int8_t countdown[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  return __lsx_vadd_b(pset1<Packet16c>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet8s plset<Packet8s>(const int16_t& a) {
  const int16_t countdown[] = {0, 1, 2, 3, 4, 5, 6, 7};
  return __lsx_vadd_h(pset1<Packet8s>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet4i plset<Packet4i>(const int32_t& a) {
  const int32_t countdown[] = {0, 1, 2, 3};
  return __lsx_vadd_w(pset1<Packet4i>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2l plset<Packet2l>(const int64_t& a) {
  const int64_t countdown[] = {0, 1};
  return __lsx_vadd_d(pset1<Packet2l>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet16uc plset<Packet16uc>(const uint8_t& a) {
  const uint8_t countdown[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  return __lsx_vadd_b(pset1<Packet16uc>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet8us plset<Packet8us>(const uint16_t& a) {
  const uint16_t countdown[] = {0, 1, 2, 3, 4, 5, 6, 7};
  return __lsx_vadd_h(pset1<Packet8us>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet4ui plset<Packet4ui>(const uint32_t& a) {
  const uint32_t countdown[] = {0, 1, 2, 3};
  return __lsx_vadd_w(pset1<Packet4ui>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2ul plset<Packet2ul>(const uint64_t& a) {
  const uint64_t countdown[] = {0, 1};
  return __lsx_vadd_d(pset1<Packet2ul>(a), __lsx_vld(countdown, 0));
}
template <>
EIGEN_STRONG_INLINE Packet4f plset<Packet4f>(const float& a) {
  static const Packet4f countdown = {0.0f, 1.0f, 2.0f, 3.0f};
  return __lsx_vfadd_s(pset1<Packet4f>(a), countdown);
}
template <>
EIGEN_STRONG_INLINE Packet2d plset<Packet2d>(const double& a) {
  static const Packet2d countdown = {0.0f, 1.0f};
  return __lsx_vfadd_d(pset1<Packet2d>(a), countdown);
}

template <>
EIGEN_STRONG_INLINE Packet16c padd<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vadd_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s padd<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vadd_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i padd<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vadd_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l padd<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vadd_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc padd<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vadd_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us padd<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vadd_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui padd<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vadd_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul padd<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vadd_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4f padd<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return __lsx_vfadd_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d padd<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vfadd_d(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16c psub<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vsub_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s psub<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vsub_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i psub<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vsub_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l psub<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vsub_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc psub<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vsub_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us psub<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vsub_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui psub<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vsub_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul psub<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vsub_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4f psub<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return __lsx_vfsub_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d psub<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vfsub_d(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pxor<Packet4f>(const Packet4f& a, const Packet4f& b);
template <>
EIGEN_STRONG_INLINE Packet4f paddsub<Packet4f>(const Packet4f& a, const Packet4f& b) {
  const Packet4f mask =
      make_packet4f(numext::bit_cast<float>(0x80000000u), 0.0f, numext::bit_cast<float>(0x80000000u), 0.0f);
  return padd(a, pxor(mask, b));
}
template <>
EIGEN_STRONG_INLINE Packet2d pxor<Packet2d>(const Packet2d& a, const Packet2d& b);
template <>
EIGEN_STRONG_INLINE Packet2d paddsub<Packet2d>(const Packet2d& a, const Packet2d& b) {
  const Packet2d mask = make_packet2d(numext::bit_cast<double>(0x8000000000000000ull), 0.0);
  return padd(a, pxor(mask, b));
}

template <>
EIGEN_STRONG_INLINE Packet4f pnegate(const Packet4f& a) {
  Packet4f mask = make_packet4f(numext::bit_cast<float>(0x80000000), numext::bit_cast<float>(0x80000000),
                                numext::bit_cast<float>(0x80000000), numext::bit_cast<float>(0x80000000));
  return (Packet4f)__lsx_vxor_v(numext::bit_cast<__m128i>(mask), numext::bit_cast<__m128i>(a));
}
template <>
EIGEN_STRONG_INLINE Packet2d pnegate(const Packet2d& a) {
  Packet2d mask =
      make_packet2d(numext::bit_cast<double>(0x8000000000000000), numext::bit_cast<double>(0x8000000000000000));
  return (Packet2d)__lsx_vxor_v(numext::bit_cast<__m128i>(mask), numext::bit_cast<__m128i>(a));
}
template <>
EIGEN_STRONG_INLINE Packet16c pnegate(const Packet16c& a) {
  return __lsx_vneg_b(a);
}
template <>
EIGEN_STRONG_INLINE Packet8s pnegate(const Packet8s& a) {
  return __lsx_vneg_h(a);
}
template <>
EIGEN_STRONG_INLINE Packet4i pnegate(const Packet4i& a) {
  return __lsx_vneg_w(a);
}
template <>
EIGEN_STRONG_INLINE Packet2l pnegate(const Packet2l& a) {
  return __lsx_vneg_d(a);
}

template <>
EIGEN_STRONG_INLINE Packet4f pconj(const Packet4f& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet2d pconj(const Packet2d& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet16c pconj(const Packet16c& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet8s pconj(const Packet8s& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet4i pconj(const Packet4i& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet2l pconj(const Packet2l& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet16uc pconj(const Packet16uc& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet8us pconj(const Packet8us& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet4ui pconj(const Packet4ui& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet2ul pconj(const Packet2ul& a) {
  return a;
}

template <>
EIGEN_STRONG_INLINE Packet4f pmul<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return __lsx_vfmul_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pmul<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vfmul_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16c pmul<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vmul_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pmul<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vmul_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pmul<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vmul_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pmul<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vmul_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pmul<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vmul_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pmul<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vmul_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pmul<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vmul_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pmul<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vmul_d(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pdiv<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return __lsx_vfdiv_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pdiv<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vfdiv_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pdiv<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vdiv_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pdiv<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vdiv_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pdiv<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vdiv_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pdiv<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vdiv_hu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pdiv<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vdiv_wu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pdiv<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vdiv_du(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pmadd(const Packet4f& a, const Packet4f& b, const Packet4f& c) {
  return __lsx_vfmadd_s(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet2d pmadd(const Packet2d& a, const Packet2d& b, const Packet2d& c) {
  return __lsx_vfmadd_d(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet4f pmsub(const Packet4f& a, const Packet4f& b, const Packet4f& c) {
  return __lsx_vfmsub_s(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet2d pmsub(const Packet2d& a, const Packet2d& b, const Packet2d& c) {
  return __lsx_vfmsub_d(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet4f pnmadd(const Packet4f& a, const Packet4f& b, const Packet4f& c) {
  return __lsx_vfnmsub_s(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet2d pnmadd(const Packet2d& a, const Packet2d& b, const Packet2d& c) {
  return __lsx_vfnmsub_d(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet4f pnmsub(const Packet4f& a, const Packet4f& b, const Packet4f& c) {
  return __lsx_vfnmadd_s(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet2d pnmsub(const Packet2d& a, const Packet2d& b, const Packet2d& c) {
  return __lsx_vfnmadd_d(a, b, c);
}
template <>
EIGEN_STRONG_INLINE Packet16c pmadd(const Packet16c& a, const Packet16c& b, const Packet16c& c) {
  return __lsx_vmadd_b(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pmadd(const Packet8s& a, const Packet8s& b, const Packet8s& c) {
  return __lsx_vmadd_h(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pmadd(const Packet4i& a, const Packet4i& b, const Packet4i& c) {
  return __lsx_vmadd_w(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pmadd(const Packet2l& a, const Packet2l& b, const Packet2l& c) {
  return __lsx_vmadd_d(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pmadd(const Packet16uc& a, const Packet16uc& b, const Packet16uc& c) {
  return __lsx_vmadd_b(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pmadd(const Packet8us& a, const Packet8us& b, const Packet8us& c) {
  return __lsx_vmadd_h(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pmadd(const Packet4ui& a, const Packet4ui& b, const Packet4ui& c) {
  return __lsx_vmadd_w(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pmadd(const Packet2ul& a, const Packet2ul& b, const Packet2ul& c) {
  return __lsx_vmadd_d(c, a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pand<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vand_v((__m128i)a, (__m128i)b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pand<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vand_v((__m128i)a, (__m128i)b);
}
template <>
EIGEN_STRONG_INLINE Packet16c pand<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pand<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pand<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pand<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pand<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pand<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pand<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vand_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pand<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vand_v(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f por<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vor_v((__m128i)a, (__m128i)b);
}
template <>
EIGEN_STRONG_INLINE Packet2d por<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vor_v((__m128i)a, (__m128i)b);
}
template <>
EIGEN_STRONG_INLINE Packet16c por<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s por<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i por<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l por<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc por<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us por<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui por<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul por<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vor_v(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pxor<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vxor_v((__m128i)a, (__m128i)b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pxor<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vxor_v((__m128i)a, (__m128i)b);
}
template <>
EIGEN_STRONG_INLINE Packet16c pxor<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pxor<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pxor<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pxor<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pxor<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pxor<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pxor<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vxor_v(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pxor<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vxor_v(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pandnot<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vandn_v((__m128i)b, (__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet2d pandnot<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vandn_v((__m128i)b, (__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet16c pandnot<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet8s pandnot<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet4i pandnot<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet2l pandnot<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pandnot<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet8us pandnot<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pandnot<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vandn_v(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pandnot<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vandn_v(b, a);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcmp_le<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vfcmp_cle_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pcmp_le<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vfcmp_cle_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcmp_le<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vsle_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcmp_le<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vsle_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcmp_le<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vsle_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcmp_le<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vsle_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcmp_le<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vsle_bu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcmp_le<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vsle_hu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcmp_le<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vsle_wu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcmp_le<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vsle_du(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcmp_lt<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vfcmp_clt_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pcmp_lt<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vfcmp_clt_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcmp_lt<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vslt_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcmp_lt<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vslt_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcmp_lt<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vslt_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcmp_lt<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vslt_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcmp_lt<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vslt_bu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcmp_lt<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vslt_hu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcmp_lt<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vslt_wu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcmp_lt<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vslt_du(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcmp_lt_or_nan<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vfcmp_sult_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pcmp_lt_or_nan<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vfcmp_sult_d(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcmp_eq<Packet4f>(const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vfcmp_seq_s(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pcmp_eq<Packet2d>(const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vfcmp_seq_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcmp_eq<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vseq_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcmp_eq<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vseq_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcmp_eq<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vseq_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcmp_eq<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vseq_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcmp_eq<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vseq_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcmp_eq<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vseq_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcmp_eq<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vseq_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcmp_eq<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vseq_d(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16c pmin<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vmin_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pmin<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vmin_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pmin<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vmin_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pmin<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vmin_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pmin<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vmin_bu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pmin<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vmin_hu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pmin<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vmin_wu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pmin<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vmin_du(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16c pmax<Packet16c>(const Packet16c& a, const Packet16c& b) {
  return __lsx_vmax_b(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pmax<Packet8s>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vmax_h(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pmax<Packet4i>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vmax_w(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pmax<Packet2l>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vmax_d(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pmax<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vmax_bu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8us pmax<Packet8us>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vmax_hu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pmax<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vmax_wu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pmax<Packet2ul>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vmax_du(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pmin<Packet4f>(const Packet4f& a, const Packet4f& b) {
  Packet4i aNaN = __lsx_vfcmp_cun_s(a, a);
  Packet4i aMinOrNaN = por<Packet4i>(__lsx_vfcmp_clt_s(a, b), aNaN);
  return (Packet4f)__lsx_vbitsel_v((__m128i)b, (__m128i)a, aMinOrNaN);
}
template <>
EIGEN_STRONG_INLINE Packet2d pmin<Packet2d>(const Packet2d& a, const Packet2d& b) {
  Packet2l aNaN = __lsx_vfcmp_cun_d(a, a);
  Packet2l aMinOrNaN = por<Packet2l>(__lsx_vfcmp_clt_d(a, b), aNaN);
  return (Packet2d)__lsx_vbitsel_v((__m128i)b, (__m128i)a, aMinOrNaN);
}
template <>
EIGEN_STRONG_INLINE Packet4f pmax<Packet4f>(const Packet4f& a, const Packet4f& b) {
  Packet4i aNaN = __lsx_vfcmp_cun_s(a, a);
  Packet4i aMaxOrNaN = por<Packet4i>(__lsx_vfcmp_clt_s(b, a), aNaN);
  return (Packet4f)__lsx_vbitsel_v((__m128i)b, (__m128i)a, aMaxOrNaN);
}
template <>
EIGEN_STRONG_INLINE Packet2d pmax<Packet2d>(const Packet2d& a, const Packet2d& b) {
  Packet2l aNaN = __lsx_vfcmp_cun_d(a, a);
  Packet2l aMaxOrNaN = por<Packet2l>(__lsx_vfcmp_clt_d(b, a), aNaN);
  return (Packet2d)__lsx_vbitsel_v((__m128i)b, (__m128i)a, aMaxOrNaN);
}

template <int N>
EIGEN_STRONG_INLINE Packet16c parithmetic_shift_right(const Packet16c& a) {
  return __lsx_vsrai_b((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet8s parithmetic_shift_right(const Packet8s& a) {
  return __lsx_vsrai_h((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet4i parithmetic_shift_right(const Packet4i& a) {
  return __lsx_vsrai_w((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet2l parithmetic_shift_right(const Packet2l& a) {
  return __lsx_vsrai_d((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet16uc parithmetic_shift_right(const Packet16uc& a) {
  return __lsx_vsrli_b((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet8us parithmetic_shift_right(const Packet8us& a) {
  return __lsx_vsrli_h((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet4ui parithmetic_shift_right(const Packet4ui& a) {
  return __lsx_vsrli_w((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet2ul parithmetic_shift_right(const Packet2ul& a) {
  return __lsx_vsrli_d((__m128i)a, N);
}

template <int N>
EIGEN_STRONG_INLINE Packet16c plogical_shift_right(const Packet16c& a) {
  return __lsx_vsrli_b((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet8s plogical_shift_right(const Packet8s& a) {
  return __lsx_vsrli_h((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet4i plogical_shift_right(const Packet4i& a) {
  return __lsx_vsrli_w((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet2l plogical_shift_right(const Packet2l& a) {
  return __lsx_vsrli_d((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet16uc plogical_shift_right(const Packet16uc& a) {
  return __lsx_vsrli_b((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet8us plogical_shift_right(const Packet8us& a) {
  return __lsx_vsrli_h((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet4ui plogical_shift_right(const Packet4ui& a) {
  return __lsx_vsrli_w((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet2ul plogical_shift_right(const Packet2ul& a) {
  return __lsx_vsrli_d((__m128i)a, N);
}

template <int N>
EIGEN_STRONG_INLINE Packet16c plogical_shift_left(const Packet16c& a) {
  return __lsx_vslli_b((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet8s plogical_shift_left(const Packet8s& a) {
  return __lsx_vslli_h((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet4i plogical_shift_left(const Packet4i& a) {
  return __lsx_vslli_w((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet2l plogical_shift_left(const Packet2l& a) {
  return __lsx_vslli_d((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet16uc plogical_shift_left(const Packet16uc& a) {
  return __lsx_vslli_b((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet8us plogical_shift_left(const Packet8us& a) {
  return __lsx_vslli_h((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet4ui plogical_shift_left(const Packet4ui& a) {
  return __lsx_vslli_w((__m128i)a, N);
}
template <int N>
EIGEN_STRONG_INLINE Packet2ul plogical_shift_left(const Packet2ul& a) {
  return __lsx_vslli_d((__m128i)a, N);
}

template <>
EIGEN_STRONG_INLINE Packet4f pabs(const Packet4f& a) {
  return (Packet4f)__lsx_vbitclri_w((__m128i)a, 31);
}
template <>
EIGEN_STRONG_INLINE Packet2d pabs(const Packet2d& a) {
  return (Packet2d)__lsx_vbitclri_d((__m128i)a, 63);
}
template <>
EIGEN_STRONG_INLINE Packet16c pabs(const Packet16c& a) {
  return __lsx_vabsd_b(a, pzero(a));
}
template <>
EIGEN_STRONG_INLINE Packet8s pabs(const Packet8s& a) {
  return __lsx_vabsd_h(a, pzero(a));
}
template <>
EIGEN_STRONG_INLINE Packet4i pabs(const Packet4i& a) {
  return __lsx_vabsd_w(a, pzero(a));
}
template <>
EIGEN_STRONG_INLINE Packet2l pabs(const Packet2l& a) {
  return __lsx_vabsd_d(a, pzero(a));
}
template <>
EIGEN_STRONG_INLINE Packet16uc pabs(const Packet16uc& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet8us pabs(const Packet8us& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet4ui pabs(const Packet4ui& a) {
  return a;
}
template <>
EIGEN_STRONG_INLINE Packet2ul pabs(const Packet2ul& a) {
  return a;
}

template <>
EIGEN_STRONG_INLINE Packet4f pload<Packet4f>(const float* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return (Packet4f)__lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2d pload<Packet2d>(const double* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return (Packet2d)__lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pload<Packet16c>(const int8_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pload<Packet8s>(const int16_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i pload<Packet4i>(const int32_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2l pload<Packet2l>(const int64_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pload<Packet16uc>(const uint8_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pload<Packet8us>(const uint16_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pload<Packet4ui>(const uint32_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pload<Packet2ul>(const uint64_t* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return __lsx_vld(from, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f ploadu<Packet4f>(const float* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return (Packet4f)__lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2d ploadu<Packet2d>(const double* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return (Packet2d)__lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c ploadu<Packet16c>(const int8_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s ploadu<Packet8s>(const int16_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i ploadu<Packet4i>(const int32_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2l ploadu<Packet2l>(const int64_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc ploadu<Packet16uc>(const uint8_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us ploadu<Packet8us>(const uint16_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui ploadu<Packet4ui>(const uint32_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2ul ploadu<Packet2ul>(const uint64_t* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return __lsx_vld(from, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f ploaddup<Packet4f>(const float* from) {
  float f0 = from[0], f1 = from[1];
  return make_packet4f(f0, f0, f1, f1);
}
template <>
EIGEN_STRONG_INLINE Packet2d ploaddup<Packet2d>(const double* from) {
  return pset1<Packet2d>(from[0]);
}
template <>
EIGEN_STRONG_INLINE Packet16c ploaddup<Packet16c>(const int8_t* from) {
  Packet16c tmp = pload<Packet16c>(from);
  return __lsx_vilvl_b(tmp, tmp);
}
template <>
EIGEN_STRONG_INLINE Packet8s ploaddup<Packet8s>(const int16_t* from) {
  Packet8s tmp = pload<Packet8s>(from);
  return __lsx_vilvl_h(tmp, tmp);
}
template <>
EIGEN_STRONG_INLINE Packet4i ploaddup<Packet4i>(const int32_t* from) {
  Packet4i tmp = pload<Packet4i>(from);
  return __lsx_vilvl_w(tmp, tmp);
}
template <>
EIGEN_STRONG_INLINE Packet2l ploaddup<Packet2l>(const int64_t* from) {
  return pset1<Packet2l>(from[0]);
}
template <>
EIGEN_STRONG_INLINE Packet16uc ploaddup<Packet16uc>(const uint8_t* from) {
  Packet16uc tmp = pload<Packet16uc>(from);
  return __lsx_vilvl_b(tmp, tmp);
}
template <>
EIGEN_STRONG_INLINE Packet8us ploaddup<Packet8us>(const uint16_t* from) {
  Packet8us tmp = pload<Packet8us>(from);
  return __lsx_vilvl_h(tmp, tmp);
}
template <>
EIGEN_STRONG_INLINE Packet4ui ploaddup<Packet4ui>(const uint32_t* from) {
  Packet4ui tmp = pload<Packet4ui>(from);
  return __lsx_vilvl_w(tmp, tmp);
}
template <>
EIGEN_STRONG_INLINE Packet2ul ploaddup<Packet2ul>(const uint64_t* from) {
  return pset1<Packet2ul>(from[0]);
}

template <>
EIGEN_STRONG_INLINE void pstore<float>(float* to, const Packet4f& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst(from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<double>(double* to, const Packet2d& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst(from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<int8_t>(int8_t* to, const Packet16c& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<int16_t>(int16_t* to, const Packet8s& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<int32_t>(int32_t* to, const Packet4i& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<int64_t>(int64_t* to, const Packet2l& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<uint8_t>(uint8_t* to, const Packet16uc& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<uint16_t>(uint16_t* to, const Packet8us& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<uint32_t>(uint32_t* to, const Packet4ui& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstore<uint64_t>(uint64_t* to, const Packet2ul& from) {
  EIGEN_DEBUG_ALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}

template <>
EIGEN_STRONG_INLINE void pstoreu<float>(float* to, const Packet4f& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst(from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<double>(double* to, const Packet2d& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst(from, to, 0);
}

template <>
EIGEN_STRONG_INLINE void pstoreu<int8_t>(int8_t* to, const Packet16c& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<int16_t>(int16_t* to, const Packet8s& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<int32_t>(int32_t* to, const Packet4i& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<int64_t>(int64_t* to, const Packet2l& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<uint8_t>(uint8_t* to, const Packet16uc& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<uint16_t>(uint16_t* to, const Packet8us& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<uint32_t>(uint32_t* to, const Packet4ui& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}
template <>
EIGEN_STRONG_INLINE void pstoreu<uint64_t>(uint64_t* to, const Packet2ul& from) {
  EIGEN_DEBUG_UNALIGNED_STORE __lsx_vst((__m128i)from, to, 0);
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet4f pgather<float, Packet4f>(const float* from, Index stride) {
  Packet4f v = {from[0], from[stride], from[2 * stride], from[3 * stride]};
  return v;
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet2d pgather<double, Packet2d>(const double* from, Index stride) {
  Packet2d v = {from[0], from[stride]};
  return v;
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet16c pgather<int8_t, Packet16c>(const int8_t* from, Index stride) {
  int8_t v[16] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  v[2] = from[2 * stride];
  v[3] = from[3 * stride];
  v[4] = from[4 * stride];
  v[5] = from[5 * stride];
  v[6] = from[6 * stride];
  v[7] = from[7 * stride];
  v[8] = from[8 * stride];
  v[9] = from[9 * stride];
  v[10] = from[10 * stride];
  v[11] = from[11 * stride];
  v[12] = from[12 * stride];
  v[13] = from[13 * stride];
  v[14] = from[14 * stride];
  v[15] = from[15 * stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet8s pgather<int16_t, Packet8s>(const int16_t* from, Index stride) {
  int16_t v[8] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  v[2] = from[2 * stride];
  v[3] = from[3 * stride];
  v[4] = from[4 * stride];
  v[5] = from[5 * stride];
  v[6] = from[6 * stride];
  v[7] = from[7 * stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet4i pgather<int32_t, Packet4i>(const int32_t* from, Index stride) {
  int32_t v[4] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  v[2] = from[2 * stride];
  v[3] = from[3 * stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet2l pgather<int64_t, Packet2l>(const int64_t* from, Index stride) {
  int64_t v[2] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet16uc pgather<uint8_t, Packet16uc>(const uint8_t* from, Index stride) {
  uint8_t v[16] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  v[2] = from[2 * stride];
  v[3] = from[3 * stride];
  v[4] = from[4 * stride];
  v[5] = from[5 * stride];
  v[6] = from[6 * stride];
  v[7] = from[7 * stride];
  v[8] = from[8 * stride];
  v[9] = from[9 * stride];
  v[10] = from[10 * stride];
  v[11] = from[11 * stride];
  v[12] = from[12 * stride];
  v[13] = from[13 * stride];
  v[14] = from[14 * stride];
  v[15] = from[15 * stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet8us pgather<uint16_t, Packet8us>(const uint16_t* from, Index stride) {
  uint16_t v[8] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  v[2] = from[2 * stride];
  v[3] = from[3 * stride];
  v[4] = from[4 * stride];
  v[5] = from[5 * stride];
  v[6] = from[6 * stride];
  v[7] = from[7 * stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet4ui pgather<uint32_t, Packet4ui>(const uint32_t* from, Index stride) {
  uint32_t v[4] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  v[2] = from[2 * stride];
  v[3] = from[3 * stride];
  return __lsx_vld(v, 0);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet2ul pgather<uint64_t, Packet2ul>(const uint64_t* from, Index stride) {
  uint64_t v[2] __attribute__((aligned(16)));
  v[0] = from[0];
  v[1] = from[stride];
  return __lsx_vld(v, 0);
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<float, Packet4f>(float* to, const Packet4f& from, Index stride) {
  __lsx_vstelm_w(from, to, 0, 0);
  __lsx_vstelm_w(from, to + stride * 1, 0, 1);
  __lsx_vstelm_w(from, to + stride * 2, 0, 2);
  __lsx_vstelm_w(from, to + stride * 3, 0, 3);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<double, Packet2d>(double* to, const Packet2d& from, Index stride) {
  __lsx_vstelm_d(from, to, 0, 0);
  __lsx_vstelm_d(from, to + stride, 0, 1);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<int8_t, Packet16c>(int8_t* to, const Packet16c& from,
                                                                       Index stride) {
  __lsx_vstelm_b((__m128i)from, to, 0, 0);
  __lsx_vstelm_b((__m128i)from, to + stride * 1, 0, 1);
  __lsx_vstelm_b((__m128i)from, to + stride * 2, 0, 2);
  __lsx_vstelm_b((__m128i)from, to + stride * 3, 0, 3);
  __lsx_vstelm_b((__m128i)from, to + stride * 4, 0, 4);
  __lsx_vstelm_b((__m128i)from, to + stride * 5, 0, 5);
  __lsx_vstelm_b((__m128i)from, to + stride * 6, 0, 6);
  __lsx_vstelm_b((__m128i)from, to + stride * 7, 0, 7);
  __lsx_vstelm_b((__m128i)from, to + stride * 8, 0, 8);
  __lsx_vstelm_b((__m128i)from, to + stride * 9, 0, 9);
  __lsx_vstelm_b((__m128i)from, to + stride * 10, 0, 10);
  __lsx_vstelm_b((__m128i)from, to + stride * 11, 0, 11);
  __lsx_vstelm_b((__m128i)from, to + stride * 12, 0, 12);
  __lsx_vstelm_b((__m128i)from, to + stride * 13, 0, 13);
  __lsx_vstelm_b((__m128i)from, to + stride * 14, 0, 14);
  __lsx_vstelm_b((__m128i)from, to + stride * 15, 0, 15);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<int16_t, Packet8s>(int16_t* to, const Packet8s& from,
                                                                       Index stride) {
  __lsx_vstelm_h((__m128i)from, to, 0, 0);
  __lsx_vstelm_h((__m128i)from, to + stride * 1, 0, 1);
  __lsx_vstelm_h((__m128i)from, to + stride * 2, 0, 2);
  __lsx_vstelm_h((__m128i)from, to + stride * 3, 0, 3);
  __lsx_vstelm_h((__m128i)from, to + stride * 4, 0, 4);
  __lsx_vstelm_h((__m128i)from, to + stride * 5, 0, 5);
  __lsx_vstelm_h((__m128i)from, to + stride * 6, 0, 6);
  __lsx_vstelm_h((__m128i)from, to + stride * 7, 0, 7);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<int32_t, Packet4i>(int32_t* to, const Packet4i& from,
                                                                       Index stride) {
  __lsx_vstelm_w((__m128i)from, to, 0, 0);
  __lsx_vstelm_w((__m128i)from, to + stride * 1, 0, 1);
  __lsx_vstelm_w((__m128i)from, to + stride * 2, 0, 2);
  __lsx_vstelm_w((__m128i)from, to + stride * 3, 0, 3);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<int64_t, Packet2l>(int64_t* to, const Packet2l& from,
                                                                       Index stride) {
  __lsx_vstelm_d((__m128i)from, to, 0, 0);
  __lsx_vstelm_d((__m128i)from, to + stride * 1, 0, 1);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<uint8_t, Packet16uc>(uint8_t* to, const Packet16uc& from,
                                                                         Index stride) {
  __lsx_vstelm_b((__m128i)from, to, 0, 0);
  __lsx_vstelm_b((__m128i)from, to + stride * 1, 0, 1);
  __lsx_vstelm_b((__m128i)from, to + stride * 2, 0, 2);
  __lsx_vstelm_b((__m128i)from, to + stride * 3, 0, 3);
  __lsx_vstelm_b((__m128i)from, to + stride * 4, 0, 4);
  __lsx_vstelm_b((__m128i)from, to + stride * 5, 0, 5);
  __lsx_vstelm_b((__m128i)from, to + stride * 6, 0, 6);
  __lsx_vstelm_b((__m128i)from, to + stride * 7, 0, 7);
  __lsx_vstelm_b((__m128i)from, to + stride * 8, 0, 8);
  __lsx_vstelm_b((__m128i)from, to + stride * 9, 0, 9);
  __lsx_vstelm_b((__m128i)from, to + stride * 10, 0, 10);
  __lsx_vstelm_b((__m128i)from, to + stride * 11, 0, 11);
  __lsx_vstelm_b((__m128i)from, to + stride * 12, 0, 12);
  __lsx_vstelm_b((__m128i)from, to + stride * 13, 0, 13);
  __lsx_vstelm_b((__m128i)from, to + stride * 14, 0, 14);
  __lsx_vstelm_b((__m128i)from, to + stride * 15, 0, 15);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<uint16_t, Packet8us>(uint16_t* to, const Packet8us& from,
                                                                         Index stride) {
  __lsx_vstelm_h((__m128i)from, to, 0, 0);
  __lsx_vstelm_h((__m128i)from, to + stride * 1, 0, 1);
  __lsx_vstelm_h((__m128i)from, to + stride * 2, 0, 2);
  __lsx_vstelm_h((__m128i)from, to + stride * 3, 0, 3);
  __lsx_vstelm_h((__m128i)from, to + stride * 4, 0, 4);
  __lsx_vstelm_h((__m128i)from, to + stride * 5, 0, 5);
  __lsx_vstelm_h((__m128i)from, to + stride * 6, 0, 6);
  __lsx_vstelm_h((__m128i)from, to + stride * 7, 0, 7);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<uint32_t, Packet4ui>(uint32_t* to, const Packet4ui& from,
                                                                         Index stride) {
  __lsx_vstelm_w((__m128i)from, to, 0, 0);
  __lsx_vstelm_w((__m128i)from, to + stride * 1, 0, 1);
  __lsx_vstelm_w((__m128i)from, to + stride * 2, 0, 2);
  __lsx_vstelm_w((__m128i)from, to + stride * 3, 0, 3);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void pscatter<uint64_t, Packet2ul>(uint64_t* to, const Packet2ul& from,
                                                                         Index stride) {
  __lsx_vstelm_d((__m128i)from, to, 0, 0);
  __lsx_vstelm_d((__m128i)from, to + stride * 1, 0, 1);
}

template <>
EIGEN_STRONG_INLINE void prefetch<float>(const float* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<double>(const double* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<int8_t>(const int8_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<int16_t>(const int16_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<int32_t>(const int32_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<int64_t>(const int64_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<uint8_t>(const uint8_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<uint16_t>(const uint16_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<uint32_t>(const uint32_t* addr) {
  __builtin_prefetch(addr);
}
template <>
EIGEN_STRONG_INLINE void prefetch<uint64_t>(const uint64_t* addr) {
  __builtin_prefetch(addr);
}

template <>
EIGEN_STRONG_INLINE float pfirst<Packet4f>(const Packet4f& a) {
  float v;
  __lsx_vstelm_w(a, &v, 0, 0);
  return v;
}
template <>
EIGEN_STRONG_INLINE double pfirst<Packet2d>(const Packet2d& a) {
  double v;
  __lsx_vstelm_d(a, &v, 0, 0);
  return v;
}

template <>
EIGEN_STRONG_INLINE int8_t pfirst<Packet16c>(const Packet16c& a) {
  return (int8_t)__lsx_vpickve2gr_b((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE int16_t pfirst<Packet8s>(const Packet8s& a) {
  return (int16_t)__lsx_vpickve2gr_h((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE int32_t pfirst<Packet4i>(const Packet4i& a) {
  return __lsx_vpickve2gr_w((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE int64_t pfirst<Packet2l>(const Packet2l& a) {
  return __lsx_vpickve2gr_d((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE uint8_t pfirst<Packet16uc>(const Packet16uc& a) {
  return (uint8_t)__lsx_vpickve2gr_bu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE uint16_t pfirst<Packet8us>(const Packet8us& a) {
  return (uint16_t)__lsx_vpickve2gr_hu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE uint32_t pfirst<Packet4ui>(const Packet4ui& a) {
  return __lsx_vpickve2gr_wu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE uint64_t pfirst<Packet2ul>(const Packet2ul& a) {
  return __lsx_vpickve2gr_du((__m128i)a, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f preverse(const Packet4f& a) {
  return (Packet4f)__lsx_vshuf4i_w(a, 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet2d preverse(const Packet2d& a) {
  return (Packet2d)__lsx_vshuf4i_d(a, a, 0x1);
}
template <>
EIGEN_STRONG_INLINE Packet16c preverse(const Packet16c& a) {
  return __lsx_vshuf4i_b(__lsx_vshuf4i_w((__m128i)a, 0x1B), 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet8s preverse(const Packet8s& a) {
  return __lsx_vshuf4i_h(__lsx_vshuf4i_d((__m128i)a, (__m128i)a, 0x1), 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet4i preverse(const Packet4i& a) {
  return __lsx_vshuf4i_w((__m128i)a, 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet2l preverse(const Packet2l& a) {
  return __lsx_vshuf4i_d((__m128i)a, (__m128i)a, 0x1);
}
template <>
EIGEN_STRONG_INLINE Packet16uc preverse(const Packet16uc& a) {
  return __lsx_vshuf4i_b(__lsx_vshuf4i_w((__m128i)a, 0x1B), 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet8us preverse(const Packet8us& a) {
  return __lsx_vshuf4i_h(__lsx_vshuf4i_d((__m128i)a, (__m128i)a, 0x1), 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet4ui preverse(const Packet4ui& a) {
  return __lsx_vshuf4i_w((__m128i)a, 0x1B);
}
template <>
EIGEN_STRONG_INLINE Packet2ul preverse(const Packet2ul& a) {
  return __lsx_vshuf4i_d((__m128i)a, (__m128i)a, 0x1);
}

template <>
EIGEN_STRONG_INLINE float predux<Packet4f>(const Packet4f& a) {
  Packet4f tmp = __lsx_vfadd_s(a, vec4f_swizzle1(a, 2, 3, 2, 3));
  return pfirst<Packet4f>(__lsx_vfadd_s(tmp, vec4f_swizzle1(tmp, 1, 1, 1, 1)));
}
template <>
EIGEN_STRONG_INLINE double predux<Packet2d>(const Packet2d& a) {
  return pfirst<Packet2d>(__lsx_vfadd_d(a, preverse(a)));
}
template <>
EIGEN_STRONG_INLINE int8_t predux<Packet16c>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vhaddw_h_b(a, a);
  Packet4i tmp2 = __lsx_vhaddw_w_h(tmp1, tmp1);
  Packet2l tmp3 = __lsx_vhaddw_d_w(tmp2, tmp2);
  return (int8_t)__lsx_vpickve2gr_d(__lsx_vhaddw_q_d(tmp3, tmp3), 0);
}
template <>
EIGEN_STRONG_INLINE int16_t predux<Packet8s>(const Packet8s& a) {
  Packet4i tmp1 = __lsx_vhaddw_w_h(a, a);
  Packet2l tmp2 = __lsx_vhaddw_d_w(tmp1, tmp1);
  return (int16_t)__lsx_vpickve2gr_d(__lsx_vhaddw_q_d(tmp2, tmp2), 0);
}
template <>
EIGEN_STRONG_INLINE int32_t predux<Packet4i>(const Packet4i& a) {
  Packet2l tmp = __lsx_vhaddw_d_w(a, a);
  return (int32_t)__lsx_vpickve2gr_d(__lsx_vhaddw_q_d(tmp, tmp), 0);
}
template <>
EIGEN_STRONG_INLINE int64_t predux<Packet2l>(const Packet2l& a) {
  return (int64_t)__lsx_vpickve2gr_d(__lsx_vhaddw_q_d(a, a), 0);
}
template <>
EIGEN_STRONG_INLINE uint8_t predux<Packet16uc>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vhaddw_hu_bu(a, a);
  Packet4ui tmp2 = __lsx_vhaddw_wu_hu(tmp1, tmp1);
  Packet2ul tmp3 = __lsx_vhaddw_du_wu(tmp2, tmp2);
  return (uint8_t)__lsx_vpickve2gr_d(__lsx_vhaddw_qu_du(tmp3, tmp3), 0);
}
template <>
EIGEN_STRONG_INLINE uint16_t predux<Packet8us>(const Packet8us& a) {
  Packet4ui tmp1 = __lsx_vhaddw_wu_hu(a, a);
  Packet2ul tmp2 = __lsx_vhaddw_du_wu(tmp1, tmp1);
  return (uint16_t)__lsx_vpickve2gr_d(__lsx_vhaddw_qu_du(tmp2, tmp2), 0);
}
template <>
EIGEN_STRONG_INLINE uint32_t predux<Packet4ui>(const Packet4ui& a) {
  Packet2ul tmp = __lsx_vhaddw_du_wu(a, a);
  return (uint32_t)__lsx_vpickve2gr_d(__lsx_vhaddw_qu_du(tmp, tmp), 0);
}
template <>
EIGEN_STRONG_INLINE uint64_t predux<Packet2ul>(const Packet2ul& a) {
  return (uint64_t)__lsx_vpickve2gr_d(__lsx_vhaddw_qu_du(a, a), 0);
}

template <>
EIGEN_STRONG_INLINE float predux_mul<Packet4f>(const Packet4f& a) {
  Packet4f tmp = __lsx_vfmul_s(a, vec4f_swizzle1(a, 2, 3, 2, 3));
  return pfirst<Packet4f>(__lsx_vfmul_s(tmp, vec4f_swizzle1(tmp, 1, 1, 1, 1)));
}
template <>
EIGEN_STRONG_INLINE double predux_mul<Packet2d>(const Packet2d& a) {
  return pfirst<Packet2d>(__lsx_vfmul_d(a, preverse(a)));
}
template <>
EIGEN_STRONG_INLINE int8_t predux_mul<Packet16c>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vmulwev_h_b(a, preverse(a));
  Packet4i tmp2 = __lsx_vmulwev_w_h(tmp1, preverse(tmp1));
  Packet2l tmp3 = __lsx_vmulwev_d_w(tmp2, preverse(tmp2));
  return (int8_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(tmp3, preverse(tmp3)), 0);
}
template <>
EIGEN_STRONG_INLINE int16_t predux_mul<Packet8s>(const Packet8s& a) {
  Packet4i tmp1 = __lsx_vmulwev_w_h(a, preverse(a));
  Packet2l tmp2 = __lsx_vmulwev_d_w(tmp1, preverse(tmp1));
  return (int16_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(tmp2, preverse(tmp2)), 0);
}
template <>
EIGEN_STRONG_INLINE int32_t predux_mul<Packet4i>(const Packet4i& a) {
  Packet2l tmp = __lsx_vmulwev_d_w(a, preverse(a));
  return (int32_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(tmp, preverse(tmp)), 0);
}
template <>
EIGEN_STRONG_INLINE int64_t predux_mul<Packet2l>(const Packet2l& a) {
  return (int64_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(a, preverse(a)), 0);
}
template <>
EIGEN_STRONG_INLINE uint8_t predux_mul<Packet16uc>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vmulwev_h_bu(a, preverse(a));
  Packet4ui tmp2 = __lsx_vmulwev_w_h(tmp1, preverse(tmp1));
  Packet2ul tmp3 = __lsx_vmulwev_d_w(tmp2, preverse(tmp2));
  return (uint8_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(tmp3, preverse(tmp3)), 0);
}
template <>
EIGEN_STRONG_INLINE uint16_t predux_mul<Packet8us>(const Packet8us& a) {
  Packet4ui tmp1 = __lsx_vmulwev_w_hu(a, preverse(a));
  Packet2ul tmp2 = __lsx_vmulwev_d_w(tmp1, preverse(tmp1));
  return (uint16_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(tmp2, preverse(tmp2)), 0);
}
template <>
EIGEN_STRONG_INLINE uint32_t predux_mul<Packet4ui>(const Packet4ui& a) {
  Packet2ul tmp = __lsx_vmulwev_d_wu(a, preverse(a));
  return (uint32_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_d(tmp, preverse(tmp)), 0);
}
template <>
EIGEN_STRONG_INLINE uint64_t predux_mul<Packet2ul>(const Packet2ul& a) {
  return (uint64_t)__lsx_vpickve2gr_d(__lsx_vmulwev_q_du(a, preverse(a)), 0);
}

template <>
EIGEN_STRONG_INLINE float predux_min<Packet4f>(const Packet4f& a) {
  Packet4f tmp = __lsx_vfmin_s(a, (Packet4f)__lsx_vshuf4i_w(a, 0x4E));
  return pfirst(__lsx_vfmin_s(tmp, (Packet4f)__lsx_vshuf4i_w(tmp, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE double predux_min<Packet2d>(const Packet2d& a) {
  return pfirst(__lsx_vfmin_d(a, preverse(a)));
}
template <>
EIGEN_STRONG_INLINE int8_t predux_min<Packet16c>(const Packet16c& a) {
  Packet16c tmp1 = __lsx_vmin_b(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet16c tmp2 = __lsx_vmin_b(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  Packet16c tmp3 = __lsx_vmin_b(tmp2, __lsx_vshuf4i_b((__m128i)tmp2, 0x4E));
  return pfirst((Packet16c)__lsx_vmin_b(tmp3, __lsx_vshuf4i_b((__m128i)tmp3, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE int16_t predux_min<Packet8s>(const Packet8s& a) {
  Packet8s tmp1 = __lsx_vmin_h(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet8s tmp2 = __lsx_vmin_h(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  return pfirst((Packet8s)__lsx_vmin_h(tmp2, __lsx_vshuf4i_h((__m128i)tmp2, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE int32_t predux_min<Packet4i>(const Packet4i& a) {
  Packet4i tmp = __lsx_vmin_w(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  return pfirst((Packet4i)__lsx_vmin_w(tmp, __lsx_vshuf4i_w((__m128i)tmp, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE int64_t predux_min<Packet2l>(const Packet2l& a) {
  return pfirst((Packet2l)__lsx_vmin_d(a, preverse(a)));
}
template <>
EIGEN_STRONG_INLINE uint8_t predux_min<Packet16uc>(const Packet16uc& a) {
  Packet16uc tmp1 = __lsx_vmin_bu(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet16uc tmp2 = __lsx_vmin_bu(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  Packet16uc tmp3 = __lsx_vmin_bu(tmp2, __lsx_vshuf4i_b((__m128i)tmp2, 0x4E));
  return pfirst((Packet16uc)__lsx_vmin_bu(tmp3, __lsx_vshuf4i_b((__m128i)tmp3, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE uint16_t predux_min<Packet8us>(const Packet8us& a) {
  Packet8us tmp1 = __lsx_vmin_hu(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet8us tmp2 = __lsx_vmin_hu(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  return pfirst((Packet8us)__lsx_vmin_hu(tmp2, __lsx_vshuf4i_h((__m128i)tmp2, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE uint32_t predux_min<Packet4ui>(const Packet4ui& a) {
  Packet4ui tmp = __lsx_vmin_wu(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  return pfirst((Packet4ui)__lsx_vmin_wu(tmp, __lsx_vshuf4i_w((__m128i)tmp, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE uint64_t predux_min<Packet2ul>(const Packet2ul& a) {
  return pfirst((Packet2ul)__lsx_vmin_du(a, preverse(a)));
}

template <>
EIGEN_STRONG_INLINE float predux_max<Packet4f>(const Packet4f& a) {
  Packet4f tmp = __lsx_vfmax_s(a, (Packet4f)__lsx_vshuf4i_w(a, 0x4E));
  return pfirst(__lsx_vfmax_s(tmp, (Packet4f)__lsx_vshuf4i_w(tmp, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE double predux_max<Packet2d>(const Packet2d& a) {
  return pfirst(__lsx_vfmax_d(a, preverse(a)));
}
template <>
EIGEN_STRONG_INLINE int8_t predux_max<Packet16c>(const Packet16c& a) {
  Packet16c tmp1 = __lsx_vmax_b(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet16c tmp2 = __lsx_vmax_b(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  Packet16c tmp3 = __lsx_vmax_b(tmp2, __lsx_vshuf4i_b((__m128i)tmp2, 0x4E));
  return pfirst((Packet16c)__lsx_vmax_b(tmp3, __lsx_vshuf4i_b((__m128i)tmp3, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE int16_t predux_max<Packet8s>(const Packet8s& a) {
  Packet8s tmp1 = __lsx_vmax_h(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet8s tmp2 = __lsx_vmax_h(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  return pfirst((Packet8s)__lsx_vmax_h(tmp2, __lsx_vshuf4i_h((__m128i)tmp2, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE int32_t predux_max<Packet4i>(const Packet4i& a) {
  Packet4i tmp = __lsx_vmax_w(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  return pfirst((Packet4i)__lsx_vmax_w(tmp, __lsx_vshuf4i_w((__m128i)tmp, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE int64_t predux_max<Packet2l>(const Packet2l& a) {
  return pfirst((Packet2l)__lsx_vmax_d(a, preverse(a)));
}
template <>
EIGEN_STRONG_INLINE uint8_t predux_max<Packet16uc>(const Packet16uc& a) {
  Packet16uc tmp1 = __lsx_vmax_bu(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet16uc tmp2 = __lsx_vmax_bu(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  Packet16uc tmp3 = __lsx_vmax_bu(tmp2, __lsx_vshuf4i_b((__m128i)tmp2, 0x4E));
  return pfirst((Packet16uc)__lsx_vmax_bu(tmp3, __lsx_vshuf4i_b((__m128i)tmp3, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE uint16_t predux_max<Packet8us>(const Packet8us& a) {
  Packet8us tmp1 = __lsx_vmax_hu(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  Packet8us tmp2 = __lsx_vmax_hu(tmp1, __lsx_vshuf4i_h((__m128i)tmp1, 0x4E));
  return pfirst((Packet8us)__lsx_vmax_hu(tmp2, __lsx_vshuf4i_h((__m128i)tmp2, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE uint32_t predux_max<Packet4ui>(const Packet4ui& a) {
  Packet4ui tmp = __lsx_vmax_wu(a, __lsx_vshuf4i_w((__m128i)a, 0x4E));
  return pfirst((Packet4ui)__lsx_vmax_wu(tmp, __lsx_vshuf4i_w((__m128i)tmp, 0xB1)));
}
template <>
EIGEN_STRONG_INLINE uint64_t predux_max<Packet2ul>(const Packet2ul& a) {
  return pfirst((Packet2ul)__lsx_vmax_du(a, preverse(a)));
}

template <>
EIGEN_STRONG_INLINE Packet4f psqrt(const Packet4f& a) {
  return __lsx_vfsqrt_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d psqrt(const Packet2d& a) {
  return __lsx_vfsqrt_d(a);
}

EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet4f, 4>& kernel) {
  Packet4f T0 = (Packet4f)__lsx_vilvl_w((__m128i)kernel.packet[1], (__m128i)kernel.packet[0]);
  Packet4f T1 = (Packet4f)__lsx_vilvh_w((__m128i)kernel.packet[1], (__m128i)kernel.packet[0]);
  Packet4f T2 = (Packet4f)__lsx_vilvl_w((__m128i)kernel.packet[3], (__m128i)kernel.packet[2]);
  Packet4f T3 = (Packet4f)__lsx_vilvh_w((__m128i)kernel.packet[3], (__m128i)kernel.packet[2]);

  kernel.packet[0] = (Packet4f)__lsx_vilvl_d((__m128i)T2, (__m128i)T0);
  kernel.packet[1] = (Packet4f)__lsx_vilvh_d((__m128i)T2, (__m128i)T0);
  kernel.packet[2] = (Packet4f)__lsx_vilvl_d((__m128i)T3, (__m128i)T1);
  kernel.packet[3] = (Packet4f)__lsx_vilvh_d((__m128i)T3, (__m128i)T1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet2d, 2>& kernel) {
  Packet2d tmp = (Packet2d)__lsx_vilvh_d((__m128i)kernel.packet[1], (__m128i)kernel.packet[0]);
  kernel.packet[0] = (Packet2d)__lsx_vilvl_d((__m128i)kernel.packet[1], (__m128i)kernel.packet[0]);
  kernel.packet[1] = tmp;
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16c, 16>& kernel) {
  __m128i t0 = __lsx_vilvl_b(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_b(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_b(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_b(kernel.packet[3], kernel.packet[2]);
  __m128i t4 = __lsx_vilvl_b(kernel.packet[5], kernel.packet[4]);
  __m128i t5 = __lsx_vilvh_b(kernel.packet[5], kernel.packet[4]);
  __m128i t6 = __lsx_vilvl_b(kernel.packet[7], kernel.packet[6]);
  __m128i t7 = __lsx_vilvh_b(kernel.packet[7], kernel.packet[6]);
  __m128i t8 = __lsx_vilvl_b(kernel.packet[9], kernel.packet[8]);
  __m128i t9 = __lsx_vilvh_b(kernel.packet[9], kernel.packet[8]);
  __m128i ta = __lsx_vilvl_b(kernel.packet[11], kernel.packet[10]);
  __m128i tb = __lsx_vilvh_b(kernel.packet[11], kernel.packet[10]);
  __m128i tc = __lsx_vilvl_b(kernel.packet[13], kernel.packet[12]);
  __m128i td = __lsx_vilvh_b(kernel.packet[13], kernel.packet[12]);
  __m128i te = __lsx_vilvl_b(kernel.packet[15], kernel.packet[14]);
  __m128i tf = __lsx_vilvh_b(kernel.packet[15], kernel.packet[14]);

  __m128i s0 = __lsx_vilvl_h(t2, t0);
  __m128i s1 = __lsx_vilvh_h(t2, t0);
  __m128i s2 = __lsx_vilvl_h(t3, t1);
  __m128i s3 = __lsx_vilvh_h(t3, t1);
  __m128i s4 = __lsx_vilvl_h(t6, t4);
  __m128i s5 = __lsx_vilvh_h(t6, t4);
  __m128i s6 = __lsx_vilvl_h(t7, t5);
  __m128i s7 = __lsx_vilvh_h(t7, t5);
  __m128i s8 = __lsx_vilvl_h(ta, t8);
  __m128i s9 = __lsx_vilvh_h(ta, t8);
  __m128i sa = __lsx_vilvl_h(tb, t9);
  __m128i sb = __lsx_vilvh_h(tb, t9);
  __m128i sc = __lsx_vilvl_h(te, tc);
  __m128i sd = __lsx_vilvh_h(te, tc);
  __m128i se = __lsx_vilvl_h(tf, td);
  __m128i sf = __lsx_vilvh_h(tf, td);

  __m128i u0 = __lsx_vilvl_w(s4, s0);
  __m128i u1 = __lsx_vilvh_w(s4, s0);
  __m128i u2 = __lsx_vilvl_w(s5, s1);
  __m128i u3 = __lsx_vilvh_w(s5, s1);
  __m128i u4 = __lsx_vilvl_w(s6, s2);
  __m128i u5 = __lsx_vilvh_w(s6, s2);
  __m128i u6 = __lsx_vilvl_w(s7, s3);
  __m128i u7 = __lsx_vilvh_w(s7, s3);
  __m128i u8 = __lsx_vilvl_w(sc, s8);
  __m128i u9 = __lsx_vilvh_w(sc, s8);
  __m128i ua = __lsx_vilvl_w(sd, s9);
  __m128i ub = __lsx_vilvh_w(sd, s9);
  __m128i uc = __lsx_vilvl_w(se, sa);
  __m128i ud = __lsx_vilvh_w(se, sa);
  __m128i ue = __lsx_vilvl_w(sf, sb);
  __m128i uf = __lsx_vilvh_w(sf, sb);

  kernel.packet[0] = __lsx_vilvl_d(u8, u0);
  kernel.packet[1] = __lsx_vilvh_d(u8, u0);
  kernel.packet[2] = __lsx_vilvl_d(u9, u1);
  kernel.packet[3] = __lsx_vilvh_d(u9, u1);
  kernel.packet[4] = __lsx_vilvl_d(ua, u2);
  kernel.packet[5] = __lsx_vilvh_d(ua, u2);
  kernel.packet[6] = __lsx_vilvl_d(ub, u3);
  kernel.packet[7] = __lsx_vilvh_d(ub, u3);
  kernel.packet[8] = __lsx_vilvl_d(uc, u4);
  kernel.packet[9] = __lsx_vilvh_d(uc, u4);
  kernel.packet[10] = __lsx_vilvl_d(ud, u5);
  kernel.packet[11] = __lsx_vilvh_d(ud, u5);
  kernel.packet[12] = __lsx_vilvl_d(ue, u6);
  kernel.packet[13] = __lsx_vilvh_d(ue, u6);
  kernel.packet[14] = __lsx_vilvl_d(uf, u7);
  kernel.packet[15] = __lsx_vilvh_d(uf, u7);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16c, 8>& kernel) {
  __m128i t0 = __lsx_vilvl_b(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_b(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_b(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_b(kernel.packet[3], kernel.packet[2]);
  __m128i t4 = __lsx_vilvl_b(kernel.packet[5], kernel.packet[4]);
  __m128i t5 = __lsx_vilvh_b(kernel.packet[5], kernel.packet[4]);
  __m128i t6 = __lsx_vilvl_b(kernel.packet[7], kernel.packet[6]);
  __m128i t7 = __lsx_vilvh_b(kernel.packet[7], kernel.packet[6]);

  __m128i s0 = __lsx_vilvl_h(t2, t0);
  __m128i s1 = __lsx_vilvh_h(t2, t0);
  __m128i s2 = __lsx_vilvl_h(t3, t1);
  __m128i s3 = __lsx_vilvh_h(t3, t1);
  __m128i s4 = __lsx_vilvl_h(t6, t4);
  __m128i s5 = __lsx_vilvh_h(t6, t4);
  __m128i s6 = __lsx_vilvl_h(t7, t5);
  __m128i s7 = __lsx_vilvh_h(t7, t5);

  kernel.packet[0] = __lsx_vilvl_w(s4, s0);
  kernel.packet[1] = __lsx_vilvh_w(s4, s0);
  kernel.packet[2] = __lsx_vilvl_w(s5, s1);
  kernel.packet[3] = __lsx_vilvh_w(s5, s1);
  kernel.packet[4] = __lsx_vilvl_w(s6, s2);
  kernel.packet[5] = __lsx_vilvh_w(s6, s2);
  kernel.packet[6] = __lsx_vilvl_w(s7, s3);
  kernel.packet[7] = __lsx_vilvh_w(s7, s3);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16c, 4>& kernel) {
  __m128i t0 = __lsx_vilvl_b(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_b(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_b(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_b(kernel.packet[3], kernel.packet[2]);

  kernel.packet[0] = __lsx_vilvl_h(t2, t0);
  kernel.packet[1] = __lsx_vilvh_h(t2, t0);
  kernel.packet[2] = __lsx_vilvl_h(t3, t1);
  kernel.packet[3] = __lsx_vilvh_h(t3, t1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet8s, 8>& kernel) {
  __m128i t0 = __lsx_vilvl_h(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_h(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_h(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_h(kernel.packet[3], kernel.packet[2]);
  __m128i t4 = __lsx_vilvl_h(kernel.packet[5], kernel.packet[4]);
  __m128i t5 = __lsx_vilvh_h(kernel.packet[5], kernel.packet[4]);
  __m128i t6 = __lsx_vilvl_h(kernel.packet[7], kernel.packet[6]);
  __m128i t7 = __lsx_vilvh_h(kernel.packet[7], kernel.packet[6]);

  __m128i s0 = __lsx_vilvl_w(t2, t0);
  __m128i s1 = __lsx_vilvh_w(t2, t0);
  __m128i s2 = __lsx_vilvl_w(t3, t1);
  __m128i s3 = __lsx_vilvh_w(t3, t1);
  __m128i s4 = __lsx_vilvl_w(t6, t4);
  __m128i s5 = __lsx_vilvh_w(t6, t4);
  __m128i s6 = __lsx_vilvl_w(t7, t5);
  __m128i s7 = __lsx_vilvh_w(t7, t5);

  kernel.packet[0] = __lsx_vilvl_d(s4, s0);
  kernel.packet[1] = __lsx_vilvh_d(s4, s0);
  kernel.packet[2] = __lsx_vilvl_d(s5, s1);
  kernel.packet[3] = __lsx_vilvh_d(s5, s1);
  kernel.packet[4] = __lsx_vilvl_d(s6, s2);
  kernel.packet[5] = __lsx_vilvh_d(s6, s2);
  kernel.packet[6] = __lsx_vilvl_d(s7, s3);
  kernel.packet[7] = __lsx_vilvh_d(s7, s3);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet8s, 4>& kernel) {
  __m128i t0 = __lsx_vilvl_h(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_h(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_h(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_h(kernel.packet[3], kernel.packet[2]);

  kernel.packet[0] = __lsx_vilvl_w(t2, t0);
  kernel.packet[1] = __lsx_vilvh_w(t2, t0);
  kernel.packet[2] = __lsx_vilvl_w(t3, t1);
  kernel.packet[3] = __lsx_vilvh_w(t3, t1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet4i, 4>& kernel) {
  __m128i T0 = __lsx_vilvl_w(kernel.packet[1], kernel.packet[0]);
  __m128i T1 = __lsx_vilvh_w(kernel.packet[1], kernel.packet[0]);
  __m128i T2 = __lsx_vilvl_w(kernel.packet[3], kernel.packet[2]);
  __m128i T3 = __lsx_vilvh_w(kernel.packet[3], kernel.packet[2]);

  kernel.packet[0] = __lsx_vilvl_d(T2, T0);
  kernel.packet[1] = __lsx_vilvh_d(T2, T0);
  kernel.packet[2] = __lsx_vilvl_d(T3, T1);
  kernel.packet[3] = __lsx_vilvh_d(T3, T1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet2l, 2>& kernel) {
  __m128i tmp = __lsx_vilvh_d(kernel.packet[1], kernel.packet[0]);
  kernel.packet[0] = __lsx_vilvl_d(kernel.packet[1], kernel.packet[0]);
  kernel.packet[1] = tmp;
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16uc, 16>& kernel) {
  __m128i t0 = __lsx_vilvl_b(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_b(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_b(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_b(kernel.packet[3], kernel.packet[2]);
  __m128i t4 = __lsx_vilvl_b(kernel.packet[5], kernel.packet[4]);
  __m128i t5 = __lsx_vilvh_b(kernel.packet[5], kernel.packet[4]);
  __m128i t6 = __lsx_vilvl_b(kernel.packet[7], kernel.packet[6]);
  __m128i t7 = __lsx_vilvh_b(kernel.packet[7], kernel.packet[6]);
  __m128i t8 = __lsx_vilvl_b(kernel.packet[9], kernel.packet[8]);
  __m128i t9 = __lsx_vilvh_b(kernel.packet[9], kernel.packet[8]);
  __m128i ta = __lsx_vilvl_b(kernel.packet[11], kernel.packet[10]);
  __m128i tb = __lsx_vilvh_b(kernel.packet[11], kernel.packet[10]);
  __m128i tc = __lsx_vilvl_b(kernel.packet[13], kernel.packet[12]);
  __m128i td = __lsx_vilvh_b(kernel.packet[13], kernel.packet[12]);
  __m128i te = __lsx_vilvl_b(kernel.packet[15], kernel.packet[14]);
  __m128i tf = __lsx_vilvh_b(kernel.packet[15], kernel.packet[14]);

  __m128i s0 = __lsx_vilvl_h(t2, t0);
  __m128i s1 = __lsx_vilvh_h(t2, t0);
  __m128i s2 = __lsx_vilvl_h(t3, t1);
  __m128i s3 = __lsx_vilvh_h(t3, t1);
  __m128i s4 = __lsx_vilvl_h(t6, t4);
  __m128i s5 = __lsx_vilvh_h(t6, t4);
  __m128i s6 = __lsx_vilvl_h(t7, t5);
  __m128i s7 = __lsx_vilvh_h(t7, t5);
  __m128i s8 = __lsx_vilvl_h(ta, t8);
  __m128i s9 = __lsx_vilvh_h(ta, t8);
  __m128i sa = __lsx_vilvl_h(tb, t9);
  __m128i sb = __lsx_vilvh_h(tb, t9);
  __m128i sc = __lsx_vilvl_h(te, tc);
  __m128i sd = __lsx_vilvh_h(te, tc);
  __m128i se = __lsx_vilvl_h(tf, td);
  __m128i sf = __lsx_vilvh_h(tf, td);

  __m128i u0 = __lsx_vilvl_w(s4, s0);
  __m128i u1 = __lsx_vilvh_w(s4, s0);
  __m128i u2 = __lsx_vilvl_w(s5, s1);
  __m128i u3 = __lsx_vilvh_w(s5, s1);
  __m128i u4 = __lsx_vilvl_w(s6, s2);
  __m128i u5 = __lsx_vilvh_w(s6, s2);
  __m128i u6 = __lsx_vilvl_w(s7, s3);
  __m128i u7 = __lsx_vilvh_w(s7, s3);
  __m128i u8 = __lsx_vilvl_w(sc, s8);
  __m128i u9 = __lsx_vilvh_w(sc, s8);
  __m128i ua = __lsx_vilvl_w(sd, s9);
  __m128i ub = __lsx_vilvh_w(sd, s9);
  __m128i uc = __lsx_vilvl_w(se, sa);
  __m128i ud = __lsx_vilvh_w(se, sa);
  __m128i ue = __lsx_vilvl_w(sf, sb);
  __m128i uf = __lsx_vilvh_w(sf, sb);

  kernel.packet[0] = __lsx_vilvl_d(u8, u0);
  kernel.packet[1] = __lsx_vilvh_d(u8, u0);
  kernel.packet[2] = __lsx_vilvl_d(u9, u1);
  kernel.packet[3] = __lsx_vilvh_d(u9, u1);
  kernel.packet[4] = __lsx_vilvl_d(ua, u2);
  kernel.packet[5] = __lsx_vilvh_d(ua, u2);
  kernel.packet[6] = __lsx_vilvl_d(ub, u3);
  kernel.packet[7] = __lsx_vilvh_d(ub, u3);
  kernel.packet[8] = __lsx_vilvl_d(uc, u4);
  kernel.packet[9] = __lsx_vilvh_d(uc, u4);
  kernel.packet[10] = __lsx_vilvl_d(ud, u5);
  kernel.packet[11] = __lsx_vilvh_d(ud, u5);
  kernel.packet[12] = __lsx_vilvl_d(ue, u6);
  kernel.packet[13] = __lsx_vilvh_d(ue, u6);
  kernel.packet[14] = __lsx_vilvl_d(uf, u7);
  kernel.packet[15] = __lsx_vilvh_d(uf, u7);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16uc, 8>& kernel) {
  __m128i t0 = __lsx_vilvl_b(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_b(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_b(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_b(kernel.packet[3], kernel.packet[2]);
  __m128i t4 = __lsx_vilvl_b(kernel.packet[5], kernel.packet[4]);
  __m128i t5 = __lsx_vilvh_b(kernel.packet[5], kernel.packet[4]);
  __m128i t6 = __lsx_vilvl_b(kernel.packet[7], kernel.packet[6]);
  __m128i t7 = __lsx_vilvh_b(kernel.packet[7], kernel.packet[6]);

  __m128i s0 = __lsx_vilvl_h(t2, t0);
  __m128i s1 = __lsx_vilvh_h(t2, t0);
  __m128i s2 = __lsx_vilvl_h(t3, t1);
  __m128i s3 = __lsx_vilvh_h(t3, t1);
  __m128i s4 = __lsx_vilvl_h(t6, t4);
  __m128i s5 = __lsx_vilvh_h(t6, t4);
  __m128i s6 = __lsx_vilvl_h(t7, t5);
  __m128i s7 = __lsx_vilvh_h(t7, t5);

  kernel.packet[0] = __lsx_vilvl_w(s4, s0);
  kernel.packet[1] = __lsx_vilvh_w(s4, s0);
  kernel.packet[2] = __lsx_vilvl_w(s5, s1);
  kernel.packet[3] = __lsx_vilvh_w(s5, s1);
  kernel.packet[4] = __lsx_vilvl_w(s6, s2);
  kernel.packet[5] = __lsx_vilvh_w(s6, s2);
  kernel.packet[6] = __lsx_vilvl_w(s7, s3);
  kernel.packet[7] = __lsx_vilvh_w(s7, s3);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16uc, 4>& kernel) {
  __m128i t0 = __lsx_vilvl_b(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_b(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_b(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_b(kernel.packet[3], kernel.packet[2]);

  kernel.packet[0] = __lsx_vilvl_h(t2, t0);
  kernel.packet[1] = __lsx_vilvh_h(t2, t0);
  kernel.packet[2] = __lsx_vilvl_h(t3, t1);
  kernel.packet[3] = __lsx_vilvh_h(t3, t1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet8us, 8>& kernel) {
  __m128i t0 = __lsx_vilvl_h(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_h(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_h(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_h(kernel.packet[3], kernel.packet[2]);
  __m128i t4 = __lsx_vilvl_h(kernel.packet[5], kernel.packet[4]);
  __m128i t5 = __lsx_vilvh_h(kernel.packet[5], kernel.packet[4]);
  __m128i t6 = __lsx_vilvl_h(kernel.packet[7], kernel.packet[6]);
  __m128i t7 = __lsx_vilvh_h(kernel.packet[7], kernel.packet[6]);

  __m128i s0 = __lsx_vilvl_w(t2, t0);
  __m128i s1 = __lsx_vilvh_w(t2, t0);
  __m128i s2 = __lsx_vilvl_w(t3, t1);
  __m128i s3 = __lsx_vilvh_w(t3, t1);
  __m128i s4 = __lsx_vilvl_w(t6, t4);
  __m128i s5 = __lsx_vilvh_w(t6, t4);
  __m128i s6 = __lsx_vilvl_w(t7, t5);
  __m128i s7 = __lsx_vilvh_w(t7, t5);

  kernel.packet[0] = __lsx_vilvl_d(s4, s0);
  kernel.packet[1] = __lsx_vilvh_d(s4, s0);
  kernel.packet[2] = __lsx_vilvl_d(s5, s1);
  kernel.packet[3] = __lsx_vilvh_d(s5, s1);
  kernel.packet[4] = __lsx_vilvl_d(s6, s2);
  kernel.packet[5] = __lsx_vilvh_d(s6, s2);
  kernel.packet[6] = __lsx_vilvl_d(s7, s3);
  kernel.packet[7] = __lsx_vilvh_d(s7, s3);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet8us, 4>& kernel) {
  __m128i t0 = __lsx_vilvl_h(kernel.packet[1], kernel.packet[0]);
  __m128i t1 = __lsx_vilvh_h(kernel.packet[1], kernel.packet[0]);
  __m128i t2 = __lsx_vilvl_h(kernel.packet[3], kernel.packet[2]);
  __m128i t3 = __lsx_vilvh_h(kernel.packet[3], kernel.packet[2]);

  kernel.packet[0] = __lsx_vilvl_w(t2, t0);
  kernel.packet[1] = __lsx_vilvh_w(t2, t0);
  kernel.packet[2] = __lsx_vilvl_w(t3, t1);
  kernel.packet[3] = __lsx_vilvh_w(t3, t1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet4ui, 4>& kernel) {
  __m128i T0 = __lsx_vilvl_w(kernel.packet[1], kernel.packet[0]);
  __m128i T1 = __lsx_vilvh_w(kernel.packet[1], kernel.packet[0]);
  __m128i T2 = __lsx_vilvl_w(kernel.packet[3], kernel.packet[2]);
  __m128i T3 = __lsx_vilvh_w(kernel.packet[3], kernel.packet[2]);

  kernel.packet[0] = __lsx_vilvl_d(T2, T0);
  kernel.packet[1] = __lsx_vilvh_d(T2, T0);
  kernel.packet[2] = __lsx_vilvl_d(T3, T1);
  kernel.packet[3] = __lsx_vilvh_d(T3, T1);
}
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet2ul, 2>& kernel) {
  __m128i tmp = __lsx_vilvh_d(kernel.packet[1], kernel.packet[0]);
  kernel.packet[0] = __lsx_vilvl_d(kernel.packet[1], kernel.packet[0]);
  kernel.packet[1] = tmp;
}

template <>
EIGEN_STRONG_INLINE Packet4f prsqrt(const Packet4f& a) {
  return __lsx_vfrsqrt_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d prsqrt(const Packet2d& a) {
  return __lsx_vfrsqrt_d(a);
}

template <>
EIGEN_STRONG_INLINE Packet4f pfloor(const Packet4f& a) {
  return __lsx_vfrintrm_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d pfloor(const Packet2d& a) {
  return __lsx_vfrintrm_d(a);
}

template <>
EIGEN_STRONG_INLINE Packet4f pceil(const Packet4f& a) {
  return __lsx_vfrintrp_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d pceil(const Packet2d& a) {
  return __lsx_vfrintrp_d(a);
}

template <>
EIGEN_STRONG_INLINE Packet4f pround(const Packet4f& a) {
  const Packet4f mask = pset1frombits<Packet4f>(static_cast<numext::uint32_t>(0x80000000u));
  const Packet4f prev0dot5 = pset1frombits<Packet4f>(static_cast<numext::uint32_t>(0x3EFFFFFFu));
  return __lsx_vfrintrz_s(padd(pxor(pand(a, mask), prev0dot5), a));
}
template <>
EIGEN_STRONG_INLINE Packet2d pround(const Packet2d& a) {
  const Packet2d mask = pset1frombits<Packet2d>(static_cast<numext::uint64_t>(0x8000000000000000ull));
  const Packet2d prev0dot5 = pset1frombits<Packet2d>(static_cast<numext::uint64_t>(0x3FDFFFFFFFFFFFFFull));
  return __lsx_vfrintrz_d(padd(por(pand(a, mask), prev0dot5), a));
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet4f pselect(const Packet4f& mask, const Packet4f& a, const Packet4f& b) {
  return (Packet4f)__lsx_vbitsel_v((__m128i)b, (__m128i)a, (__m128i)mask);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet16c pselect(const Packet16c& mask, const Packet16c& a, const Packet16c& b) {
  return (Packet16c)__lsx_vbitsel_v((__m128i)b, (__m128i)a, (__m128i)mask);
}

template <>
EIGEN_STRONG_INLINE Packet16c ploadquad<Packet16c>(const int8_t* from) {
  int8_t tmp[16] = {*from,       *from,       *from,       *from,       *(from + 1), *(from + 1),
                    *(from + 1), *(from + 1), *(from + 2), *(from + 2), *(from + 2), *(from + 2),
                    *(from + 3), *(from + 3), *(from + 3), *(from + 3)};
  return __lsx_vld(tmp, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc ploadquad<Packet16uc>(const uint8_t* from) {
  uint8_t tmp[16] = {*from,       *from,       *from,       *from,       *(from + 1), *(from + 1),
                     *(from + 1), *(from + 1), *(from + 2), *(from + 2), *(from + 2), *(from + 2),
                     *(from + 3), *(from + 3), *(from + 3), *(from + 3)};
  return __lsx_vld(tmp, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s ploadquad<Packet8s>(const int16_t* from) {
  int16_t tmp[8] = {*from, *from, *from, *from, *(from + 1), *(from + 1), *(from + 1), *(from + 1)};
  return __lsx_vld(tmp, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us ploadquad<Packet8us>(const uint16_t* from) {
  uint16_t tmp[8] = {*from, *from, *from, *from, *(from + 1), *(from + 1), *(from + 1), *(from + 1)};
  return __lsx_vld(tmp, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i ploadquad<Packet4i>(const int32_t* from) {
  int32_t tmp[4] = {*from, *from, *from, *from};
  return __lsx_vld(tmp, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui ploadquad<Packet4ui>(const uint32_t* from) {
  uint32_t tmp[4] = {*from, *from, *from, *from};
  return __lsx_vld(tmp, 0);
}

template <>
EIGEN_STRONG_INLINE Packet16c pnmsub(const Packet16c& a, const Packet16c& b, const Packet16c& c) {
  return __lsx_vmsub_b(pnegate(c), a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pnmsub(const Packet8s& a, const Packet8s& b, const Packet8s& c) {
  return __lsx_vmsub_h(pnegate(c), a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pnmsub(const Packet4i& a, const Packet4i& b, const Packet4i& c) {
  return __lsx_vmsub_w(pnegate(c), a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pnmsub(const Packet2l& a, const Packet2l& b, const Packet2l& c) {
  return __lsx_vmsub_d(pnegate(c), a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16c pmsub(const Packet16c& a, const Packet16c& b, const Packet16c& c) {
  return __lsx_vmadd_b(pnegate(c), a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pmsub(const Packet8s& a, const Packet8s& b, const Packet8s& c) {
  return __lsx_vmadd_h(pnegate(c), a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pmsub(const Packet4i& a, const Packet4i& b, const Packet4i& c) {
  return __lsx_vmadd_w(pnegate(c), a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pmsub(const Packet2l& a, const Packet2l& b, const Packet2l& c) {
  return __lsx_vmadd_d(pnegate(c), a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16c pnmadd(const Packet16c& a, const Packet16c& b, const Packet16c& c) {
  return __lsx_vmsub_b(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet8s pnmadd(const Packet8s& a, const Packet8s& b, const Packet8s& c) {
  return __lsx_vmsub_h(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4i pnmadd(const Packet4i& a, const Packet4i& b, const Packet4i& c) {
  return __lsx_vmsub_w(c, a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2l pnmadd(const Packet2l& a, const Packet2l& b, const Packet2l& c) {
  return __lsx_vmsub_d(c, a, b);
}

template <>
EIGEN_STRONG_INLINE Packet4f pexp(const Packet4f& _x) {
  return pexp_float(_x);
}
template <>
EIGEN_STRONG_INLINE Packet2d pexp(const Packet2d& _x) {
  return pexp_double(_x);
}

template <>
EIGEN_STRONG_INLINE Packet4f pldexp<Packet4f>(const Packet4f& a, const Packet4f& exponent) {
  return pldexp_generic(a, exponent);
}

template <>
EIGEN_STRONG_INLINE Packet2d pfrexp<Packet2d>(const Packet2d& a, Packet2d& exponent) {
  return pfrexp_generic(a, exponent);
}
template <>
EIGEN_STRONG_INLINE Packet4f pfrexp<Packet4f>(const Packet4f& a, Packet4f& exponent) {
  return pfrexp_generic(a, exponent);
}
template <>
EIGEN_STRONG_INLINE Packet4f pzero(const Packet4f& /* a */) {
  Packet4f v = {0.0f, 0.0f, 0.0f, 0.0f};
  return v;
}
template <>
EIGEN_STRONG_INLINE Packet4f pabsdiff<Packet4f>(const Packet4f& a, const Packet4f& b) {
  Packet4f v = psub(a, b);
  return pabs(v);
}
template <>
EIGEN_STRONG_INLINE Packet4f pmin<PropagateNaN, Packet4f>(const Packet4f& a, const Packet4f& b) {
  return pmin<Packet4f>(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4f pmax<PropagateNaN, Packet4f>(const Packet4f& a, const Packet4f& b) {
  return pmax<Packet4f>(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet4f ploadquad<Packet4f>(const float* from) {
  return (__m128)__lsx_vldrepl_w(from, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4f psignbit(const Packet4f& a) {
  return (__m128)__lsx_vsrai_w((__m128i)a, 31);
}
template <>
EIGEN_STRONG_INLINE Packet4f print<Packet4f>(const Packet4f& a) {
  return __lsx_vfrintrne_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet4f ptrunc<Packet4f>(const Packet4f& a) {
  return __lsx_vfrintrz_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet4f preciprocal<Packet4f>(const Packet4f& a) {
  return __lsx_vfrecip_s(a);
}

template <>
EIGEN_STRONG_INLINE Packet2d pzero(const Packet2d& /* a */) {
  Packet2d v = {0.0, 0.0};
  return v;
}
template <>
EIGEN_STRONG_INLINE Packet2d pmin<PropagateNaN, Packet2d>(const Packet2d& a, const Packet2d& b) {
  return pmin<Packet2d>(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d pmax<PropagateNaN, Packet2d>(const Packet2d& a, const Packet2d& b) {
  return pmax<Packet2d>(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet2d psignbit(const Packet2d& a) {
  return (__m128d)(__lsx_vsrai_d((__m128i)a, 63));
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet2d pselect(const Packet2d& mask, const Packet2d& a, const Packet2d& b) {
  return (Packet2d)__lsx_vbitsel_v((__m128i)b, (__m128i)a, (__m128i)mask);
}
template <>
EIGEN_STRONG_INLINE Packet2d print<Packet2d>(const Packet2d& a) {
  return __lsx_vfrintrne_d(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d ptrunc<Packet2d>(const Packet2d& a) {
  return __lsx_vfrintrz_d(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d pldexp<Packet2d>(const Packet2d& a, const Packet2d& exponent) {
  return pldexp_generic(a, exponent);
}

template <>
EIGEN_STRONG_INLINE Packet16c pabsdiff<Packet16c>(const Packet16c& a, const Packet16c& b) {
  Packet16c v = psub(a, b);
  return pabs(v);
}

template <>
EIGEN_STRONG_INLINE Packet8s pabsdiff<Packet8s>(const Packet8s& a, const Packet8s& b) {
  Packet8s v = psub(a, b);
  return pabs(v);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet8s pselect(const Packet8s& mask, const Packet8s& a, const Packet8s& b) {
  return __lsx_vbitsel_v(b, a, mask);
}

template <>
EIGEN_STRONG_INLINE Packet4i pabsdiff<Packet4i>(const Packet4i& a, const Packet4i& b) {
  Packet4i v = psub(a, b);
  return pabs(v);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet4i pselect(const Packet4i& mask, const Packet4i& a, const Packet4i& b) {
  return __lsx_vbitsel_v(b, a, mask);
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet2l pselect(const Packet2l& mask, const Packet2l& a, const Packet2l& b) {
  return __lsx_vbitsel_v(b, a, mask);
}

template <>
EIGEN_STRONG_INLINE Packet16uc pdiv<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  return __lsx_vdiv_bu(a, b);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pabsdiff<Packet16uc>(const Packet16uc& a, const Packet16uc& b) {
  Packet16uc v = psub(a, b);
  return pabs(v);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet16uc pselect(const Packet16uc& mask, const Packet16uc& a,
                                                         const Packet16uc& b) {
  return __lsx_vbitsel_v(b, a, mask);
}
template <>
EIGEN_STRONG_INLINE Packet16uc psqrt(const Packet16uc& a) {
  __m128i res = {0, 0};
  __m128i add = {0x0808080808080808, 0x0808080808080808};
  for (int i = 0; i < 4; i++) {
    const __m128i temp = __lsx_vor_v(res, add);
    const __m128i tmul = __lsx_vpackev_b(__lsx_vmulwod_h_bu(temp, temp), __lsx_vmulwev_h_bu(temp, temp));
    res = __lsx_vbitsel_v(res, temp, __lsx_vsle_bu(tmul, a));
    add = __lsx_vsrli_b(add, 1);
  }
  return res;
}

template <>
EIGEN_STRONG_INLINE Packet8us pabsdiff<Packet8us>(const Packet8us& a, const Packet8us& b) {
  Packet8us v = psub(a, b);
  return pabs(v);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet8us pselect(const Packet8us& mask, const Packet8us& a, const Packet8us& b) {
  return __lsx_vbitsel_v(b, a, mask);
}
template <>
EIGEN_STRONG_INLINE Packet8us psqrt(const Packet8us& a) {
  __m128i res = {0, 0};
  __m128i add = {0x0080008000800080, 0x0080008000800080};
  for (int i = 0; i < 4; i++) {
    const __m128i temp = __lsx_vor_v(res, add);
    const __m128i tmul = __lsx_vpackev_h(__lsx_vmulwod_w_hu(temp, temp), __lsx_vmulwev_w_hu(temp, temp));
    res = __lsx_vbitsel_v(res, temp, __lsx_vsle_hu(tmul, a));
    add = __lsx_vsrli_h(add, 1);
  }
  return res;
}

template <>
EIGEN_STRONG_INLINE Packet4ui pabsdiff<Packet4ui>(const Packet4ui& a, const Packet4ui& b) {
  Packet4ui v = psub(a, b);
  return pabs(v);
}
template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet4ui pselect(const Packet4ui& mask, const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vbitsel_v(b, a, mask);
}
template <>
EIGEN_STRONG_INLINE Packet4ui psqrt(const Packet4ui& a) {
  __m128i res = {0, 0};
  __m128i add = {0x0000800000008000, 0x0000800000008000};
  for (int i = 0; i < 4; i++) {
    const __m128i temp = __lsx_vor_v(res, add);
    const __m128i tmul = __lsx_vpackev_w(__lsx_vmulwod_d_wu(temp, temp), __lsx_vmulwev_d_wu(temp, temp));
    res = __lsx_vbitsel_v(res, temp, __lsx_vsle_wu(tmul, a));
    add = __lsx_vsrli_w(add, 1);
  }
  return res;
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet2ul pselect(const Packet2ul& mask, const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vbitsel_v(b, a, mask);
}

}  // namespace internal
}  // namespace Eigen
#endif
