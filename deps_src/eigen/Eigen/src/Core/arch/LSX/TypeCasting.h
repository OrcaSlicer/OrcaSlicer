// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2023 Zang Ruochen <zangruochen@loongson.cn>
// Copyright (C) 2024 XiWei Gu <guxiwei-hf@loongson.cn>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_TYPE_CASTING_LSX_H
#define EIGEN_TYPE_CASTING_LSX_H

// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

//==============================================================================
// preinterpret
//==============================================================================
template <>
EIGEN_STRONG_INLINE Packet4f preinterpret<Packet4f, Packet4i>(const Packet4i& a) {
  return (__m128)((__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet4f preinterpret<Packet4f, Packet4ui>(const Packet4ui& a) {
  return (__m128)((__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet2d preinterpret<Packet2d, Packet2l>(const Packet2l& a) {
  return (__m128d)((__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet2d preinterpret<Packet2d, Packet2ul>(const Packet2ul& a) {
  return (__m128d)((__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet2d preinterpret<Packet2d, Packet4i>(const Packet4i& a) {
  return (__m128d)((__m128i)a);
}
template <>
EIGEN_STRONG_INLINE Packet16c preinterpret<Packet16c, Packet16uc>(const Packet16uc& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet8s preinterpret<Packet8s, Packet8us>(const Packet8us& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet4i preinterpret<Packet4i, Packet4f>(const Packet4f& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet4i preinterpret<Packet4i, Packet4ui>(const Packet4ui& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet4i preinterpret<Packet4i, Packet2d>(const Packet2d& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet2l preinterpret<Packet2l, Packet2d>(const Packet2d& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet16uc preinterpret<Packet16uc, Packet16c>(const Packet16c& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet8us preinterpret<Packet8us, Packet8s>(const Packet8s& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet4ui preinterpret<Packet4ui, Packet4f>(const Packet4f& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet4ui preinterpret<Packet4ui, Packet4i>(const Packet4i& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet2ul preinterpret<Packet2ul, Packet2d>(const Packet2d& a) {
  return (__m128i)a;
}
template <>
EIGEN_STRONG_INLINE Packet2ul preinterpret<Packet2ul, Packet2l>(const Packet2l& a) {
  return (__m128i)a;
}

template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet4f, Packet2l>(const Packet4f& a) {
  Packet2d tmp = __lsx_vfcvtl_d_s(a);
  return __lsx_vftint_l_d(tmp);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet4f, Packet2ul>(const Packet4f& a) {
  Packet2d tmp = __lsx_vfcvtl_d_s(a);
  return __lsx_vftint_lu_d(tmp);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet4f, Packet4i>(const Packet4f& a) {
  return __lsx_vftint_w_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet4f, Packet4ui>(const Packet4f& a) {
  return __lsx_vftint_wu_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet4f, Packet8s>(const Packet4f& a, const Packet4f& b) {
  return __lsx_vssrlni_h_w(__lsx_vftint_w_s(a), __lsx_vftint_w_s(b), 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet4f, Packet8us>(const Packet4f& a, const Packet4f& b) {
  return __lsx_vssrlni_hu_w(__lsx_vftint_wu_s(a), __lsx_vftint_wu_s(b), 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet4f, Packet16c>(const Packet4f& a, const Packet4f& b, const Packet4f& c,
                                                         const Packet4f& d) {
  Packet8s tmp1 = __lsx_vssrlni_h_w(__lsx_vftint_w_s(a), __lsx_vftint_w_s(b), 0);
  Packet8s tmp2 = __lsx_vssrlni_h_w(__lsx_vftint_w_s(c), __lsx_vftint_w_s(d), 0);
  return __lsx_vssrlni_b_h((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet4f, Packet16uc>(const Packet4f& a, const Packet4f& b, const Packet4f& c,
                                                           const Packet4f& d) {
  Packet8us tmp1 = __lsx_vssrlni_hu_w(__lsx_vftint_wu_s(a), __lsx_vftint_wu_s(b), 0);
  Packet8us tmp2 = __lsx_vssrlni_hu_w(__lsx_vftint_wu_s(c), __lsx_vftint_wu_s(d), 0);
  return __lsx_vssrlni_bu_h((__m128i)tmp1, (__m128i)tmp2, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet16c, Packet4f>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vsllwil_h_b((__m128i)a, 0);
  Packet4i tmp2 = __lsx_vsllwil_w_h((__m128i)tmp1, 0);
  return __lsx_vffint_s_w(tmp2);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet16c, Packet2l>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vsllwil_h_b((__m128i)a, 0);
  Packet4i tmp2 = __lsx_vsllwil_w_h((__m128i)tmp1, 0);
  return __lsx_vsllwil_d_w((__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet16c, Packet2ul>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vsllwil_h_b((__m128i)a, 0);
  Packet4i tmp2 = __lsx_vsllwil_w_h((__m128i)tmp1, 0);
  return (Packet2ul)__lsx_vsllwil_d_w((__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet16c, Packet4i>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vsllwil_h_b((__m128i)a, 0);
  return __lsx_vsllwil_w_h((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet16c, Packet4ui>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vsllwil_h_b((__m128i)a, 0);
  return (Packet4ui)__lsx_vsllwil_w_h((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet16c, Packet8s>(const Packet16c& a) {
  return __lsx_vsllwil_h_b((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet16c, Packet8us>(const Packet16c& a) {
  return (Packet8us)__lsx_vsllwil_h_b((__m128i)a, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet16uc, Packet4f>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vsllwil_hu_bu((__m128i)a, 0);
  Packet4ui tmp2 = __lsx_vsllwil_wu_hu((__m128i)tmp1, 0);
  return __lsx_vffint_s_wu(tmp2);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet16uc, Packet2ul>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vsllwil_hu_bu((__m128i)a, 0);
  Packet4ui tmp2 = __lsx_vsllwil_wu_hu((__m128i)tmp1, 0);
  return __lsx_vsllwil_du_wu((__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet16uc, Packet2l>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vsllwil_hu_bu((__m128i)a, 0);
  Packet4ui tmp2 = __lsx_vsllwil_wu_hu((__m128i)tmp1, 0);
  return (Packet2l)__lsx_vsllwil_du_wu((__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet16uc, Packet4ui>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vsllwil_hu_bu((__m128i)a, 0);
  return __lsx_vsllwil_wu_hu((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet16uc, Packet4i>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vsllwil_hu_bu((__m128i)a, 0);
  return (Packet4i)__lsx_vsllwil_wu_hu((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet16uc, Packet8us>(const Packet16uc& a) {
  return __lsx_vsllwil_hu_bu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet16uc, Packet8s>(const Packet16uc& a) {
  return (Packet8s)__lsx_vsllwil_hu_bu((__m128i)a, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet8s, Packet4f>(const Packet8s& a) {
  Packet4i tmp1 = __lsx_vsllwil_w_h((__m128i)a, 0);
  return __lsx_vffint_s_w(tmp1);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet8s, Packet2l>(const Packet8s& a) {
  Packet4i tmp1 = __lsx_vsllwil_w_h((__m128i)a, 0);
  return __lsx_vsllwil_d_w((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet8s, Packet2ul>(const Packet8s& a) {
  Packet4i tmp1 = __lsx_vsllwil_w_h((__m128i)a, 0);
  return (Packet2ul)__lsx_vsllwil_d_w((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet8s, Packet4i>(const Packet8s& a) {
  return __lsx_vsllwil_w_h((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet8s, Packet4ui>(const Packet8s& a) {
  return (Packet4ui)__lsx_vsllwil_w_h((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet8s, Packet16c>(const Packet8s& a, const Packet8s& b) {
  return __lsx_vssrlni_b_h((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet8s, Packet16uc>(const Packet8s& a, const Packet8s& b) {
  return (Packet16uc)__lsx_vssrlni_b_h((__m128i)a, (__m128i)b, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet8us, Packet4f>(const Packet8us& a) {
  Packet4ui tmp1 = __lsx_vsllwil_wu_hu((__m128i)a, 0);
  return __lsx_vffint_s_wu(tmp1);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet8us, Packet2ul>(const Packet8us& a) {
  Packet4ui tmp1 = __lsx_vsllwil_wu_hu((__m128i)a, 0);
  return __lsx_vsllwil_du_wu((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet8us, Packet2l>(const Packet8us& a) {
  Packet4ui tmp1 = __lsx_vsllwil_wu_hu((__m128i)a, 0);
  return (Packet2l)__lsx_vsllwil_du_wu((__m128i)tmp1, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet8us, Packet4ui>(const Packet8us& a) {
  return __lsx_vsllwil_wu_hu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet8us, Packet4i>(const Packet8us& a) {
  return (Packet4i)__lsx_vsllwil_wu_hu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet8us, Packet16uc>(const Packet8us& a, const Packet8us& b) {
  return __lsx_vssrlni_bu_h((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet8us, Packet16c>(const Packet8us& a, const Packet8us& b) {
  return (Packet16c)__lsx_vssrlni_bu_h((__m128i)a, (__m128i)b, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet4i, Packet4f>(const Packet4i& a) {
  return __lsx_vffint_s_w(a);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet4i, Packet2l>(const Packet4i& a) {
  return __lsx_vsllwil_d_w((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet4i, Packet2ul>(const Packet4i& a) {
  return (Packet2ul)__lsx_vsllwil_d_w((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet4i, Packet8s>(const Packet4i& a, const Packet4i& b) {
  return __lsx_vssrlni_h_w((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet4i, Packet8us>(const Packet4i& a, const Packet4i& b) {
  return (Packet8us)__lsx_vssrlni_h_w((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet4i, Packet16c>(const Packet4i& a, const Packet4i& b, const Packet4i& c,
                                                         const Packet4i& d) {
  Packet8s tmp1 = __lsx_vssrlni_h_w((__m128i)a, (__m128i)b, 0);
  Packet8s tmp2 = __lsx_vssrlni_h_w((__m128i)c, (__m128i)d, 0);
  return __lsx_vssrlni_b_h((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet4i, Packet16uc>(const Packet4i& a, const Packet4i& b, const Packet4i& c,
                                                           const Packet4i& d) {
  Packet8s tmp1 = __lsx_vssrlni_h_w((__m128i)a, (__m128i)b, 0);
  Packet8s tmp2 = __lsx_vssrlni_h_w((__m128i)c, (__m128i)d, 0);
  return (Packet16uc)__lsx_vssrlni_b_h((__m128i)tmp1, (__m128i)tmp2, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet4ui, Packet4f>(const Packet4ui& a) {
  return __lsx_vffint_s_wu(a);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet4ui, Packet2ul>(const Packet4ui& a) {
  return __lsx_vsllwil_du_wu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet4ui, Packet2l>(const Packet4ui& a) {
  return (Packet2l)__lsx_vsllwil_du_wu((__m128i)a, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet4ui, Packet8us>(const Packet4ui& a, const Packet4ui& b) {
  return __lsx_vssrlni_hu_w((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet4ui, Packet8s>(const Packet4ui& a, const Packet4ui& b) {
  return (Packet8s)__lsx_vssrlni_hu_w((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet4ui, Packet16uc>(const Packet4ui& a, const Packet4ui& b, const Packet4ui& c,
                                                            const Packet4ui& d) {
  Packet8us tmp1 = __lsx_vssrlni_hu_w((__m128i)a, (__m128i)b, 0);
  Packet8us tmp2 = __lsx_vssrlni_hu_w((__m128i)c, (__m128i)d, 0);
  return __lsx_vssrlni_bu_h((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet4ui, Packet16c>(const Packet4ui& a, const Packet4ui& b, const Packet4ui& c,
                                                          const Packet4ui& d) {
  Packet8us tmp1 = __lsx_vssrlni_hu_w((__m128i)a, (__m128i)b, 0);
  Packet8us tmp2 = __lsx_vssrlni_hu_w((__m128i)c, (__m128i)d, 0);
  return (Packet16c)__lsx_vssrlni_bu_h((__m128i)tmp1, (__m128i)tmp2, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet2l, Packet4f>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vffint_s_w(__lsx_vssrlni_w_d((__m128i)a, (__m128i)b, 0));
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet2l, Packet4i>(const Packet2l& a, const Packet2l& b) {
  return __lsx_vssrlni_w_d((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet2l, Packet4ui>(const Packet2l& a, const Packet2l& b) {
  return (Packet4ui)__lsx_vssrlni_w_d((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet2l, Packet8s>(const Packet2l& a, const Packet2l& b, const Packet2l& c,
                                                       const Packet2l& d) {
  Packet4i tmp1 = __lsx_vssrlni_w_d((__m128i)a, (__m128i)b, 0);
  Packet4i tmp2 = __lsx_vssrlni_w_d((__m128i)c, (__m128i)d, 0);
  return __lsx_vssrlni_h_w((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet2l, Packet8us>(const Packet2l& a, const Packet2l& b, const Packet2l& c,
                                                         const Packet2l& d) {
  Packet4i tmp1 = __lsx_vssrlni_w_d((__m128i)a, (__m128i)b, 0);
  Packet4i tmp2 = __lsx_vssrlni_w_d((__m128i)c, (__m128i)d, 0);
  return (Packet8us)__lsx_vssrlni_h_w((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet2l, Packet16c>(const Packet2l& a, const Packet2l& b, const Packet2l& c,
                                                         const Packet2l& d, const Packet2l& e, const Packet2l& f,
                                                         const Packet2l& g, const Packet2l& h) {
  const Packet8s abcd = pcast<Packet2l, Packet8s>(a, b, c, d);
  const Packet8s efgh = pcast<Packet2l, Packet8s>(e, f, g, h);
  return __lsx_vssrlni_b_h((__m128i)abcd, (__m128i)efgh, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet2l, Packet16uc>(const Packet2l& a, const Packet2l& b, const Packet2l& c,
                                                           const Packet2l& d, const Packet2l& e, const Packet2l& f,
                                                           const Packet2l& g, const Packet2l& h) {
  const Packet8us abcd = pcast<Packet2l, Packet8us>(a, b, c, d);
  const Packet8us efgh = pcast<Packet2l, Packet8us>(e, f, g, h);
  return __lsx_vssrlni_bu_h((__m128i)abcd, (__m128i)efgh, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet2ul, Packet4f>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vffint_s_wu(__lsx_vssrlni_w_d((__m128i)a, (__m128i)b, 0));
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet2ul, Packet4ui>(const Packet2ul& a, const Packet2ul& b) {
  return __lsx_vssrlni_wu_d((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet2ul, Packet4i>(const Packet2ul& a, const Packet2ul& b) {
  return (Packet4i)__lsx_vssrlni_wu_d((__m128i)a, (__m128i)b, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet2ul, Packet8us>(const Packet2ul& a, const Packet2ul& b, const Packet2ul& c,
                                                          const Packet2ul& d) {
  Packet4ui tmp1 = __lsx_vssrlni_wu_d((__m128i)a, (__m128i)b, 0);
  Packet4ui tmp2 = __lsx_vssrlni_wu_d((__m128i)c, (__m128i)d, 0);
  return __lsx_vssrlni_hu_w((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet2ul, Packet8s>(const Packet2ul& a, const Packet2ul& b, const Packet2ul& c,
                                                        const Packet2ul& d) {
  Packet4ui tmp1 = __lsx_vssrlni_wu_d((__m128i)a, (__m128i)b, 0);
  Packet4ui tmp2 = __lsx_vssrlni_wu_d((__m128i)c, (__m128i)d, 0);
  return (Packet8s)__lsx_vssrlni_hu_w((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet2ul, Packet16uc>(const Packet2ul& a, const Packet2ul& b, const Packet2ul& c,
                                                            const Packet2ul& d, const Packet2ul& e, const Packet2ul& f,
                                                            const Packet2ul& g, const Packet2ul& h) {
  const Packet8s abcd = pcast<Packet2ul, Packet8s>(a, b, c, d);
  const Packet8s efgh = pcast<Packet2ul, Packet8s>(e, f, g, h);
  return __lsx_vssrlni_b_h((__m128i)abcd, (__m128i)efgh, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet2ul, Packet16c>(const Packet2ul& a, const Packet2ul& b, const Packet2ul& c,
                                                          const Packet2ul& d, const Packet2ul& e, const Packet2ul& f,
                                                          const Packet2ul& g, const Packet2ul& h) {
  const Packet8us abcd = pcast<Packet2ul, Packet8us>(a, b, c, d);
  const Packet8us efgh = pcast<Packet2ul, Packet8us>(e, f, g, h);
  return __lsx_vssrlni_bu_h((__m128i)abcd, (__m128i)efgh, 0);
}

template <>
EIGEN_STRONG_INLINE Packet4f pcast<Packet2d, Packet4f>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vfcvt_s_d(b, a);
}
template <>
EIGEN_STRONG_INLINE Packet2l pcast<Packet2d, Packet2l>(const Packet2d& a) {
  return __lsx_vftint_l_d(a);
}
template <>
EIGEN_STRONG_INLINE Packet2ul pcast<Packet2d, Packet2ul>(const Packet2d& a) {
  return __lsx_vftint_lu_d(a);
}
template <>
EIGEN_STRONG_INLINE Packet4i pcast<Packet2d, Packet4i>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vssrlni_w_d(__lsx_vftint_l_d(a), __lsx_vftint_l_d(b), 0);
}
template <>
EIGEN_STRONG_INLINE Packet4ui pcast<Packet2d, Packet4ui>(const Packet2d& a, const Packet2d& b) {
  return __lsx_vssrlni_wu_d(__lsx_vftint_lu_d(a), __lsx_vftint_lu_d(b), 0);
}
template <>
EIGEN_STRONG_INLINE Packet8s pcast<Packet2d, Packet8s>(const Packet2d& a, const Packet2d& b, const Packet2d& c,
                                                       const Packet2d& d) {
  Packet4i tmp1 = __lsx_vssrlni_w_d(__lsx_vftint_l_d(a), __lsx_vftint_l_d(b), 0);
  Packet4i tmp2 = __lsx_vssrlni_w_d(__lsx_vftint_l_d(c), __lsx_vftint_l_d(d), 0);
  return __lsx_vssrlni_h_w((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet8us pcast<Packet2d, Packet8us>(const Packet2d& a, const Packet2d& b, const Packet2d& c,
                                                         const Packet2d& d) {
  Packet4ui tmp1 = __lsx_vssrlni_wu_d(__lsx_vftint_lu_d(a), __lsx_vftint_lu_d(b), 0);
  Packet4ui tmp2 = __lsx_vssrlni_wu_d(__lsx_vftint_lu_d(c), __lsx_vftint_lu_d(d), 0);
  return __lsx_vssrlni_hu_w((__m128i)tmp1, (__m128i)tmp2, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16c pcast<Packet2d, Packet16c>(const Packet2d& a, const Packet2d& b, const Packet2d& c,
                                                         const Packet2d& d, const Packet2d& e, const Packet2d& f,
                                                         const Packet2d& g, const Packet2d& h) {
  const Packet8s abcd = pcast<Packet2d, Packet8s>(a, b, c, d);
  const Packet8s efgh = pcast<Packet2d, Packet8s>(e, f, g, h);
  return __lsx_vssrlni_b_h((__m128i)abcd, (__m128i)efgh, 0);
}
template <>
EIGEN_STRONG_INLINE Packet16uc pcast<Packet2d, Packet16uc>(const Packet2d& a, const Packet2d& b, const Packet2d& c,
                                                           const Packet2d& d, const Packet2d& e, const Packet2d& f,
                                                           const Packet2d& g, const Packet2d& h) {
  const Packet8us abcd = pcast<Packet2d, Packet8us>(a, b, c, d);
  const Packet8us efgh = pcast<Packet2d, Packet8us>(e, f, g, h);
  return __lsx_vssrlni_bu_h((__m128i)abcd, (__m128i)efgh, 0);
}

template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet4f, Packet2d>(const Packet4f& a) {
  return __lsx_vfcvtl_d_s(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet16c, Packet2d>(const Packet16c& a) {
  Packet8s tmp1 = __lsx_vsllwil_h_b((__m128i)a, 0);
  Packet4i tmp2 = __lsx_vsllwil_w_h((__m128i)tmp1, 0);
  return __lsx_vffint_d_l(__lsx_vsllwil_d_w((__m128i)tmp2, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet16uc, Packet2d>(const Packet16uc& a) {
  Packet8us tmp1 = __lsx_vsllwil_hu_bu((__m128i)a, 0);
  Packet4ui tmp2 = __lsx_vsllwil_wu_hu((__m128i)tmp1, 0);
  return __lsx_vffint_d_lu(__lsx_vsllwil_du_wu((__m128i)tmp2, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet8s, Packet2d>(const Packet8s& a) {
  Packet4i tmp = __lsx_vsllwil_w_h((__m128i)a, 0);
  return __lsx_vffint_d_l(__lsx_vsllwil_d_w((__m128i)tmp, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet8us, Packet2d>(const Packet8us& a) {
  Packet4ui tmp = __lsx_vsllwil_wu_hu((__m128i)a, 0);
  return __lsx_vffint_d_lu(__lsx_vsllwil_du_wu((__m128i)tmp, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet4i, Packet2d>(const Packet4i& a) {
  return __lsx_vffint_d_l(__lsx_vsllwil_d_w((__m128i)a, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet4ui, Packet2d>(const Packet4ui& a) {
  return __lsx_vffint_d_lu(__lsx_vsllwil_du_wu((__m128i)a, 0));
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet2l, Packet2d>(const Packet2l& a) {
  return __lsx_vffint_d_l(a);
}
template <>
EIGEN_STRONG_INLINE Packet2d pcast<Packet2ul, Packet2d>(const Packet2ul& a) {
  return __lsx_vffint_d_lu(a);
}

}  // end namespace internal

}  // end namespace Eigen

#endif  // EIGEN_TYPE_CASTING_LSX_H
