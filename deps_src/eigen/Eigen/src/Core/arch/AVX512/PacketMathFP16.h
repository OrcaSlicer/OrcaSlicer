// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 The Eigen Authors.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_PACKET_MATH_FP16_AVX512_H
#define EIGEN_PACKET_MATH_FP16_AVX512_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

typedef __m512h Packet32h;
typedef __m256h Packet16h;
typedef __m128h Packet8h;

template <>
struct is_arithmetic<Packet8h> {
  enum { value = true };
};

template <>
struct packet_traits<half> : default_packet_traits {
  typedef Packet32h type;
  typedef Packet16h half;
  enum {
    Vectorizable = 1,
    AlignedOnScalar = 1,
    size = 32,

    HasCmp = 1,
    HasAdd = 1,
    HasSub = 1,
    HasMul = 1,
    HasDiv = 1,
    HasNegate = 1,
    HasAbs = 1,
    HasAbs2 = 0,
    HasMin = 1,
    HasMax = 1,
    HasConj = 1,
    HasSetLinear = 0,
    HasLog = 1,
    HasLog1p = 1,
    HasExp = 1,
    HasExpm1 = 1,
    HasSqrt = 1,
    HasRsqrt = 1,
    // These ones should be implemented in future
    HasBessel = 0,
    HasNdtri = 0,
    HasSin = EIGEN_FAST_MATH,
    HasCos = EIGEN_FAST_MATH,
    HasTanh = EIGEN_FAST_MATH,
    HasErf = 0,  // EIGEN_FAST_MATH,
    HasBlend = 0
  };
};

template <>
struct unpacket_traits<Packet32h> {
  typedef Eigen::half type;
  typedef Packet16h half;
  typedef Packet32s integer_packet;
  enum {
    size = 32,
    alignment = Aligned64,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};

template <>
struct unpacket_traits<Packet16h> {
  typedef Eigen::half type;
  typedef Packet8h half;
  typedef Packet16s integer_packet;
  enum {
    size = 16,
    alignment = Aligned32,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};

template <>
struct unpacket_traits<Packet8h> {
  typedef Eigen::half type;
  typedef Packet8h half;
  typedef Packet8s integer_packet;
  enum {
    size = 8,
    alignment = Aligned16,
    vectorizable = true,
    masked_load_available = false,
    masked_store_available = false
  };
};

// Conversions

EIGEN_STRONG_INLINE Packet16f half2float(const Packet16h& a) { return _mm512_cvtxph_ps(a); }

EIGEN_STRONG_INLINE Packet8f half2float(const Packet8h& a) { return _mm256_cvtxph_ps(a); }

EIGEN_STRONG_INLINE Packet16h float2half(const Packet16f& a) { return _mm512_cvtxps_ph(a); }

EIGEN_STRONG_INLINE Packet8h float2half(const Packet8f& a) { return _mm256_cvtxps_ph(a); }

// Memory functions

// pset1

template <>
EIGEN_STRONG_INLINE Packet32h pset1<Packet32h>(const Eigen::half& from) {
  return _mm512_set1_ph(from.x);
}

template <>
EIGEN_STRONG_INLINE Packet16h pset1<Packet16h>(const Eigen::half& from) {
  return _mm256_set1_ph(from.x);
}

template <>
EIGEN_STRONG_INLINE Packet8h pset1<Packet8h>(const Eigen::half& from) {
  return _mm_set1_ph(from.x);
}

template <>
EIGEN_STRONG_INLINE Packet32h pzero(const Packet32h& /*a*/) {
  return _mm512_setzero_ph();
}

template <>
EIGEN_STRONG_INLINE Packet16h pzero(const Packet16h& /*a*/) {
  return _mm256_setzero_ph();
}

template <>
EIGEN_STRONG_INLINE Packet8h pzero(const Packet8h& /*a*/) {
  return _mm_setzero_ph();
}

// pset1frombits
template <>
EIGEN_STRONG_INLINE Packet32h pset1frombits<Packet32h>(unsigned short from) {
  return _mm512_castsi512_ph(_mm512_set1_epi16(from));
}

template <>
EIGEN_STRONG_INLINE Packet16h pset1frombits<Packet16h>(unsigned short from) {
  return _mm256_castsi256_ph(_mm256_set1_epi16(from));
}

template <>
EIGEN_STRONG_INLINE Packet8h pset1frombits<Packet8h>(unsigned short from) {
  return _mm_castsi128_ph(_mm_set1_epi16(from));
}

// pfirst

template <>
EIGEN_STRONG_INLINE Eigen::half pfirst<Packet32h>(const Packet32h& from) {
  return Eigen::half(_mm512_cvtsh_h(from));
}

template <>
EIGEN_STRONG_INLINE Eigen::half pfirst<Packet16h>(const Packet16h& from) {
  return Eigen::half(_mm256_cvtsh_h(from));
}

template <>
EIGEN_STRONG_INLINE Eigen::half pfirst<Packet8h>(const Packet8h& from) {
  return Eigen::half(_mm_cvtsh_h(from));
}

// pload

template <>
EIGEN_STRONG_INLINE Packet32h pload<Packet32h>(const Eigen::half* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return _mm512_load_ph(from);
}

template <>
EIGEN_STRONG_INLINE Packet16h pload<Packet16h>(const Eigen::half* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return _mm256_load_ph(from);
}

template <>
EIGEN_STRONG_INLINE Packet8h pload<Packet8h>(const Eigen::half* from) {
  EIGEN_DEBUG_ALIGNED_LOAD return _mm_load_ph(from);
}

// ploadu

template <>
EIGEN_STRONG_INLINE Packet32h ploadu<Packet32h>(const Eigen::half* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return _mm512_loadu_ph(from);
}

template <>
EIGEN_STRONG_INLINE Packet16h ploadu<Packet16h>(const Eigen::half* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return _mm256_loadu_ph(from);
}

template <>
EIGEN_STRONG_INLINE Packet8h ploadu<Packet8h>(const Eigen::half* from) {
  EIGEN_DEBUG_UNALIGNED_LOAD return _mm_loadu_ph(from);
}

// pstore

template <>
EIGEN_STRONG_INLINE void pstore<half>(Eigen::half* to, const Packet32h& from) {
  EIGEN_DEBUG_ALIGNED_STORE _mm512_store_ph(to, from);
}

template <>
EIGEN_STRONG_INLINE void pstore<half>(Eigen::half* to, const Packet16h& from) {
  EIGEN_DEBUG_ALIGNED_STORE _mm256_store_ph(to, from);
}

template <>
EIGEN_STRONG_INLINE void pstore<half>(Eigen::half* to, const Packet8h& from) {
  EIGEN_DEBUG_ALIGNED_STORE _mm_store_ph(to, from);
}

// pstoreu

template <>
EIGEN_STRONG_INLINE void pstoreu<half>(Eigen::half* to, const Packet32h& from) {
  EIGEN_DEBUG_UNALIGNED_STORE _mm512_storeu_ph(to, from);
}

template <>
EIGEN_STRONG_INLINE void pstoreu<half>(Eigen::half* to, const Packet16h& from) {
  EIGEN_DEBUG_UNALIGNED_STORE _mm256_storeu_ph(to, from);
}

template <>
EIGEN_STRONG_INLINE void pstoreu<half>(Eigen::half* to, const Packet8h& from) {
  EIGEN_DEBUG_UNALIGNED_STORE _mm_storeu_ph(to, from);
}

// ploaddup
template <>
EIGEN_STRONG_INLINE Packet32h ploaddup<Packet32h>(const Eigen::half* from) {
  __m512h a = _mm512_castph256_ph512(_mm256_loadu_ph(from));
  return _mm512_permutexvar_ph(_mm512_set_epi16(15, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6,
                                                5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0),
                               a);
}

template <>
EIGEN_STRONG_INLINE Packet16h ploaddup<Packet16h>(const Eigen::half* from) {
  __m256h a = _mm256_castph128_ph256(_mm_loadu_ph(from));
  return _mm256_permutexvar_ph(_mm256_set_epi16(7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0), a);
}

template <>
EIGEN_STRONG_INLINE Packet8h ploaddup<Packet8h>(const Eigen::half* from) {
  return _mm_set_ph(from[3].x, from[3].x, from[2].x, from[2].x, from[1].x, from[1].x, from[0].x, from[0].x);
}

// ploadquad
template <>
EIGEN_STRONG_INLINE Packet32h ploadquad<Packet32h>(const Eigen::half* from) {
  __m512h a = _mm512_castph128_ph512(_mm_loadu_ph(from));
  return _mm512_permutexvar_ph(
      _mm512_set_epi16(7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0),
      a);
}

template <>
EIGEN_STRONG_INLINE Packet16h ploadquad<Packet16h>(const Eigen::half* from) {
  return _mm256_set_ph(from[3].x, from[3].x, from[3].x, from[3].x, from[2].x, from[2].x, from[2].x, from[2].x,
                       from[1].x, from[1].x, from[1].x, from[1].x, from[0].x, from[0].x, from[0].x, from[0].x);
}

template <>
EIGEN_STRONG_INLINE Packet8h ploadquad<Packet8h>(const Eigen::half* from) {
  return _mm_set_ph(from[1].x, from[1].x, from[1].x, from[1].x, from[0].x, from[0].x, from[0].x, from[0].x);
}

// pabs

template <>
EIGEN_STRONG_INLINE Packet32h pabs<Packet32h>(const Packet32h& a) {
  return _mm512_abs_ph(a);
}

template <>
EIGEN_STRONG_INLINE Packet16h pabs<Packet16h>(const Packet16h& a) {
  return _mm256_abs_ph(a);
}

template <>
EIGEN_STRONG_INLINE Packet8h pabs<Packet8h>(const Packet8h& a) {
  return _mm_abs_ph(a);
}

// psignbit

template <>
EIGEN_STRONG_INLINE Packet32h psignbit<Packet32h>(const Packet32h& a) {
  return _mm512_castsi512_ph(_mm512_srai_epi16(_mm512_castph_si512(a), 15));
}

template <>
EIGEN_STRONG_INLINE Packet16h psignbit<Packet16h>(const Packet16h& a) {
  return _mm256_castsi256_ph(_mm256_srai_epi16(_mm256_castph_si256(a), 15));
}

template <>
EIGEN_STRONG_INLINE Packet8h psignbit<Packet8h>(const Packet8h& a) {
  return _mm_castsi128_ph(_mm_srai_epi16(_mm_castph_si128(a), 15));
}

// pmin

template <>
EIGEN_STRONG_INLINE Packet32h pmin<Packet32h>(const Packet32h& a, const Packet32h& b) {
  return _mm512_min_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16h pmin<Packet16h>(const Packet16h& a, const Packet16h& b) {
  return _mm256_min_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet8h pmin<Packet8h>(const Packet8h& a, const Packet8h& b) {
  return _mm_min_ph(a, b);
}

// pmax

template <>
EIGEN_STRONG_INLINE Packet32h pmax<Packet32h>(const Packet32h& a, const Packet32h& b) {
  return _mm512_max_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16h pmax<Packet16h>(const Packet16h& a, const Packet16h& b) {
  return _mm256_max_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet8h pmax<Packet8h>(const Packet8h& a, const Packet8h& b) {
  return _mm_max_ph(a, b);
}

// plset
template <>
EIGEN_STRONG_INLINE Packet32h plset<Packet32h>(const half& a) {
  return _mm512_add_ph(pset1<Packet32h>(a), _mm512_set_ph(31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                                          16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}

template <>
EIGEN_STRONG_INLINE Packet16h plset<Packet16h>(const half& a) {
  return _mm256_add_ph(pset1<Packet16h>(a), _mm256_set_ph(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}

template <>
EIGEN_STRONG_INLINE Packet8h plset<Packet8h>(const half& a) {
  return _mm_add_ph(pset1<Packet8h>(a), _mm_set_ph(7, 6, 5, 4, 3, 2, 1, 0));
}

// por

template <>
EIGEN_STRONG_INLINE Packet32h por(const Packet32h& a, const Packet32h& b) {
  return _mm512_castsi512_ph(_mm512_or_si512(_mm512_castph_si512(a), _mm512_castph_si512(b)));
}

template <>
EIGEN_STRONG_INLINE Packet16h por(const Packet16h& a, const Packet16h& b) {
  return _mm256_castsi256_ph(_mm256_or_si256(_mm256_castph_si256(a), _mm256_castph_si256(b)));
}

template <>
EIGEN_STRONG_INLINE Packet8h por(const Packet8h& a, const Packet8h& b) {
  return _mm_castsi128_ph(_mm_or_si128(_mm_castph_si128(a), _mm_castph_si128(b)));
}

// pxor

template <>
EIGEN_STRONG_INLINE Packet32h pxor(const Packet32h& a, const Packet32h& b) {
  return _mm512_castsi512_ph(_mm512_xor_si512(_mm512_castph_si512(a), _mm512_castph_si512(b)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pxor(const Packet16h& a, const Packet16h& b) {
  return _mm256_castsi256_ph(_mm256_xor_si256(_mm256_castph_si256(a), _mm256_castph_si256(b)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pxor(const Packet8h& a, const Packet8h& b) {
  return _mm_castsi128_ph(_mm_xor_si128(_mm_castph_si128(a), _mm_castph_si128(b)));
}

// pand

template <>
EIGEN_STRONG_INLINE Packet32h pand(const Packet32h& a, const Packet32h& b) {
  return _mm512_castsi512_ph(_mm512_and_si512(_mm512_castph_si512(a), _mm512_castph_si512(b)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pand(const Packet16h& a, const Packet16h& b) {
  return _mm256_castsi256_ph(_mm256_and_si256(_mm256_castph_si256(a), _mm256_castph_si256(b)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pand(const Packet8h& a, const Packet8h& b) {
  return _mm_castsi128_ph(_mm_and_si128(_mm_castph_si128(a), _mm_castph_si128(b)));
}

// pandnot

template <>
EIGEN_STRONG_INLINE Packet32h pandnot(const Packet32h& a, const Packet32h& b) {
  return _mm512_castsi512_ph(_mm512_andnot_si512(_mm512_castph_si512(b), _mm512_castph_si512(a)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pandnot(const Packet16h& a, const Packet16h& b) {
  return _mm256_castsi256_ph(_mm256_andnot_si256(_mm256_castph_si256(b), _mm256_castph_si256(a)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pandnot(const Packet8h& a, const Packet8h& b) {
  return _mm_castsi128_ph(_mm_andnot_si128(_mm_castph_si128(b), _mm_castph_si128(a)));
}

// pselect

template <>
EIGEN_DEVICE_FUNC inline Packet32h pselect(const Packet32h& mask, const Packet32h& a, const Packet32h& b) {
  __mmask32 mask32 = _mm512_cmp_epi16_mask(_mm512_castph_si512(mask), _mm512_setzero_epi32(), _MM_CMPINT_EQ);
  return _mm512_mask_blend_ph(mask32, a, b);
}

template <>
EIGEN_DEVICE_FUNC inline Packet16h pselect(const Packet16h& mask, const Packet16h& a, const Packet16h& b) {
  __mmask16 mask16 = _mm256_cmp_epi16_mask(_mm256_castph_si256(mask), _mm256_setzero_si256(), _MM_CMPINT_EQ);
  return _mm256_mask_blend_ph(mask16, a, b);
}

template <>
EIGEN_DEVICE_FUNC inline Packet8h pselect(const Packet8h& mask, const Packet8h& a, const Packet8h& b) {
  __mmask8 mask8 = _mm_cmp_epi16_mask(_mm_castph_si128(mask), _mm_setzero_si128(), _MM_CMPINT_EQ);
  return _mm_mask_blend_ph(mask8, a, b);
}

// pcmp_eq

template <>
EIGEN_STRONG_INLINE Packet32h pcmp_eq(const Packet32h& a, const Packet32h& b) {
  __mmask32 mask = _mm512_cmp_ph_mask(a, b, _CMP_EQ_OQ);
  return _mm512_castsi512_ph(_mm512_mask_set1_epi16(_mm512_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pcmp_eq(const Packet16h& a, const Packet16h& b) {
  __mmask16 mask = _mm256_cmp_ph_mask(a, b, _CMP_EQ_OQ);
  return _mm256_castsi256_ph(_mm256_mask_set1_epi16(_mm256_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pcmp_eq(const Packet8h& a, const Packet8h& b) {
  __mmask8 mask = _mm_cmp_ph_mask(a, b, _CMP_EQ_OQ);
  return _mm_castsi128_ph(_mm_mask_set1_epi16(_mm_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

// pcmp_le

template <>
EIGEN_STRONG_INLINE Packet32h pcmp_le(const Packet32h& a, const Packet32h& b) {
  __mmask32 mask = _mm512_cmp_ph_mask(a, b, _CMP_LE_OQ);
  return _mm512_castsi512_ph(_mm512_mask_set1_epi16(_mm512_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pcmp_le(const Packet16h& a, const Packet16h& b) {
  __mmask16 mask = _mm256_cmp_ph_mask(a, b, _CMP_LE_OQ);
  return _mm256_castsi256_ph(_mm256_mask_set1_epi16(_mm256_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pcmp_le(const Packet8h& a, const Packet8h& b) {
  __mmask8 mask = _mm_cmp_ph_mask(a, b, _CMP_LE_OQ);
  return _mm_castsi128_ph(_mm_mask_set1_epi16(_mm_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

// pcmp_lt

template <>
EIGEN_STRONG_INLINE Packet32h pcmp_lt(const Packet32h& a, const Packet32h& b) {
  __mmask32 mask = _mm512_cmp_ph_mask(a, b, _CMP_LT_OQ);
  return _mm512_castsi512_ph(_mm512_mask_set1_epi16(_mm512_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pcmp_lt(const Packet16h& a, const Packet16h& b) {
  __mmask16 mask = _mm256_cmp_ph_mask(a, b, _CMP_LT_OQ);
  return _mm256_castsi256_ph(_mm256_mask_set1_epi16(_mm256_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pcmp_lt(const Packet8h& a, const Packet8h& b) {
  __mmask8 mask = _mm_cmp_ph_mask(a, b, _CMP_LT_OQ);
  return _mm_castsi128_ph(_mm_mask_set1_epi16(_mm_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

// pcmp_lt_or_nan

template <>
EIGEN_STRONG_INLINE Packet32h pcmp_lt_or_nan(const Packet32h& a, const Packet32h& b) {
  __mmask32 mask = _mm512_cmp_ph_mask(a, b, _CMP_NGE_UQ);
  return _mm512_castsi512_ph(_mm512_mask_set1_epi16(_mm512_set1_epi16(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet16h pcmp_lt_or_nan(const Packet16h& a, const Packet16h& b) {
  __mmask16 mask = _mm256_cmp_ph_mask(a, b, _CMP_NGE_UQ);
  return _mm256_castsi256_ph(_mm256_mask_set1_epi16(_mm256_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

template <>
EIGEN_STRONG_INLINE Packet8h pcmp_lt_or_nan(const Packet8h& a, const Packet8h& b) {
  __mmask8 mask = _mm_cmp_ph_mask(a, b, _CMP_NGE_UQ);
  return _mm_castsi128_ph(_mm_mask_set1_epi16(_mm_set1_epi32(0), mask, static_cast<short>(0xffffu)));
}

// padd

template <>
EIGEN_STRONG_INLINE Packet32h padd<Packet32h>(const Packet32h& a, const Packet32h& b) {
  return _mm512_add_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16h padd<Packet16h>(const Packet16h& a, const Packet16h& b) {
  return _mm256_add_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet8h padd<Packet8h>(const Packet8h& a, const Packet8h& b) {
  return _mm_add_ph(a, b);
}

// psub

template <>
EIGEN_STRONG_INLINE Packet32h psub<Packet32h>(const Packet32h& a, const Packet32h& b) {
  return _mm512_sub_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16h psub<Packet16h>(const Packet16h& a, const Packet16h& b) {
  return _mm256_sub_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet8h psub<Packet8h>(const Packet8h& a, const Packet8h& b) {
  return _mm_sub_ph(a, b);
}

// pmul

template <>
EIGEN_STRONG_INLINE Packet32h pmul<Packet32h>(const Packet32h& a, const Packet32h& b) {
  return _mm512_mul_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16h pmul<Packet16h>(const Packet16h& a, const Packet16h& b) {
  return _mm256_mul_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet8h pmul<Packet8h>(const Packet8h& a, const Packet8h& b) {
  return _mm_mul_ph(a, b);
}

// pdiv

template <>
EIGEN_STRONG_INLINE Packet32h pdiv<Packet32h>(const Packet32h& a, const Packet32h& b) {
  return _mm512_div_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet16h pdiv<Packet16h>(const Packet16h& a, const Packet16h& b) {
  return _mm256_div_ph(a, b);
}

template <>
EIGEN_STRONG_INLINE Packet8h pdiv<Packet8h>(const Packet8h& a, const Packet8h& b) {
  return _mm_div_ph(a, b);
  ;
}

// pround

template <>
EIGEN_STRONG_INLINE Packet32h pround<Packet32h>(const Packet32h& a) {
  // Work-around for default std::round rounding mode.

  // Mask for the sign bit.
  const Packet32h signMask =
      pset1frombits<Packet32h>(static_cast<numext::uint16_t>(static_cast<std::uint16_t>(0x8000u)));
  // The largest half-precision float less than 0.5.
  const Packet32h prev0dot5 = pset1frombits<Packet32h>(static_cast<numext::uint16_t>(0x37FFu));

  return _mm512_roundscale_ph(padd(por(pand(a, signMask), prev0dot5), a), _MM_FROUND_TO_ZERO);
}

template <>
EIGEN_STRONG_INLINE Packet16h pround<Packet16h>(const Packet16h& a) {
  // Work-around for default std::round rounding mode.

  // Mask for the sign bit.
  const Packet16h signMask =
      pset1frombits<Packet16h>(static_cast<numext::uint16_t>(static_cast<std::uint16_t>(0x8000u)));
  // The largest half-precision float less than 0.5.
  const Packet16h prev0dot5 = pset1frombits<Packet16h>(static_cast<numext::uint16_t>(0x37FFu));

  return _mm256_roundscale_ph(padd(por(pand(a, signMask), prev0dot5), a), _MM_FROUND_TO_ZERO);
}

template <>
EIGEN_STRONG_INLINE Packet8h pround<Packet8h>(const Packet8h& a) {
  // Work-around for default std::round rounding mode.

  // Mask for the sign bit.
  const Packet8h signMask = pset1frombits<Packet8h>(static_cast<numext::uint16_t>(static_cast<std::uint16_t>(0x8000u)));
  // The largest half-precision float less than 0.5.
  const Packet8h prev0dot5 = pset1frombits<Packet8h>(static_cast<numext::uint16_t>(0x37FFu));

  return _mm_roundscale_ph(padd(por(pand(a, signMask), prev0dot5), a), _MM_FROUND_TO_ZERO);
}

// print

template <>
EIGEN_STRONG_INLINE Packet32h print<Packet32h>(const Packet32h& a) {
  return _mm512_roundscale_ph(a, _MM_FROUND_CUR_DIRECTION);
}

template <>
EIGEN_STRONG_INLINE Packet16h print<Packet16h>(const Packet16h& a) {
  return _mm256_roundscale_ph(a, _MM_FROUND_CUR_DIRECTION);
}

template <>
EIGEN_STRONG_INLINE Packet8h print<Packet8h>(const Packet8h& a) {
  return _mm_roundscale_ph(a, _MM_FROUND_CUR_DIRECTION);
}

// pceil

template <>
EIGEN_STRONG_INLINE Packet32h pceil<Packet32h>(const Packet32h& a) {
  return _mm512_roundscale_ph(a, _MM_FROUND_TO_POS_INF);
}

template <>
EIGEN_STRONG_INLINE Packet16h pceil<Packet16h>(const Packet16h& a) {
  return _mm256_roundscale_ph(a, _MM_FROUND_TO_POS_INF);
}

template <>
EIGEN_STRONG_INLINE Packet8h pceil<Packet8h>(const Packet8h& a) {
  return _mm_roundscale_ph(a, _MM_FROUND_TO_POS_INF);
}

// pfloor

template <>
EIGEN_STRONG_INLINE Packet32h pfloor<Packet32h>(const Packet32h& a) {
  return _mm512_roundscale_ph(a, _MM_FROUND_TO_NEG_INF);
}

template <>
EIGEN_STRONG_INLINE Packet16h pfloor<Packet16h>(const Packet16h& a) {
  return _mm256_roundscale_ph(a, _MM_FROUND_TO_NEG_INF);
}

template <>
EIGEN_STRONG_INLINE Packet8h pfloor<Packet8h>(const Packet8h& a) {
  return _mm_roundscale_ph(a, _MM_FROUND_TO_NEG_INF);
}

// ptrunc

template <>
EIGEN_STRONG_INLINE Packet32h ptrunc<Packet32h>(const Packet32h& a) {
  return _mm512_roundscale_ph(a, _MM_FROUND_TO_ZERO);
}

template <>
EIGEN_STRONG_INLINE Packet16h ptrunc<Packet16h>(const Packet16h& a) {
  return _mm256_roundscale_ph(a, _MM_FROUND_TO_ZERO);
}

template <>
EIGEN_STRONG_INLINE Packet8h ptrunc<Packet8h>(const Packet8h& a) {
  return _mm_roundscale_ph(a, _MM_FROUND_TO_ZERO);
}

// predux
template <>
EIGEN_STRONG_INLINE half predux<Packet32h>(const Packet32h& a) {
  return half(_mm512_reduce_add_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux<Packet16h>(const Packet16h& a) {
  return half(_mm256_reduce_add_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux<Packet8h>(const Packet8h& a) {
  return half(_mm_reduce_add_ph(a));
}

// predux_half_dowto4
template <>
EIGEN_STRONG_INLINE Packet16h predux_half_dowto4<Packet32h>(const Packet32h& a) {
  const __m512i bits = _mm512_castph_si512(a);
  Packet16h lo = _mm256_castsi256_ph(_mm512_castsi512_si256(bits));
  Packet16h hi = _mm256_castsi256_ph(_mm512_extracti64x4_epi64(bits, 1));
  return padd(lo, hi);
}

template <>
EIGEN_STRONG_INLINE Packet8h predux_half_dowto4<Packet16h>(const Packet16h& a) {
  Packet8h lo = _mm_castsi128_ph(_mm256_castsi256_si128(_mm256_castph_si256(a)));
  Packet8h hi = _mm_castps_ph(_mm256_extractf128_ps(_mm256_castph_ps(a), 1));
  return padd(lo, hi);
}

// predux_max

template <>
EIGEN_STRONG_INLINE half predux_max<Packet32h>(const Packet32h& a) {
  return half(_mm512_reduce_max_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux_max<Packet16h>(const Packet16h& a) {
  return half(_mm256_reduce_max_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux_max<Packet8h>(const Packet8h& a) {
  return half(_mm_reduce_max_ph(a));
}

// predux_min

template <>
EIGEN_STRONG_INLINE half predux_min<Packet32h>(const Packet32h& a) {
  return half(_mm512_reduce_min_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux_min<Packet16h>(const Packet16h& a) {
  return half(_mm256_reduce_min_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux_min<Packet8h>(const Packet8h& a) {
  return half(_mm_reduce_min_ph(a));
}

// predux_mul

template <>
EIGEN_STRONG_INLINE half predux_mul<Packet32h>(const Packet32h& a) {
  return half(_mm512_reduce_mul_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux_mul<Packet16h>(const Packet16h& a) {
  return half(_mm256_reduce_mul_ph(a));
}

template <>
EIGEN_STRONG_INLINE half predux_mul<Packet8h>(const Packet8h& a) {
  return half(_mm_reduce_mul_ph(a));
}

#ifdef EIGEN_VECTORIZE_FMA

// pmadd

template <>
EIGEN_STRONG_INLINE Packet32h pmadd(const Packet32h& a, const Packet32h& b, const Packet32h& c) {
  return _mm512_fmadd_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet16h pmadd(const Packet16h& a, const Packet16h& b, const Packet16h& c) {
  return _mm256_fmadd_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet8h pmadd(const Packet8h& a, const Packet8h& b, const Packet8h& c) {
  return _mm_fmadd_ph(a, b, c);
}

// pmsub

template <>
EIGEN_STRONG_INLINE Packet32h pmsub(const Packet32h& a, const Packet32h& b, const Packet32h& c) {
  return _mm512_fmsub_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet16h pmsub(const Packet16h& a, const Packet16h& b, const Packet16h& c) {
  return _mm256_fmsub_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet8h pmsub(const Packet8h& a, const Packet8h& b, const Packet8h& c) {
  return _mm_fmsub_ph(a, b, c);
}

// pnmadd

template <>
EIGEN_STRONG_INLINE Packet32h pnmadd(const Packet32h& a, const Packet32h& b, const Packet32h& c) {
  return _mm512_fnmadd_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet16h pnmadd(const Packet16h& a, const Packet16h& b, const Packet16h& c) {
  return _mm256_fnmadd_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet8h pnmadd(const Packet8h& a, const Packet8h& b, const Packet8h& c) {
  return _mm_fnmadd_ph(a, b, c);
}

// pnmsub

template <>
EIGEN_STRONG_INLINE Packet32h pnmsub(const Packet32h& a, const Packet32h& b, const Packet32h& c) {
  return _mm512_fnmsub_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet16h pnmsub(const Packet16h& a, const Packet16h& b, const Packet16h& c) {
  return _mm256_fnmsub_ph(a, b, c);
}

template <>
EIGEN_STRONG_INLINE Packet8h pnmsub(const Packet8h& a, const Packet8h& b, const Packet8h& c) {
  return _mm_fnmsub_ph(a, b, c);
}

#endif

// pnegate

template <>
EIGEN_STRONG_INLINE Packet32h pnegate<Packet32h>(const Packet32h& a) {
  return _mm512_castsi512_ph(
      _mm512_xor_si512(_mm512_castph_si512(a), _mm512_set1_epi16(static_cast<std::uint16_t>(0x8000u))));
}

template <>
EIGEN_STRONG_INLINE Packet16h pnegate<Packet16h>(const Packet16h& a) {
  return _mm256_castsi256_ph(
      _mm256_xor_si256(_mm256_castph_si256(a), _mm256_set1_epi16(static_cast<std::uint16_t>(0x8000u))));
}

template <>
EIGEN_STRONG_INLINE Packet8h pnegate<Packet8h>(const Packet8h& a) {
  return _mm_castsi128_ph(_mm_xor_si128(_mm_castph_si128(a), _mm_set1_epi16(static_cast<std::uint16_t>(0x8000u))));
}

// pconj

// Nothing, packets are real.

// psqrt

template <>
EIGEN_STRONG_INLINE Packet32h psqrt<Packet32h>(const Packet32h& a) {
  return generic_sqrt_newton_step<Packet32h>::run(a, _mm512_rsqrt_ph(a));
}

template <>
EIGEN_STRONG_INLINE Packet16h psqrt<Packet16h>(const Packet16h& a) {
  return generic_sqrt_newton_step<Packet16h>::run(a, _mm256_rsqrt_ph(a));
}

template <>
EIGEN_STRONG_INLINE Packet8h psqrt<Packet8h>(const Packet8h& a) {
  return generic_sqrt_newton_step<Packet8h>::run(a, _mm_rsqrt_ph(a));
}

// prsqrt

template <>
EIGEN_STRONG_INLINE Packet32h prsqrt<Packet32h>(const Packet32h& a) {
  return generic_rsqrt_newton_step<Packet32h, /*Steps=*/1>::run(a, _mm512_rsqrt_ph(a));
}

template <>
EIGEN_STRONG_INLINE Packet16h prsqrt<Packet16h>(const Packet16h& a) {
  return generic_rsqrt_newton_step<Packet16h, /*Steps=*/1>::run(a, _mm256_rsqrt_ph(a));
}

template <>
EIGEN_STRONG_INLINE Packet8h prsqrt<Packet8h>(const Packet8h& a) {
  return generic_rsqrt_newton_step<Packet8h, /*Steps=*/1>::run(a, _mm_rsqrt_ph(a));
}

// preciprocal

template <>
EIGEN_STRONG_INLINE Packet32h preciprocal<Packet32h>(const Packet32h& a) {
  return generic_reciprocal_newton_step<Packet32h, /*Steps=*/1>::run(a, _mm512_rcp_ph(a));
}

template <>
EIGEN_STRONG_INLINE Packet16h preciprocal<Packet16h>(const Packet16h& a) {
  return generic_reciprocal_newton_step<Packet16h, /*Steps=*/1>::run(a, _mm256_rcp_ph(a));
}

template <>
EIGEN_STRONG_INLINE Packet8h preciprocal<Packet8h>(const Packet8h& a) {
  return generic_reciprocal_newton_step<Packet8h, /*Steps=*/1>::run(a, _mm_rcp_ph(a));
}

// ptranspose

EIGEN_DEVICE_FUNC inline void ptranspose(PacketBlock<Packet32h, 32>& a) {
  __m512i t[32];

  EIGEN_UNROLL_LOOP
  for (int i = 0; i < 16; i++) {
    t[2 * i] = _mm512_unpacklo_epi16(_mm512_castph_si512(a.packet[2 * i]), _mm512_castph_si512(a.packet[2 * i + 1]));
    t[2 * i + 1] =
        _mm512_unpackhi_epi16(_mm512_castph_si512(a.packet[2 * i]), _mm512_castph_si512(a.packet[2 * i + 1]));
  }

  __m512i p[32];

  EIGEN_UNROLL_LOOP
  for (int i = 0; i < 8; i++) {
    p[4 * i] = _mm512_unpacklo_epi32(t[4 * i], t[4 * i + 2]);
    p[4 * i + 1] = _mm512_unpackhi_epi32(t[4 * i], t[4 * i + 2]);
    p[4 * i + 2] = _mm512_unpacklo_epi32(t[4 * i + 1], t[4 * i + 3]);
    p[4 * i + 3] = _mm512_unpackhi_epi32(t[4 * i + 1], t[4 * i + 3]);
  }

  __m512i q[32];

  EIGEN_UNROLL_LOOP
  for (int i = 0; i < 4; i++) {
    q[8 * i] = _mm512_unpacklo_epi64(p[8 * i], p[8 * i + 4]);
    q[8 * i + 1] = _mm512_unpackhi_epi64(p[8 * i], p[8 * i + 4]);
    q[8 * i + 2] = _mm512_unpacklo_epi64(p[8 * i + 1], p[8 * i + 5]);
    q[8 * i + 3] = _mm512_unpackhi_epi64(p[8 * i + 1], p[8 * i + 5]);
    q[8 * i + 4] = _mm512_unpacklo_epi64(p[8 * i + 2], p[8 * i + 6]);
    q[8 * i + 5] = _mm512_unpackhi_epi64(p[8 * i + 2], p[8 * i + 6]);
    q[8 * i + 6] = _mm512_unpacklo_epi64(p[8 * i + 3], p[8 * i + 7]);
    q[8 * i + 7] = _mm512_unpackhi_epi64(p[8 * i + 3], p[8 * i + 7]);
  }

  __m512i f[32];

#define PACKET32H_TRANSPOSE_HELPER(X, Y)                                                            \
  do {                                                                                              \
    f[Y * 8] = _mm512_inserti32x4(f[Y * 8], _mm512_extracti32x4_epi32(q[X * 8], Y), X);             \
    f[Y * 8 + 1] = _mm512_inserti32x4(f[Y * 8 + 1], _mm512_extracti32x4_epi32(q[X * 8 + 1], Y), X); \
    f[Y * 8 + 2] = _mm512_inserti32x4(f[Y * 8 + 2], _mm512_extracti32x4_epi32(q[X * 8 + 2], Y), X); \
    f[Y * 8 + 3] = _mm512_inserti32x4(f[Y * 8 + 3], _mm512_extracti32x4_epi32(q[X * 8 + 3], Y), X); \
    f[Y * 8 + 4] = _mm512_inserti32x4(f[Y * 8 + 4], _mm512_extracti32x4_epi32(q[X * 8 + 4], Y), X); \
    f[Y * 8 + 5] = _mm512_inserti32x4(f[Y * 8 + 5], _mm512_extracti32x4_epi32(q[X * 8 + 5], Y), X); \
    f[Y * 8 + 6] = _mm512_inserti32x4(f[Y * 8 + 6], _mm512_extracti32x4_epi32(q[X * 8 + 6], Y), X); \
    f[Y * 8 + 7] = _mm512_inserti32x4(f[Y * 8 + 7], _mm512_extracti32x4_epi32(q[X * 8 + 7], Y), X); \
  } while (false);

  PACKET32H_TRANSPOSE_HELPER(0, 0);
  PACKET32H_TRANSPOSE_HELPER(1, 1);
  PACKET32H_TRANSPOSE_HELPER(2, 2);
  PACKET32H_TRANSPOSE_HELPER(3, 3);

  PACKET32H_TRANSPOSE_HELPER(1, 0);
  PACKET32H_TRANSPOSE_HELPER(2, 0);
  PACKET32H_TRANSPOSE_HELPER(3, 0);
  PACKET32H_TRANSPOSE_HELPER(2, 1);
  PACKET32H_TRANSPOSE_HELPER(3, 1);
  PACKET32H_TRANSPOSE_HELPER(3, 2);

  PACKET32H_TRANSPOSE_HELPER(0, 1);
  PACKET32H_TRANSPOSE_HELPER(0, 2);
  PACKET32H_TRANSPOSE_HELPER(0, 3);
  PACKET32H_TRANSPOSE_HELPER(1, 2);
  PACKET32H_TRANSPOSE_HELPER(1, 3);
  PACKET32H_TRANSPOSE_HELPER(2, 3);

#undef PACKET32H_TRANSPOSE_HELPER

  EIGEN_UNROLL_LOOP
  for (int i = 0; i < 32; i++) {
    a.packet[i] = _mm512_castsi512_ph(f[i]);
  }
}

EIGEN_DEVICE_FUNC inline void ptranspose(PacketBlock<Packet32h, 4>& a) {
  __m512i p0, p1, p2, p3, t0, t1, t2, t3, a0, a1, a2, a3;
  t0 = _mm512_unpacklo_epi16(_mm512_castph_si512(a.packet[0]), _mm512_castph_si512(a.packet[1]));
  t1 = _mm512_unpackhi_epi16(_mm512_castph_si512(a.packet[0]), _mm512_castph_si512(a.packet[1]));
  t2 = _mm512_unpacklo_epi16(_mm512_castph_si512(a.packet[2]), _mm512_castph_si512(a.packet[3]));
  t3 = _mm512_unpackhi_epi16(_mm512_castph_si512(a.packet[2]), _mm512_castph_si512(a.packet[3]));

  p0 = _mm512_unpacklo_epi32(t0, t2);
  p1 = _mm512_unpackhi_epi32(t0, t2);
  p2 = _mm512_unpacklo_epi32(t1, t3);
  p3 = _mm512_unpackhi_epi32(t1, t3);

  a0 = p0;
  a1 = p1;
  a2 = p2;
  a3 = p3;

  a0 = _mm512_inserti32x4(a0, _mm512_extracti32x4_epi32(p1, 0), 1);
  a1 = _mm512_inserti32x4(a1, _mm512_extracti32x4_epi32(p0, 1), 0);

  a0 = _mm512_inserti32x4(a0, _mm512_extracti32x4_epi32(p2, 0), 2);
  a2 = _mm512_inserti32x4(a2, _mm512_extracti32x4_epi32(p0, 2), 0);

  a0 = _mm512_inserti32x4(a0, _mm512_extracti32x4_epi32(p3, 0), 3);
  a3 = _mm512_inserti32x4(a3, _mm512_extracti32x4_epi32(p0, 3), 0);

  a1 = _mm512_inserti32x4(a1, _mm512_extracti32x4_epi32(p2, 1), 2);
  a2 = _mm512_inserti32x4(a2, _mm512_extracti32x4_epi32(p1, 2), 1);

  a2 = _mm512_inserti32x4(a2, _mm512_extracti32x4_epi32(p3, 2), 3);
  a3 = _mm512_inserti32x4(a3, _mm512_extracti32x4_epi32(p2, 3), 2);

  a1 = _mm512_inserti32x4(a1, _mm512_extracti32x4_epi32(p3, 1), 3);
  a3 = _mm512_inserti32x4(a3, _mm512_extracti32x4_epi32(p1, 3), 1);

  a.packet[0] = _mm512_castsi512_ph(a0);
  a.packet[1] = _mm512_castsi512_ph(a1);
  a.packet[2] = _mm512_castsi512_ph(a2);
  a.packet[3] = _mm512_castsi512_ph(a3);
}

EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16h, 16>& kernel) {
  __m256i a = _mm256_castph_si256(kernel.packet[0]);
  __m256i b = _mm256_castph_si256(kernel.packet[1]);
  __m256i c = _mm256_castph_si256(kernel.packet[2]);
  __m256i d = _mm256_castph_si256(kernel.packet[3]);
  __m256i e = _mm256_castph_si256(kernel.packet[4]);
  __m256i f = _mm256_castph_si256(kernel.packet[5]);
  __m256i g = _mm256_castph_si256(kernel.packet[6]);
  __m256i h = _mm256_castph_si256(kernel.packet[7]);
  __m256i i = _mm256_castph_si256(kernel.packet[8]);
  __m256i j = _mm256_castph_si256(kernel.packet[9]);
  __m256i k = _mm256_castph_si256(kernel.packet[10]);
  __m256i l = _mm256_castph_si256(kernel.packet[11]);
  __m256i m = _mm256_castph_si256(kernel.packet[12]);
  __m256i n = _mm256_castph_si256(kernel.packet[13]);
  __m256i o = _mm256_castph_si256(kernel.packet[14]);
  __m256i p = _mm256_castph_si256(kernel.packet[15]);

  __m256i ab_07 = _mm256_unpacklo_epi16(a, b);
  __m256i cd_07 = _mm256_unpacklo_epi16(c, d);
  __m256i ef_07 = _mm256_unpacklo_epi16(e, f);
  __m256i gh_07 = _mm256_unpacklo_epi16(g, h);
  __m256i ij_07 = _mm256_unpacklo_epi16(i, j);
  __m256i kl_07 = _mm256_unpacklo_epi16(k, l);
  __m256i mn_07 = _mm256_unpacklo_epi16(m, n);
  __m256i op_07 = _mm256_unpacklo_epi16(o, p);

  __m256i ab_8f = _mm256_unpackhi_epi16(a, b);
  __m256i cd_8f = _mm256_unpackhi_epi16(c, d);
  __m256i ef_8f = _mm256_unpackhi_epi16(e, f);
  __m256i gh_8f = _mm256_unpackhi_epi16(g, h);
  __m256i ij_8f = _mm256_unpackhi_epi16(i, j);
  __m256i kl_8f = _mm256_unpackhi_epi16(k, l);
  __m256i mn_8f = _mm256_unpackhi_epi16(m, n);
  __m256i op_8f = _mm256_unpackhi_epi16(o, p);

  __m256i abcd_03 = _mm256_unpacklo_epi32(ab_07, cd_07);
  __m256i abcd_47 = _mm256_unpackhi_epi32(ab_07, cd_07);
  __m256i efgh_03 = _mm256_unpacklo_epi32(ef_07, gh_07);
  __m256i efgh_47 = _mm256_unpackhi_epi32(ef_07, gh_07);
  __m256i ijkl_03 = _mm256_unpacklo_epi32(ij_07, kl_07);
  __m256i ijkl_47 = _mm256_unpackhi_epi32(ij_07, kl_07);
  __m256i mnop_03 = _mm256_unpacklo_epi32(mn_07, op_07);
  __m256i mnop_47 = _mm256_unpackhi_epi32(mn_07, op_07);

  __m256i abcd_8b = _mm256_unpacklo_epi32(ab_8f, cd_8f);
  __m256i abcd_cf = _mm256_unpackhi_epi32(ab_8f, cd_8f);
  __m256i efgh_8b = _mm256_unpacklo_epi32(ef_8f, gh_8f);
  __m256i efgh_cf = _mm256_unpackhi_epi32(ef_8f, gh_8f);
  __m256i ijkl_8b = _mm256_unpacklo_epi32(ij_8f, kl_8f);
  __m256i ijkl_cf = _mm256_unpackhi_epi32(ij_8f, kl_8f);
  __m256i mnop_8b = _mm256_unpacklo_epi32(mn_8f, op_8f);
  __m256i mnop_cf = _mm256_unpackhi_epi32(mn_8f, op_8f);

  __m256i abcdefgh_01 = _mm256_unpacklo_epi64(abcd_03, efgh_03);
  __m256i abcdefgh_23 = _mm256_unpackhi_epi64(abcd_03, efgh_03);
  __m256i ijklmnop_01 = _mm256_unpacklo_epi64(ijkl_03, mnop_03);
  __m256i ijklmnop_23 = _mm256_unpackhi_epi64(ijkl_03, mnop_03);
  __m256i abcdefgh_45 = _mm256_unpacklo_epi64(abcd_47, efgh_47);
  __m256i abcdefgh_67 = _mm256_unpackhi_epi64(abcd_47, efgh_47);
  __m256i ijklmnop_45 = _mm256_unpacklo_epi64(ijkl_47, mnop_47);
  __m256i ijklmnop_67 = _mm256_unpackhi_epi64(ijkl_47, mnop_47);
  __m256i abcdefgh_89 = _mm256_unpacklo_epi64(abcd_8b, efgh_8b);
  __m256i abcdefgh_ab = _mm256_unpackhi_epi64(abcd_8b, efgh_8b);
  __m256i ijklmnop_89 = _mm256_unpacklo_epi64(ijkl_8b, mnop_8b);
  __m256i ijklmnop_ab = _mm256_unpackhi_epi64(ijkl_8b, mnop_8b);
  __m256i abcdefgh_cd = _mm256_unpacklo_epi64(abcd_cf, efgh_cf);
  __m256i abcdefgh_ef = _mm256_unpackhi_epi64(abcd_cf, efgh_cf);
  __m256i ijklmnop_cd = _mm256_unpacklo_epi64(ijkl_cf, mnop_cf);
  __m256i ijklmnop_ef = _mm256_unpackhi_epi64(ijkl_cf, mnop_cf);

  // NOTE: no unpacklo/hi instr in this case, so using permute instr.
  __m256i a_p_0 = _mm256_permute2x128_si256(abcdefgh_01, ijklmnop_01, 0x20);
  __m256i a_p_1 = _mm256_permute2x128_si256(abcdefgh_23, ijklmnop_23, 0x20);
  __m256i a_p_2 = _mm256_permute2x128_si256(abcdefgh_45, ijklmnop_45, 0x20);
  __m256i a_p_3 = _mm256_permute2x128_si256(abcdefgh_67, ijklmnop_67, 0x20);
  __m256i a_p_4 = _mm256_permute2x128_si256(abcdefgh_89, ijklmnop_89, 0x20);
  __m256i a_p_5 = _mm256_permute2x128_si256(abcdefgh_ab, ijklmnop_ab, 0x20);
  __m256i a_p_6 = _mm256_permute2x128_si256(abcdefgh_cd, ijklmnop_cd, 0x20);
  __m256i a_p_7 = _mm256_permute2x128_si256(abcdefgh_ef, ijklmnop_ef, 0x20);
  __m256i a_p_8 = _mm256_permute2x128_si256(abcdefgh_01, ijklmnop_01, 0x31);
  __m256i a_p_9 = _mm256_permute2x128_si256(abcdefgh_23, ijklmnop_23, 0x31);
  __m256i a_p_a = _mm256_permute2x128_si256(abcdefgh_45, ijklmnop_45, 0x31);
  __m256i a_p_b = _mm256_permute2x128_si256(abcdefgh_67, ijklmnop_67, 0x31);
  __m256i a_p_c = _mm256_permute2x128_si256(abcdefgh_89, ijklmnop_89, 0x31);
  __m256i a_p_d = _mm256_permute2x128_si256(abcdefgh_ab, ijklmnop_ab, 0x31);
  __m256i a_p_e = _mm256_permute2x128_si256(abcdefgh_cd, ijklmnop_cd, 0x31);
  __m256i a_p_f = _mm256_permute2x128_si256(abcdefgh_ef, ijklmnop_ef, 0x31);

  kernel.packet[0] = _mm256_castsi256_ph(a_p_0);
  kernel.packet[1] = _mm256_castsi256_ph(a_p_1);
  kernel.packet[2] = _mm256_castsi256_ph(a_p_2);
  kernel.packet[3] = _mm256_castsi256_ph(a_p_3);
  kernel.packet[4] = _mm256_castsi256_ph(a_p_4);
  kernel.packet[5] = _mm256_castsi256_ph(a_p_5);
  kernel.packet[6] = _mm256_castsi256_ph(a_p_6);
  kernel.packet[7] = _mm256_castsi256_ph(a_p_7);
  kernel.packet[8] = _mm256_castsi256_ph(a_p_8);
  kernel.packet[9] = _mm256_castsi256_ph(a_p_9);
  kernel.packet[10] = _mm256_castsi256_ph(a_p_a);
  kernel.packet[11] = _mm256_castsi256_ph(a_p_b);
  kernel.packet[12] = _mm256_castsi256_ph(a_p_c);
  kernel.packet[13] = _mm256_castsi256_ph(a_p_d);
  kernel.packet[14] = _mm256_castsi256_ph(a_p_e);
  kernel.packet[15] = _mm256_castsi256_ph(a_p_f);
}

EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16h, 8>& kernel) {
  EIGEN_ALIGN64 half in[8][16];
  pstore<half>(in[0], kernel.packet[0]);
  pstore<half>(in[1], kernel.packet[1]);
  pstore<half>(in[2], kernel.packet[2]);
  pstore<half>(in[3], kernel.packet[3]);
  pstore<half>(in[4], kernel.packet[4]);
  pstore<half>(in[5], kernel.packet[5]);
  pstore<half>(in[6], kernel.packet[6]);
  pstore<half>(in[7], kernel.packet[7]);

  EIGEN_ALIGN64 half out[8][16];

  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
      out[i][j] = in[j][2 * i];
    }
    for (int j = 0; j < 8; ++j) {
      out[i][j + 8] = in[j][2 * i + 1];
    }
  }

  kernel.packet[0] = pload<Packet16h>(out[0]);
  kernel.packet[1] = pload<Packet16h>(out[1]);
  kernel.packet[2] = pload<Packet16h>(out[2]);
  kernel.packet[3] = pload<Packet16h>(out[3]);
  kernel.packet[4] = pload<Packet16h>(out[4]);
  kernel.packet[5] = pload<Packet16h>(out[5]);
  kernel.packet[6] = pload<Packet16h>(out[6]);
  kernel.packet[7] = pload<Packet16h>(out[7]);
}

EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet16h, 4>& kernel) {
  EIGEN_ALIGN64 half in[4][16];
  pstore<half>(in[0], kernel.packet[0]);
  pstore<half>(in[1], kernel.packet[1]);
  pstore<half>(in[2], kernel.packet[2]);
  pstore<half>(in[3], kernel.packet[3]);

  EIGEN_ALIGN64 half out[4][16];

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      out[i][j] = in[j][4 * i];
    }
    for (int j = 0; j < 4; ++j) {
      out[i][j + 4] = in[j][4 * i + 1];
    }
    for (int j = 0; j < 4; ++j) {
      out[i][j + 8] = in[j][4 * i + 2];
    }
    for (int j = 0; j < 4; ++j) {
      out[i][j + 12] = in[j][4 * i + 3];
    }
  }

  kernel.packet[0] = pload<Packet16h>(out[0]);
  kernel.packet[1] = pload<Packet16h>(out[1]);
  kernel.packet[2] = pload<Packet16h>(out[2]);
  kernel.packet[3] = pload<Packet16h>(out[3]);
}

EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet8h, 8>& kernel) {
  __m128i a = _mm_castph_si128(kernel.packet[0]);
  __m128i b = _mm_castph_si128(kernel.packet[1]);
  __m128i c = _mm_castph_si128(kernel.packet[2]);
  __m128i d = _mm_castph_si128(kernel.packet[3]);
  __m128i e = _mm_castph_si128(kernel.packet[4]);
  __m128i f = _mm_castph_si128(kernel.packet[5]);
  __m128i g = _mm_castph_si128(kernel.packet[6]);
  __m128i h = _mm_castph_si128(kernel.packet[7]);

  __m128i a03b03 = _mm_unpacklo_epi16(a, b);
  __m128i c03d03 = _mm_unpacklo_epi16(c, d);
  __m128i e03f03 = _mm_unpacklo_epi16(e, f);
  __m128i g03h03 = _mm_unpacklo_epi16(g, h);
  __m128i a47b47 = _mm_unpackhi_epi16(a, b);
  __m128i c47d47 = _mm_unpackhi_epi16(c, d);
  __m128i e47f47 = _mm_unpackhi_epi16(e, f);
  __m128i g47h47 = _mm_unpackhi_epi16(g, h);

  __m128i a01b01c01d01 = _mm_unpacklo_epi32(a03b03, c03d03);
  __m128i a23b23c23d23 = _mm_unpackhi_epi32(a03b03, c03d03);
  __m128i e01f01g01h01 = _mm_unpacklo_epi32(e03f03, g03h03);
  __m128i e23f23g23h23 = _mm_unpackhi_epi32(e03f03, g03h03);
  __m128i a45b45c45d45 = _mm_unpacklo_epi32(a47b47, c47d47);
  __m128i a67b67c67d67 = _mm_unpackhi_epi32(a47b47, c47d47);
  __m128i e45f45g45h45 = _mm_unpacklo_epi32(e47f47, g47h47);
  __m128i e67f67g67h67 = _mm_unpackhi_epi32(e47f47, g47h47);

  __m128i a0b0c0d0e0f0g0h0 = _mm_unpacklo_epi64(a01b01c01d01, e01f01g01h01);
  __m128i a1b1c1d1e1f1g1h1 = _mm_unpackhi_epi64(a01b01c01d01, e01f01g01h01);
  __m128i a2b2c2d2e2f2g2h2 = _mm_unpacklo_epi64(a23b23c23d23, e23f23g23h23);
  __m128i a3b3c3d3e3f3g3h3 = _mm_unpackhi_epi64(a23b23c23d23, e23f23g23h23);
  __m128i a4b4c4d4e4f4g4h4 = _mm_unpacklo_epi64(a45b45c45d45, e45f45g45h45);
  __m128i a5b5c5d5e5f5g5h5 = _mm_unpackhi_epi64(a45b45c45d45, e45f45g45h45);
  __m128i a6b6c6d6e6f6g6h6 = _mm_unpacklo_epi64(a67b67c67d67, e67f67g67h67);
  __m128i a7b7c7d7e7f7g7h7 = _mm_unpackhi_epi64(a67b67c67d67, e67f67g67h67);

  kernel.packet[0] = _mm_castsi128_ph(a0b0c0d0e0f0g0h0);
  kernel.packet[1] = _mm_castsi128_ph(a1b1c1d1e1f1g1h1);
  kernel.packet[2] = _mm_castsi128_ph(a2b2c2d2e2f2g2h2);
  kernel.packet[3] = _mm_castsi128_ph(a3b3c3d3e3f3g3h3);
  kernel.packet[4] = _mm_castsi128_ph(a4b4c4d4e4f4g4h4);
  kernel.packet[5] = _mm_castsi128_ph(a5b5c5d5e5f5g5h5);
  kernel.packet[6] = _mm_castsi128_ph(a6b6c6d6e6f6g6h6);
  kernel.packet[7] = _mm_castsi128_ph(a7b7c7d7e7f7g7h7);
}

EIGEN_STRONG_INLINE void ptranspose(PacketBlock<Packet8h, 4>& kernel) {
  EIGEN_ALIGN32 Eigen::half in[4][8];
  pstore<Eigen::half>(in[0], kernel.packet[0]);
  pstore<Eigen::half>(in[1], kernel.packet[1]);
  pstore<Eigen::half>(in[2], kernel.packet[2]);
  pstore<Eigen::half>(in[3], kernel.packet[3]);

  EIGEN_ALIGN32 Eigen::half out[4][8];

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      out[i][j] = in[j][2 * i];
    }
    for (int j = 0; j < 4; ++j) {
      out[i][j + 4] = in[j][2 * i + 1];
    }
  }

  kernel.packet[0] = pload<Packet8h>(out[0]);
  kernel.packet[1] = pload<Packet8h>(out[1]);
  kernel.packet[2] = pload<Packet8h>(out[2]);
  kernel.packet[3] = pload<Packet8h>(out[3]);
}

// preverse

template <>
EIGEN_STRONG_INLINE Packet32h preverse(const Packet32h& a) {
  return _mm512_permutexvar_ph(_mm512_set_epi16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                                20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31),
                               a);
}

template <>
EIGEN_STRONG_INLINE Packet16h preverse(const Packet16h& a) {
  __m128i m = _mm_setr_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
  return _mm256_castsi256_ph(_mm256_insertf128_si256(
      _mm256_castsi128_si256(_mm_shuffle_epi8(_mm256_extractf128_si256(_mm256_castph_si256(a), 1), m)),
      _mm_shuffle_epi8(_mm256_extractf128_si256(_mm256_castph_si256(a), 0), m), 1));
}

template <>
EIGEN_STRONG_INLINE Packet8h preverse(const Packet8h& a) {
  __m128i m = _mm_setr_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
  return _mm_castsi128_ph(_mm_shuffle_epi8(_mm_castph_si128(a), m));
}

// pscatter

template <>
EIGEN_STRONG_INLINE void pscatter<half, Packet32h>(half* to, const Packet32h& from, Index stride) {
  EIGEN_ALIGN64 half aux[32];
  pstore(aux, from);

  EIGEN_UNROLL_LOOP
  for (int i = 0; i < 32; i++) {
    to[stride * i] = aux[i];
  }
}
template <>
EIGEN_STRONG_INLINE void pscatter<half, Packet16h>(half* to, const Packet16h& from, Index stride) {
  EIGEN_ALIGN64 half aux[16];
  pstore(aux, from);
  to[stride * 0] = aux[0];
  to[stride * 1] = aux[1];
  to[stride * 2] = aux[2];
  to[stride * 3] = aux[3];
  to[stride * 4] = aux[4];
  to[stride * 5] = aux[5];
  to[stride * 6] = aux[6];
  to[stride * 7] = aux[7];
  to[stride * 8] = aux[8];
  to[stride * 9] = aux[9];
  to[stride * 10] = aux[10];
  to[stride * 11] = aux[11];
  to[stride * 12] = aux[12];
  to[stride * 13] = aux[13];
  to[stride * 14] = aux[14];
  to[stride * 15] = aux[15];
}

template <>
EIGEN_STRONG_INLINE void pscatter<Eigen::half, Packet8h>(Eigen::half* to, const Packet8h& from, Index stride) {
  EIGEN_ALIGN32 Eigen::half aux[8];
  pstore(aux, from);
  to[stride * 0] = aux[0];
  to[stride * 1] = aux[1];
  to[stride * 2] = aux[2];
  to[stride * 3] = aux[3];
  to[stride * 4] = aux[4];
  to[stride * 5] = aux[5];
  to[stride * 6] = aux[6];
  to[stride * 7] = aux[7];
}

// pgather

template <>
EIGEN_STRONG_INLINE Packet32h pgather<Eigen::half, Packet32h>(const Eigen::half* from, Index stride) {
  return _mm512_set_ph(from[31 * stride].x, from[30 * stride].x, from[29 * stride].x, from[28 * stride].x,
                       from[27 * stride].x, from[26 * stride].x, from[25 * stride].x, from[24 * stride].x,
                       from[23 * stride].x, from[22 * stride].x, from[21 * stride].x, from[20 * stride].x,
                       from[19 * stride].x, from[18 * stride].x, from[17 * stride].x, from[16 * stride].x,
                       from[15 * stride].x, from[14 * stride].x, from[13 * stride].x, from[12 * stride].x,
                       from[11 * stride].x, from[10 * stride].x, from[9 * stride].x, from[8 * stride].x,
                       from[7 * stride].x, from[6 * stride].x, from[5 * stride].x, from[4 * stride].x,
                       from[3 * stride].x, from[2 * stride].x, from[1 * stride].x, from[0 * stride].x);
}

template <>
EIGEN_STRONG_INLINE Packet16h pgather<Eigen::half, Packet16h>(const Eigen::half* from, Index stride) {
  return _mm256_set_ph(from[15 * stride].x, from[14 * stride].x, from[13 * stride].x, from[12 * stride].x,
                       from[11 * stride].x, from[10 * stride].x, from[9 * stride].x, from[8 * stride].x,
                       from[7 * stride].x, from[6 * stride].x, from[5 * stride].x, from[4 * stride].x,
                       from[3 * stride].x, from[2 * stride].x, from[1 * stride].x, from[0 * stride].x);
}

template <>
EIGEN_STRONG_INLINE Packet8h pgather<Eigen::half, Packet8h>(const Eigen::half* from, Index stride) {
  return _mm_set_ph(from[7 * stride].x, from[6 * stride].x, from[5 * stride].x, from[4 * stride].x, from[3 * stride].x,
                    from[2 * stride].x, from[1 * stride].x, from[0 * stride].x);
}

}  // end namespace internal
}  // end namespace Eigen

#endif  // EIGEN_PACKET_MATH_FP16_AVX512_H
