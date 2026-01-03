// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 Charlie Schlosser <cs.schlosser@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_FIND_COEFF_H
#define EIGEN_FIND_COEFF_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

template <typename Scalar, int NaNPropagation, bool IsInteger = NumTraits<Scalar>::IsInteger>
struct max_coeff_functor {
  EIGEN_DEVICE_FUNC inline bool compareCoeff(const Scalar& incumbent, const Scalar& candidate) const {
    return candidate > incumbent;
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Packet comparePacket(const Packet& incumbent, const Packet& candidate) const {
    return pcmp_lt(incumbent, candidate);
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Scalar predux(const Packet& a) const {
    return predux_max(a);
  }
};

template <typename Scalar>
struct max_coeff_functor<Scalar, PropagateNaN, false> {
  EIGEN_DEVICE_FUNC inline Scalar compareCoeff(const Scalar& incumbent, const Scalar& candidate) {
    return (candidate > incumbent) || ((candidate != candidate) && (incumbent == incumbent));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Packet comparePacket(const Packet& incumbent, const Packet& candidate) {
    return pandnot(pcmp_lt_or_nan(incumbent, candidate), pisnan(incumbent));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Scalar predux(const Packet& a) const {
    return predux_max<PropagateNaN>(a);
  }
};

template <typename Scalar>
struct max_coeff_functor<Scalar, PropagateNumbers, false> {
  EIGEN_DEVICE_FUNC inline bool compareCoeff(const Scalar& incumbent, const Scalar& candidate) const {
    return (candidate > incumbent) || ((candidate == candidate) && (incumbent != incumbent));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Packet comparePacket(const Packet& incumbent, const Packet& candidate) const {
    return pandnot(pcmp_lt_or_nan(incumbent, candidate), pisnan(candidate));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Scalar predux(const Packet& a) const {
    return predux_max<PropagateNumbers>(a);
  }
};

template <typename Scalar, int NaNPropagation, bool IsInteger = NumTraits<Scalar>::IsInteger>
struct min_coeff_functor {
  EIGEN_DEVICE_FUNC inline bool compareCoeff(const Scalar& incumbent, const Scalar& candidate) const {
    return candidate < incumbent;
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Packet comparePacket(const Packet& incumbent, const Packet& candidate) const {
    return pcmp_lt(candidate, incumbent);
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Scalar predux(const Packet& a) const {
    return predux_min(a);
  }
};

template <typename Scalar>
struct min_coeff_functor<Scalar, PropagateNaN, false> {
  EIGEN_DEVICE_FUNC inline Scalar compareCoeff(const Scalar& incumbent, const Scalar& candidate) {
    return (candidate < incumbent) || ((candidate != candidate) && (incumbent == incumbent));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Packet comparePacket(const Packet& incumbent, const Packet& candidate) {
    return pandnot(pcmp_lt_or_nan(candidate, incumbent), pisnan(incumbent));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Scalar predux(const Packet& a) const {
    return predux_min<PropagateNaN>(a);
  }
};

template <typename Scalar>
struct min_coeff_functor<Scalar, PropagateNumbers, false> {
  EIGEN_DEVICE_FUNC inline bool compareCoeff(const Scalar& incumbent, const Scalar& candidate) const {
    return (candidate < incumbent) || ((candidate == candidate) && (incumbent != incumbent));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Packet comparePacket(const Packet& incumbent, const Packet& candidate) const {
    return pandnot(pcmp_lt_or_nan(candidate, incumbent), pisnan(candidate));
  }
  template <typename Packet>
  EIGEN_DEVICE_FUNC inline Scalar predux(const Packet& a) const {
    return predux_min<PropagateNumbers>(a);
  }
};

template <typename Scalar>
struct min_max_traits {
  static constexpr bool PacketAccess = packet_traits<Scalar>::Vectorizable;
};
template <typename Scalar, int NaNPropagation>
struct functor_traits<max_coeff_functor<Scalar, NaNPropagation>> : min_max_traits<Scalar> {};
template <typename Scalar, int NaNPropagation>
struct functor_traits<min_coeff_functor<Scalar, NaNPropagation>> : min_max_traits<Scalar> {};

template <typename Evaluator, typename Func, bool Linear, bool Vectorize>
struct find_coeff_loop;
template <typename Evaluator, typename Func>
struct find_coeff_loop<Evaluator, Func, /*Linear*/ false, /*Vectorize*/ false> {
  using Scalar = typename Evaluator::Scalar;
  static EIGEN_DEVICE_FUNC inline void run(const Evaluator& eval, Func& func, Scalar& res, Index& outer, Index& inner) {
    Index outerSize = eval.outerSize();
    Index innerSize = eval.innerSize();

    /* initialization performed in calling function */
    /* result = eval.coeff(0, 0); */
    /* outer = 0; */
    /* inner = 0; */

    for (Index j = 0; j < outerSize; j++) {
      for (Index i = 0; i < innerSize; i++) {
        Scalar xprCoeff = eval.coeffByOuterInner(j, i);
        bool newRes = func.compareCoeff(res, xprCoeff);
        if (newRes) {
          outer = j;
          inner = i;
          res = xprCoeff;
        }
      }
    }
  }
};
template <typename Evaluator, typename Func>
struct find_coeff_loop<Evaluator, Func, /*Linear*/ true, /*Vectorize*/ false> {
  using Scalar = typename Evaluator::Scalar;
  static EIGEN_DEVICE_FUNC inline void run(const Evaluator& eval, Func& func, Scalar& res, Index& index) {
    Index size = eval.size();

    /* initialization performed in calling function */
    /* result = eval.coeff(0); */
    /* index = 0; */

    for (Index k = 0; k < size; k++) {
      Scalar xprCoeff = eval.coeff(k);
      bool newRes = func.compareCoeff(res, xprCoeff);
      if (newRes) {
        index = k;
        res = xprCoeff;
      }
    }
  }
};
template <typename Evaluator, typename Func>
struct find_coeff_loop<Evaluator, Func, /*Linear*/ false, /*Vectorize*/ true> {
  using ScalarImpl = find_coeff_loop<Evaluator, Func, false, false>;
  using Scalar = typename Evaluator::Scalar;
  using Packet = typename Evaluator::Packet;
  static constexpr int PacketSize = unpacket_traits<Packet>::size;
  static EIGEN_DEVICE_FUNC inline void run(const Evaluator& eval, Func& func, Scalar& result, Index& outer,
                                           Index& inner) {
    Index outerSize = eval.outerSize();
    Index innerSize = eval.innerSize();
    Index packetEnd = numext::round_down(innerSize, PacketSize);

    /* initialization performed in calling function */
    /* result = eval.coeff(0, 0); */
    /* outer = 0; */
    /* inner = 0; */

    bool checkPacket = false;

    for (Index j = 0; j < outerSize; j++) {
      Packet resultPacket = pset1<Packet>(result);
      for (Index i = 0; i < packetEnd; i += PacketSize) {
        Packet xprPacket = eval.template packetByOuterInner<Unaligned, Packet>(j, i);
        if (predux_any(func.comparePacket(resultPacket, xprPacket))) {
          outer = j;
          inner = i;
          result = func.predux(xprPacket);
          resultPacket = pset1<Packet>(result);
          checkPacket = true;
        }
      }

      for (Index i = packetEnd; i < innerSize; i++) {
        Scalar xprCoeff = eval.coeffByOuterInner(j, i);
        if (func.compareCoeff(result, xprCoeff)) {
          outer = j;
          inner = i;
          result = xprCoeff;
          checkPacket = false;
        }
      }
    }

    if (checkPacket) {
      result = eval.coeffByOuterInner(outer, inner);
      Index i_end = inner + PacketSize;
      for (Index i = inner; i < i_end; i++) {
        Scalar xprCoeff = eval.coeffByOuterInner(outer, i);
        if (func.compareCoeff(result, xprCoeff)) {
          inner = i;
          result = xprCoeff;
        }
      }
    }
  }
};
template <typename Evaluator, typename Func>
struct find_coeff_loop<Evaluator, Func, /*Linear*/ true, /*Vectorize*/ true> {
  using ScalarImpl = find_coeff_loop<Evaluator, Func, true, false>;
  using Scalar = typename Evaluator::Scalar;
  using Packet = typename Evaluator::Packet;
  static constexpr int PacketSize = unpacket_traits<Packet>::size;
  static constexpr int Alignment = Evaluator::Alignment;

  static EIGEN_DEVICE_FUNC inline void run(const Evaluator& eval, Func& func, Scalar& result, Index& index) {
    Index size = eval.size();
    Index packetEnd = numext::round_down(size, PacketSize);

    /* initialization performed in calling function */
    /* result = eval.coeff(0); */
    /* index = 0; */

    Packet resultPacket = pset1<Packet>(result);
    bool checkPacket = false;

    for (Index k = 0; k < packetEnd; k += PacketSize) {
      Packet xprPacket = eval.template packet<Alignment, Packet>(k);
      if (predux_any(func.comparePacket(resultPacket, xprPacket))) {
        index = k;
        result = func.predux(xprPacket);
        resultPacket = pset1<Packet>(result);
        checkPacket = true;
      }
    }

    for (Index k = packetEnd; k < size; k++) {
      Scalar xprCoeff = eval.coeff(k);
      if (func.compareCoeff(result, xprCoeff)) {
        index = k;
        result = xprCoeff;
        checkPacket = false;
      }
    }

    if (checkPacket) {
      result = eval.coeff(index);
      Index k_end = index + PacketSize;
      for (Index k = index; k < k_end; k++) {
        Scalar xprCoeff = eval.coeff(k);
        if (func.compareCoeff(result, xprCoeff)) {
          index = k;
          result = xprCoeff;
        }
      }
    }
  }
};

template <typename Derived>
struct find_coeff_evaluator : public evaluator<Derived> {
  using Base = evaluator<Derived>;
  using Scalar = typename Derived::Scalar;
  using Packet = typename packet_traits<Scalar>::type;
  static constexpr int Flags = Base::Flags;
  static constexpr bool IsRowMajor = bool(Flags & RowMajorBit);
  EIGEN_DEVICE_FUNC inline find_coeff_evaluator(const Derived& xpr) : Base(xpr), m_xpr(xpr) {}

  EIGEN_DEVICE_FUNC inline Scalar coeffByOuterInner(Index outer, Index inner) const {
    Index row = IsRowMajor ? outer : inner;
    Index col = IsRowMajor ? inner : outer;
    return Base::coeff(row, col);
  }
  template <int LoadMode, typename PacketType>
  EIGEN_DEVICE_FUNC inline PacketType packetByOuterInner(Index outer, Index inner) const {
    Index row = IsRowMajor ? outer : inner;
    Index col = IsRowMajor ? inner : outer;
    return Base::template packet<LoadMode, PacketType>(row, col);
  }

  EIGEN_DEVICE_FUNC inline Index innerSize() const { return m_xpr.innerSize(); }
  EIGEN_DEVICE_FUNC inline Index outerSize() const { return m_xpr.outerSize(); }
  EIGEN_DEVICE_FUNC inline Index size() const { return m_xpr.size(); }

  const Derived& m_xpr;
};

template <typename Derived, typename Func>
struct find_coeff_impl {
  using Evaluator = find_coeff_evaluator<Derived>;
  static constexpr int Flags = Evaluator::Flags;
  static constexpr int Alignment = Evaluator::Alignment;
  static constexpr bool IsRowMajor = Derived::IsRowMajor;
  static constexpr int MaxInnerSizeAtCompileTime =
      IsRowMajor ? Derived::MaxColsAtCompileTime : Derived::MaxRowsAtCompileTime;
  static constexpr int MaxSizeAtCompileTime = Derived::MaxSizeAtCompileTime;

  using Scalar = typename Derived::Scalar;
  using Packet = typename Evaluator::Packet;

  static constexpr int PacketSize = unpacket_traits<Packet>::size;
  static constexpr bool Linearize = bool(Flags & LinearAccessBit);
  static constexpr bool DontVectorize =
      enum_lt_not_dynamic(Linearize ? MaxSizeAtCompileTime : MaxInnerSizeAtCompileTime, PacketSize);
  static constexpr bool Vectorize =
      !DontVectorize && bool(Flags & PacketAccessBit) && functor_traits<Func>::PacketAccess;

  using Loop = find_coeff_loop<Evaluator, Func, Linearize, Vectorize>;

  template <bool ForwardLinearAccess = Linearize, std::enable_if_t<!ForwardLinearAccess, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& xpr, Func& func, Scalar& res, Index& outer,
                                                        Index& inner) {
    Evaluator eval(xpr);
    Loop::run(eval, func, res, outer, inner);
  }
  template <bool ForwardLinearAccess = Linearize, std::enable_if_t<ForwardLinearAccess, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& xpr, Func& func, Scalar& res, Index& outer,
                                                        Index& inner) {
    // where possible, use the linear loop and back-calculate the outer and inner indices
    Index index = 0;
    run(xpr, func, res, index);
    outer = index / xpr.innerSize();
    inner = index % xpr.innerSize();
  }
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& xpr, Func& func, Scalar& res, Index& index) {
    Evaluator eval(xpr);
    Loop::run(eval, func, res, index);
  }
};

template <typename Derived, typename IndexType, typename Func>
EIGEN_DEVICE_FUNC typename internal::traits<Derived>::Scalar findCoeff(const DenseBase<Derived>& mat, Func& func,
                                                                       IndexType* rowPtr, IndexType* colPtr) {
  eigen_assert(mat.rows() > 0 && mat.cols() > 0 && "you are using an empty matrix");
  using Scalar = typename DenseBase<Derived>::Scalar;
  using FindCoeffImpl = internal::find_coeff_impl<Derived, Func>;
  Index outer = 0;
  Index inner = 0;
  Scalar res = mat.coeff(0, 0);
  FindCoeffImpl::run(mat.derived(), func, res, outer, inner);
  *rowPtr = internal::convert_index<IndexType>(Derived::IsRowMajor ? outer : inner);
  if (colPtr) *colPtr = internal::convert_index<IndexType>(Derived::IsRowMajor ? inner : outer);
  return res;
}

template <typename Derived, typename IndexType, typename Func>
EIGEN_DEVICE_FUNC typename internal::traits<Derived>::Scalar findCoeff(const DenseBase<Derived>& mat, Func& func,
                                                                       IndexType* indexPtr) {
  eigen_assert(mat.size() > 0 && "you are using an empty matrix");
  EIGEN_STATIC_ASSERT_VECTOR_ONLY(Derived)
  using Scalar = typename DenseBase<Derived>::Scalar;
  using FindCoeffImpl = internal::find_coeff_impl<Derived, Func>;
  Index index = 0;
  Scalar res = mat.coeff(0);
  FindCoeffImpl::run(mat.derived(), func, res, index);
  *indexPtr = internal::convert_index<IndexType>(index);
  return res;
}

}  // namespace internal

/** \fn DenseBase<Derived>::minCoeff(IndexType* rowId, IndexType* colId) const
 * \returns the minimum of all coefficients of *this and puts in *row and *col its location.
 *
 * If there are multiple coefficients with the same extreme value, the location of the first instance is returned.
 *
 * In case \c *this contains NaN, NaNPropagation determines the behavior:
 *   NaNPropagation == PropagateFast : undefined
 *   NaNPropagation == PropagateNaN : result is NaN
 *   NaNPropagation == PropagateNumbers : result is maximum of elements that are not NaN
 * \warning the matrix must be not empty, otherwise an assertion is triggered.
 *
 * \sa DenseBase::minCoeff(Index*), DenseBase::maxCoeff(Index*,Index*), DenseBase::visit(), DenseBase::minCoeff()
 */
template <typename Derived>
template <int NaNPropagation, typename IndexType>
EIGEN_DEVICE_FUNC typename internal::traits<Derived>::Scalar DenseBase<Derived>::minCoeff(IndexType* rowPtr,
                                                                                          IndexType* colPtr) const {
  using Func = internal::min_coeff_functor<Scalar, NaNPropagation>;
  Func func;
  return internal::findCoeff(derived(), func, rowPtr, colPtr);
}

/** \returns the minimum of all coefficients of *this and puts in *index its location.
 *
 * If there are multiple coefficients with the same extreme value, the location of the first instance is returned.
 *
 * In case \c *this contains NaN, NaNPropagation determines the behavior:
 *   NaNPropagation == PropagateFast : undefined
 *   NaNPropagation == PropagateNaN : result is NaN
 *   NaNPropagation == PropagateNumbers : result is maximum of elements that are not NaN
 * \warning the matrix must be not empty, otherwise an assertion is triggered.
 *
 * \sa DenseBase::minCoeff(IndexType*,IndexType*), DenseBase::maxCoeff(IndexType*,IndexType*), DenseBase::visit(),
 * DenseBase::minCoeff()
 */
template <typename Derived>
template <int NaNPropagation, typename IndexType>
EIGEN_DEVICE_FUNC typename internal::traits<Derived>::Scalar DenseBase<Derived>::minCoeff(IndexType* indexPtr) const {
  using Func = internal::min_coeff_functor<Scalar, NaNPropagation>;
  Func func;
  return internal::findCoeff(derived(), func, indexPtr);
}

/** \fn DenseBase<Derived>::maxCoeff(IndexType* rowId, IndexType* colId) const
 * \returns the maximum of all coefficients of *this and puts in *row and *col its location.
 *
 * If there are multiple coefficients with the same extreme value, the location of the first instance is returned.
 *
 * In case \c *this contains NaN, NaNPropagation determines the behavior:
 *   NaNPropagation == PropagateFast : undefined
 *   NaNPropagation == PropagateNaN : result is NaN
 *   NaNPropagation == PropagateNumbers : result is maximum of elements that are not NaN
 * \warning the matrix must be not empty, otherwise an assertion is triggered.
 *
 * \sa DenseBase::minCoeff(IndexType*,IndexType*), DenseBase::visit(), DenseBase::maxCoeff()
 */
template <typename Derived>
template <int NaNPropagation, typename IndexType>
EIGEN_DEVICE_FUNC typename internal::traits<Derived>::Scalar DenseBase<Derived>::maxCoeff(IndexType* rowPtr,
                                                                                          IndexType* colPtr) const {
  using Func = internal::max_coeff_functor<Scalar, NaNPropagation>;
  Func func;
  return internal::findCoeff(derived(), func, rowPtr, colPtr);
}

/** \returns the maximum of all coefficients of *this and puts in *index its location.
 *
 * If there are multiple coefficients with the same extreme value, the location of the first instance is returned.
 *
 * In case \c *this contains NaN, NaNPropagation determines the behavior:
 *   NaNPropagation == PropagateFast : undefined
 *   NaNPropagation == PropagateNaN : result is NaN
 *   NaNPropagation == PropagateNumbers : result is maximum of elements that are not NaN
 * \warning the matrix must be not empty, otherwise an assertion is triggered.
 *
 * \sa DenseBase::maxCoeff(IndexType*,IndexType*), DenseBase::minCoeff(IndexType*,IndexType*), DenseBase::visitor(),
 * DenseBase::maxCoeff()
 */
template <typename Derived>
template <int NaNPropagation, typename IndexType>
EIGEN_DEVICE_FUNC typename internal::traits<Derived>::Scalar DenseBase<Derived>::maxCoeff(IndexType* indexPtr) const {
  using Func = internal::max_coeff_functor<Scalar, NaNPropagation>;
  Func func;
  return internal::findCoeff(derived(), func, indexPtr);
}

}  // namespace Eigen

#endif  // EIGEN_FIND_COEFF_H
