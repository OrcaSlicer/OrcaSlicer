// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2008 Gael Guennebaud <gael.guennebaud@inria.fr>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_VISITOR_H
#define EIGEN_VISITOR_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

template <typename Visitor, typename Derived, int UnrollCount,
          bool Vectorize = (Derived::PacketAccess && functor_traits<Visitor>::PacketAccess), bool LinearAccess = false,
          bool ShortCircuitEvaluation = false>
struct visitor_impl;

template <typename Visitor, bool ShortCircuitEvaluation = false>
struct short_circuit_eval_impl {
  // if short circuit evaluation is not used, do nothing
  static constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE bool run(const Visitor&) { return false; }
};
template <typename Visitor>
struct short_circuit_eval_impl<Visitor, true> {
  // if short circuit evaluation is used, check the visitor
  static constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE bool run(const Visitor& visitor) { return visitor.done(); }
};

// unrolled inner-outer traversal
template <typename Visitor, typename Derived, int UnrollCount, bool Vectorize, bool ShortCircuitEvaluation>
struct visitor_impl<Visitor, Derived, UnrollCount, Vectorize, false, ShortCircuitEvaluation> {
  // don't use short circuit evaluation for unrolled version
  using Scalar = typename Derived::Scalar;
  using Packet = typename packet_traits<Scalar>::type;
  static constexpr bool RowMajor = Derived::IsRowMajor;
  static constexpr int RowsAtCompileTime = Derived::RowsAtCompileTime;
  static constexpr int ColsAtCompileTime = Derived::ColsAtCompileTime;
  static constexpr int PacketSize = packet_traits<Scalar>::size;

  static constexpr bool CanVectorize(int K) {
    constexpr int InnerSizeAtCompileTime = RowMajor ? ColsAtCompileTime : RowsAtCompileTime;
    if (InnerSizeAtCompileTime < PacketSize) return false;
    return Vectorize && (InnerSizeAtCompileTime - (K % InnerSizeAtCompileTime) >= PacketSize);
  }

  template <int K = 0, bool Empty = (K == UnrollCount), std::enable_if_t<Empty, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived&, Visitor&) {}

  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && Initialize && !DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    visitor.init(mat.coeff(0, 0), 0, 0);
    run<1>(mat, visitor);
  }

  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && !Initialize && !DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    static constexpr int R = RowMajor ? (K / ColsAtCompileTime) : (K % RowsAtCompileTime);
    static constexpr int C = RowMajor ? (K % ColsAtCompileTime) : (K / RowsAtCompileTime);
    visitor(mat.coeff(R, C), R, C);
    run<K + 1>(mat, visitor);
  }

  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && Initialize && DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    Packet P = mat.template packet<Packet>(0, 0);
    visitor.initpacket(P, 0, 0);
    run<PacketSize>(mat, visitor);
  }

  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && !Initialize && DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    static constexpr int R = RowMajor ? (K / ColsAtCompileTime) : (K % RowsAtCompileTime);
    static constexpr int C = RowMajor ? (K % ColsAtCompileTime) : (K / RowsAtCompileTime);
    Packet P = mat.template packet<Packet>(R, C);
    visitor.packet(P, R, C);
    run<K + PacketSize>(mat, visitor);
  }
};

// unrolled linear traversal
template <typename Visitor, typename Derived, int UnrollCount, bool Vectorize, bool ShortCircuitEvaluation>
struct visitor_impl<Visitor, Derived, UnrollCount, Vectorize, true, ShortCircuitEvaluation> {
  // don't use short circuit evaluation for unrolled version
  using Scalar = typename Derived::Scalar;
  using Packet = typename packet_traits<Scalar>::type;
  static constexpr int PacketSize = packet_traits<Scalar>::size;

  static constexpr bool CanVectorize(int K) { return Vectorize && ((UnrollCount - K) >= PacketSize); }

  // empty
  template <int K = 0, bool Empty = (K == UnrollCount), std::enable_if_t<Empty, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived&, Visitor&) {}

  // scalar initialization
  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && Initialize && !DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    visitor.init(mat.coeff(0), 0);
    run<1>(mat, visitor);
  }

  // scalar iteration
  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && !Initialize && !DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    visitor(mat.coeff(K), K);
    run<K + 1>(mat, visitor);
  }

  // vector initialization
  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && Initialize && DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    Packet P = mat.template packet<Packet>(0);
    visitor.initpacket(P, 0);
    run<PacketSize>(mat, visitor);
  }

  // vector iteration
  template <int K = 0, bool Empty = (K == UnrollCount), bool Initialize = (K == 0), bool DoVectorOp = CanVectorize(K),
            std::enable_if_t<!Empty && !Initialize && DoVectorOp, bool> = true>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    Packet P = mat.template packet<Packet>(K);
    visitor.packet(P, K);
    run<K + PacketSize>(mat, visitor);
  }
};

// dynamic scalar outer-inner traversal
template <typename Visitor, typename Derived, bool ShortCircuitEvaluation>
struct visitor_impl<Visitor, Derived, Dynamic, /*Vectorize=*/false, /*LinearAccess=*/false, ShortCircuitEvaluation> {
  using short_circuit = short_circuit_eval_impl<Visitor, ShortCircuitEvaluation>;
  static constexpr bool RowMajor = Derived::IsRowMajor;

  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    const Index innerSize = RowMajor ? mat.cols() : mat.rows();
    const Index outerSize = RowMajor ? mat.rows() : mat.cols();
    if (innerSize == 0 || outerSize == 0) return;
    {
      visitor.init(mat.coeff(0, 0), 0, 0);
      if (short_circuit::run(visitor)) return;
      for (Index i = 1; i < innerSize; ++i) {
        Index r = RowMajor ? 0 : i;
        Index c = RowMajor ? i : 0;
        visitor(mat.coeff(r, c), r, c);
        if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      }
    }
    for (Index j = 1; j < outerSize; j++) {
      for (Index i = 0; i < innerSize; ++i) {
        Index r = RowMajor ? j : i;
        Index c = RowMajor ? i : j;
        visitor(mat.coeff(r, c), r, c);
        if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      }
    }
  }
};

// dynamic vectorized outer-inner traversal
template <typename Visitor, typename Derived, bool ShortCircuitEvaluation>
struct visitor_impl<Visitor, Derived, Dynamic, /*Vectorize=*/true, /*LinearAccess=*/false, ShortCircuitEvaluation> {
  using Scalar = typename Derived::Scalar;
  using Packet = typename packet_traits<Scalar>::type;
  static constexpr int PacketSize = packet_traits<Scalar>::size;
  using short_circuit = short_circuit_eval_impl<Visitor, ShortCircuitEvaluation>;
  static constexpr bool RowMajor = Derived::IsRowMajor;

  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    const Index innerSize = RowMajor ? mat.cols() : mat.rows();
    const Index outerSize = RowMajor ? mat.rows() : mat.cols();
    if (innerSize == 0 || outerSize == 0) return;
    {
      Index i = 0;
      if (innerSize < PacketSize) {
        visitor.init(mat.coeff(0, 0), 0, 0);
        i = 1;
      } else {
        Packet p = mat.template packet<Packet>(0, 0);
        visitor.initpacket(p, 0, 0);
        i = PacketSize;
      }
      if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      for (; i + PacketSize - 1 < innerSize; i += PacketSize) {
        Index r = RowMajor ? 0 : i;
        Index c = RowMajor ? i : 0;
        Packet p = mat.template packet<Packet>(r, c);
        visitor.packet(p, r, c);
        if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      }
      for (; i < innerSize; ++i) {
        Index r = RowMajor ? 0 : i;
        Index c = RowMajor ? i : 0;
        visitor(mat.coeff(r, c), r, c);
        if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      }
    }
    for (Index j = 1; j < outerSize; j++) {
      Index i = 0;
      for (; i + PacketSize - 1 < innerSize; i += PacketSize) {
        Index r = RowMajor ? j : i;
        Index c = RowMajor ? i : j;
        Packet p = mat.template packet<Packet>(r, c);
        visitor.packet(p, r, c);
        if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      }
      for (; i < innerSize; ++i) {
        Index r = RowMajor ? j : i;
        Index c = RowMajor ? i : j;
        visitor(mat.coeff(r, c), r, c);
        if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
      }
    }
  }
};

// dynamic scalar linear traversal
template <typename Visitor, typename Derived, bool ShortCircuitEvaluation>
struct visitor_impl<Visitor, Derived, Dynamic, /*Vectorize=*/false, /*LinearAccess=*/true, ShortCircuitEvaluation> {
  using short_circuit = short_circuit_eval_impl<Visitor, ShortCircuitEvaluation>;

  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    const Index size = mat.size();
    if (size == 0) return;
    visitor.init(mat.coeff(0), 0);
    if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
    for (Index k = 1; k < size; k++) {
      visitor(mat.coeff(k), k);
      if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
    }
  }
};

// dynamic vectorized linear traversal
template <typename Visitor, typename Derived, bool ShortCircuitEvaluation>
struct visitor_impl<Visitor, Derived, Dynamic, /*Vectorize=*/true, /*LinearAccess=*/true, ShortCircuitEvaluation> {
  using Scalar = typename Derived::Scalar;
  using Packet = typename packet_traits<Scalar>::type;
  static constexpr int PacketSize = packet_traits<Scalar>::size;
  using short_circuit = short_circuit_eval_impl<Visitor, ShortCircuitEvaluation>;

  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const Derived& mat, Visitor& visitor) {
    const Index size = mat.size();
    if (size == 0) return;
    Index k = 0;
    if (size < PacketSize) {
      visitor.init(mat.coeff(0), 0);
      k = 1;
    } else {
      Packet p = mat.template packet<Packet>(k);
      visitor.initpacket(p, k);
      k = PacketSize;
    }
    if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
    for (; k + PacketSize - 1 < size; k += PacketSize) {
      Packet p = mat.template packet<Packet>(k);
      visitor.packet(p, k);
      if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
    }
    for (; k < size; k++) {
      visitor(mat.coeff(k), k);
      if EIGEN_PREDICT_FALSE (short_circuit::run(visitor)) return;
    }
  }
};

// evaluator adaptor
template <typename XprType>
class visitor_evaluator {
 public:
  typedef evaluator<XprType> Evaluator;
  typedef typename XprType::Scalar Scalar;
  using Packet = typename packet_traits<Scalar>::type;
  typedef std::remove_const_t<typename XprType::CoeffReturnType> CoeffReturnType;

  static constexpr bool PacketAccess = static_cast<bool>(Evaluator::Flags & PacketAccessBit);
  static constexpr bool LinearAccess = static_cast<bool>(Evaluator::Flags & LinearAccessBit);
  static constexpr bool IsRowMajor = static_cast<bool>(XprType::IsRowMajor);
  static constexpr int RowsAtCompileTime = XprType::RowsAtCompileTime;
  static constexpr int ColsAtCompileTime = XprType::ColsAtCompileTime;
  static constexpr int XprAlignment = Evaluator::Alignment;
  static constexpr int CoeffReadCost = Evaluator::CoeffReadCost;

  EIGEN_DEVICE_FUNC explicit visitor_evaluator(const XprType& xpr) : m_evaluator(xpr), m_xpr(xpr) {}

  EIGEN_DEVICE_FUNC constexpr Index rows() const noexcept { return m_xpr.rows(); }
  EIGEN_DEVICE_FUNC constexpr Index cols() const noexcept { return m_xpr.cols(); }
  EIGEN_DEVICE_FUNC constexpr Index size() const noexcept { return m_xpr.size(); }
  // outer-inner access
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE CoeffReturnType coeff(Index row, Index col) const {
    return m_evaluator.coeff(row, col);
  }
  template <typename Packet, int Alignment = Unaligned>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet packet(Index row, Index col) const {
    return m_evaluator.template packet<Alignment, Packet>(row, col);
  }
  // linear access
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE CoeffReturnType coeff(Index index) const { return m_evaluator.coeff(index); }
  template <typename Packet, int Alignment = XprAlignment>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Packet packet(Index index) const {
    return m_evaluator.template packet<Alignment, Packet>(index);
  }

 protected:
  Evaluator m_evaluator;
  const XprType& m_xpr;
};

template <typename Derived, typename Visitor, bool ShortCircuitEvaulation>
struct visit_impl {
  using Evaluator = visitor_evaluator<Derived>;
  using Scalar = typename DenseBase<Derived>::Scalar;

  static constexpr bool IsRowMajor = DenseBase<Derived>::IsRowMajor;
  static constexpr int SizeAtCompileTime = DenseBase<Derived>::SizeAtCompileTime;
  static constexpr int RowsAtCompileTime = DenseBase<Derived>::RowsAtCompileTime;
  static constexpr int ColsAtCompileTime = DenseBase<Derived>::ColsAtCompileTime;
  static constexpr int InnerSizeAtCompileTime = IsRowMajor ? ColsAtCompileTime : RowsAtCompileTime;
  static constexpr int OuterSizeAtCompileTime = IsRowMajor ? RowsAtCompileTime : ColsAtCompileTime;

  static constexpr bool LinearAccess =
      Evaluator::LinearAccess && static_cast<bool>(functor_traits<Visitor>::LinearAccess);
  static constexpr bool Vectorize = Evaluator::PacketAccess && static_cast<bool>(functor_traits<Visitor>::PacketAccess);

  static constexpr int PacketSize = packet_traits<Scalar>::size;
  static constexpr int VectorOps =
      Vectorize ? (LinearAccess ? (SizeAtCompileTime / PacketSize)
                                : (OuterSizeAtCompileTime * (InnerSizeAtCompileTime / PacketSize)))
                : 0;
  static constexpr int ScalarOps = SizeAtCompileTime - (VectorOps * PacketSize);
  // treat vector op and scalar op as same cost for unroll logic
  static constexpr int TotalOps = VectorOps + ScalarOps;

  static constexpr int UnrollCost = int(Evaluator::CoeffReadCost) + int(functor_traits<Visitor>::Cost);
  static constexpr bool Unroll = (SizeAtCompileTime != Dynamic) && ((TotalOps * UnrollCost) <= EIGEN_UNROLLING_LIMIT);
  static constexpr int UnrollCount = Unroll ? int(SizeAtCompileTime) : Dynamic;

  using impl = visitor_impl<Visitor, Evaluator, UnrollCount, Vectorize, LinearAccess, ShortCircuitEvaulation>;

  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(const DenseBase<Derived>& mat, Visitor& visitor) {
    Evaluator evaluator(mat.derived());
    impl::run(evaluator, visitor);
  }
};

}  // end namespace internal

/** Applies the visitor \a visitor to the whole coefficients of the matrix or vector.
 *
 * The template parameter \a Visitor is the type of the visitor and provides the following interface:
 * \code
 * struct MyVisitor {
 *   // called for the first coefficient
 *   void init(const Scalar& value, Index i, Index j);
 *   // called for all other coefficients
 *   void operator() (const Scalar& value, Index i, Index j);
 * };
 * \endcode
 *
 * \note compared to one or two \em for \em loops, visitors offer automatic
 * unrolling for small fixed size matrix.
 *
 * \note if the matrix is empty, then the visitor is left unchanged.
 *
 * \sa minCoeff(Index*,Index*), maxCoeff(Index*,Index*), DenseBase::redux()
 */
template <typename Derived>
template <typename Visitor>
EIGEN_DEVICE_FUNC void DenseBase<Derived>::visit(Visitor& visitor) const {
  using impl = internal::visit_impl<Derived, Visitor, /*ShortCircuitEvaulation*/ false>;
  impl::run(derived(), visitor);
}

namespace internal {

template <typename Scalar>
struct all_visitor {
  using result_type = bool;
  using Packet = typename packet_traits<Scalar>::type;
  EIGEN_DEVICE_FUNC inline void init(const Scalar& value, Index, Index) { res = (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline void init(const Scalar& value, Index) { res = (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline bool all_predux(const Packet& p) const { return !predux_any(pcmp_eq(p, pzero(p))); }
  EIGEN_DEVICE_FUNC inline void initpacket(const Packet& p, Index, Index) { res = all_predux(p); }
  EIGEN_DEVICE_FUNC inline void initpacket(const Packet& p, Index) { res = all_predux(p); }
  EIGEN_DEVICE_FUNC inline void operator()(const Scalar& value, Index, Index) { res = res && (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline void operator()(const Scalar& value, Index) { res = res && (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline void packet(const Packet& p, Index, Index) { res = res && all_predux(p); }
  EIGEN_DEVICE_FUNC inline void packet(const Packet& p, Index) { res = res && all_predux(p); }
  EIGEN_DEVICE_FUNC inline bool done() const { return !res; }
  bool res = true;
};
template <typename Scalar>
struct functor_traits<all_visitor<Scalar>> {
  enum { Cost = NumTraits<Scalar>::ReadCost, LinearAccess = true, PacketAccess = packet_traits<Scalar>::HasCmp };
};

template <typename Scalar>
struct any_visitor {
  using result_type = bool;
  using Packet = typename packet_traits<Scalar>::type;
  EIGEN_DEVICE_FUNC inline void init(const Scalar& value, Index, Index) { res = (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline void init(const Scalar& value, Index) { res = (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline bool any_predux(const Packet& p) const {
    return predux_any(pandnot(ptrue(p), pcmp_eq(p, pzero(p))));
  }
  EIGEN_DEVICE_FUNC inline void initpacket(const Packet& p, Index, Index) { res = any_predux(p); }
  EIGEN_DEVICE_FUNC inline void initpacket(const Packet& p, Index) { res = any_predux(p); }
  EIGEN_DEVICE_FUNC inline void operator()(const Scalar& value, Index, Index) { res = res || (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline void operator()(const Scalar& value, Index) { res = res || (value != Scalar(0)); }
  EIGEN_DEVICE_FUNC inline void packet(const Packet& p, Index, Index) { res = res || any_predux(p); }
  EIGEN_DEVICE_FUNC inline void packet(const Packet& p, Index) { res = res || any_predux(p); }
  EIGEN_DEVICE_FUNC inline bool done() const { return res; }
  bool res = false;
};
template <typename Scalar>
struct functor_traits<any_visitor<Scalar>> {
  enum { Cost = NumTraits<Scalar>::ReadCost, LinearAccess = true, PacketAccess = packet_traits<Scalar>::HasCmp };
};

template <typename Scalar>
struct count_visitor {
  using result_type = Index;
  using Packet = typename packet_traits<Scalar>::type;
  EIGEN_DEVICE_FUNC inline void init(const Scalar& value, Index, Index) { res = value != Scalar(0) ? 1 : 0; }
  EIGEN_DEVICE_FUNC inline void init(const Scalar& value, Index) { res = value != Scalar(0) ? 1 : 0; }
  EIGEN_DEVICE_FUNC inline Index count_redux(const Packet& p) const {
    const Packet cst_one = pset1<Packet>(Scalar(1));
    Packet true_vals = pandnot(cst_one, pcmp_eq(p, pzero(p)));
    Scalar num_true = predux(true_vals);
    return static_cast<Index>(num_true);
  }
  EIGEN_DEVICE_FUNC inline void initpacket(const Packet& p, Index, Index) { res = count_redux(p); }
  EIGEN_DEVICE_FUNC inline void initpacket(const Packet& p, Index) { res = count_redux(p); }
  EIGEN_DEVICE_FUNC inline void operator()(const Scalar& value, Index, Index) {
    if (value != Scalar(0)) res++;
  }
  EIGEN_DEVICE_FUNC inline void operator()(const Scalar& value, Index) {
    if (value != Scalar(0)) res++;
  }
  EIGEN_DEVICE_FUNC inline void packet(const Packet& p, Index, Index) { res += count_redux(p); }
  EIGEN_DEVICE_FUNC inline void packet(const Packet& p, Index) { res += count_redux(p); }
  Index res = 0;
};

template <typename Scalar>
struct functor_traits<count_visitor<Scalar>> {
  enum {
    Cost = NumTraits<Scalar>::AddCost,
    LinearAccess = true,
    // predux is problematic for bool
    PacketAccess = packet_traits<Scalar>::HasCmp && packet_traits<Scalar>::HasAdd && !is_same<Scalar, bool>::value
  };
};

template <typename Derived, bool AlwaysTrue = NumTraits<typename traits<Derived>::Scalar>::IsInteger>
struct all_finite_impl {
  static EIGEN_DEVICE_FUNC inline bool run(const Derived& /*derived*/) { return true; }
};
#if !defined(__FINITE_MATH_ONLY__) || !(__FINITE_MATH_ONLY__)
template <typename Derived>
struct all_finite_impl<Derived, false> {
  static EIGEN_DEVICE_FUNC inline bool run(const Derived& derived) { return derived.array().isFiniteTyped().all(); }
};
#endif

}  // end namespace internal

/** \returns true if all coefficients are true
 *
 * Example: \include MatrixBase_all.cpp
 * Output: \verbinclude MatrixBase_all.out
 *
 * \sa any(), Cwise::operator<()
 */
template <typename Derived>
EIGEN_DEVICE_FUNC inline bool DenseBase<Derived>::all() const {
  using Visitor = internal::all_visitor<Scalar>;
  using impl = internal::visit_impl<Derived, Visitor, /*ShortCircuitEvaulation*/ true>;
  Visitor visitor;
  impl::run(derived(), visitor);
  return visitor.res;
}

/** \returns true if at least one coefficient is true
 *
 * \sa all()
 */
template <typename Derived>
EIGEN_DEVICE_FUNC inline bool DenseBase<Derived>::any() const {
  using Visitor = internal::any_visitor<Scalar>;
  using impl = internal::visit_impl<Derived, Visitor, /*ShortCircuitEvaulation*/ true>;
  Visitor visitor;
  impl::run(derived(), visitor);
  return visitor.res;
}

/** \returns the number of coefficients which evaluate to true
 *
 * \sa all(), any()
 */
template <typename Derived>
EIGEN_DEVICE_FUNC Index DenseBase<Derived>::count() const {
  using Visitor = internal::count_visitor<Scalar>;
  using impl = internal::visit_impl<Derived, Visitor, /*ShortCircuitEvaulation*/ false>;
  Visitor visitor;
  impl::run(derived(), visitor);
  return visitor.res;
}

template <typename Derived>
EIGEN_DEVICE_FUNC inline bool DenseBase<Derived>::hasNaN() const {
  return derived().cwiseTypedNotEqual(derived()).any();
}

/** \returns true if \c *this contains only finite numbers, i.e., no NaN and no +/-INF values.
 *
 * \sa hasNaN()
 */
template <typename Derived>
EIGEN_DEVICE_FUNC inline bool DenseBase<Derived>::allFinite() const {
  return internal::all_finite_impl<Derived>::run(derived());
}

}  // end namespace Eigen

#endif  // EIGEN_VISITOR_H
