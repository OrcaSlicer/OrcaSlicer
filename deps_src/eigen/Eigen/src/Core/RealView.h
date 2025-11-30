// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 Charlie Schlosser <cs.schlosser@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_REALVIEW_H
#define EIGEN_REALVIEW_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

// Vectorized assignment to RealView requires array-oriented access to the real and imaginary components.
// From https://en.cppreference.com/w/cpp/numeric/complex.html:
// For any pointer to an element of an array of std::complex<T> named p and any valid array index i,
// reinterpret_cast<T*>(p)[2 * i] is the real part of the complex number p[i], and
// reinterpret_cast<T*>(p)[2 * i + 1] is the imaginary part of the complex number p[i].

template <typename ComplexScalar>
struct complex_array_access : std::false_type {};
template <>
struct complex_array_access<std::complex<float>> : std::true_type {};
template <>
struct complex_array_access<std::complex<double>> : std::true_type {};
template <>
struct complex_array_access<std::complex<long double>> : std::true_type {};

template <typename Xpr>
struct traits<RealView<Xpr>> : public traits<Xpr> {
  template <typename T>
  static constexpr int double_size(T size, bool times_two) {
    int size_as_int = int(size);
    if (size_as_int == Dynamic) return Dynamic;
    return times_two ? (2 * size_as_int) : size_as_int;
  }
  using Base = traits<Xpr>;
  using ComplexScalar = typename Base::Scalar;
  using Scalar = typename NumTraits<ComplexScalar>::Real;
  static constexpr int ActualDirectAccessBit = complex_array_access<ComplexScalar>::value ? DirectAccessBit : 0;
  static constexpr int ActualPacketAccessBit = packet_traits<Scalar>::Vectorizable ? PacketAccessBit : 0;
  static constexpr int FlagMask =
      ActualDirectAccessBit | ActualPacketAccessBit | HereditaryBits | LinearAccessBit | LvalueBit;
  static constexpr int BaseFlags = int(evaluator<Xpr>::Flags) | int(Base::Flags);
  static constexpr int Flags = BaseFlags & FlagMask;
  static constexpr bool IsRowMajor = Flags & RowMajorBit;
  static constexpr int RowsAtCompileTime = double_size(Base::RowsAtCompileTime, !IsRowMajor);
  static constexpr int ColsAtCompileTime = double_size(Base::ColsAtCompileTime, IsRowMajor);
  static constexpr int SizeAtCompileTime = size_at_compile_time(RowsAtCompileTime, ColsAtCompileTime);
  static constexpr int MaxRowsAtCompileTime = double_size(Base::MaxRowsAtCompileTime, !IsRowMajor);
  static constexpr int MaxColsAtCompileTime = double_size(Base::MaxColsAtCompileTime, IsRowMajor);
  static constexpr int MaxSizeAtCompileTime = size_at_compile_time(MaxRowsAtCompileTime, MaxColsAtCompileTime);
  static constexpr int OuterStrideAtCompileTime = double_size(outer_stride_at_compile_time<Xpr>::ret, true);
  static constexpr int InnerStrideAtCompileTime = inner_stride_at_compile_time<Xpr>::ret;
};

template <typename Xpr>
struct evaluator<RealView<Xpr>> : private evaluator<Xpr> {
  using BaseEvaluator = evaluator<Xpr>;
  using XprType = RealView<Xpr>;
  using ExpressionTraits = traits<XprType>;
  using ComplexScalar = typename ExpressionTraits::ComplexScalar;
  using ComplexCoeffReturnType = typename BaseEvaluator::CoeffReturnType;
  using Scalar = typename ExpressionTraits::Scalar;

  static constexpr bool IsRowMajor = ExpressionTraits::IsRowMajor;
  static constexpr int Flags = ExpressionTraits::Flags;
  static constexpr int CoeffReadCost = BaseEvaluator::CoeffReadCost;
  static constexpr int Alignment = BaseEvaluator::Alignment;

  EIGEN_DEVICE_FUNC explicit evaluator(XprType realView) : BaseEvaluator(realView.m_xpr) {}

  template <bool Enable = std::is_reference<ComplexCoeffReturnType>::value, typename = std::enable_if_t<!Enable>>
  constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Scalar coeff(Index row, Index col) const {
    ComplexCoeffReturnType cscalar = BaseEvaluator::coeff(IsRowMajor ? row : row / 2, IsRowMajor ? col / 2 : col);
    Index p = (IsRowMajor ? col : row) & 1;
    return p ? numext::real(cscalar) : numext::imag(cscalar);
  }

  template <bool Enable = std::is_reference<ComplexCoeffReturnType>::value, typename = std::enable_if_t<Enable>>
  constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE const Scalar& coeff(Index row, Index col) const {
    ComplexCoeffReturnType cscalar = BaseEvaluator::coeff(IsRowMajor ? row : row / 2, IsRowMajor ? col / 2 : col);
    Index p = (IsRowMajor ? col : row) & 1;
    return reinterpret_cast<const Scalar(&)[2]>(cscalar)[p];
  }

  constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Scalar& coeffRef(Index row, Index col) {
    ComplexScalar& cscalar = BaseEvaluator::coeffRef(IsRowMajor ? row : row / 2, IsRowMajor ? col / 2 : col);
    Index p = (IsRowMajor ? col : row) & 1;
    return reinterpret_cast<Scalar(&)[2]>(cscalar)[p];
  }

  template <bool Enable = std::is_reference<ComplexCoeffReturnType>::value, typename = std::enable_if_t<!Enable>>
  constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Scalar coeff(Index index) const {
    ComplexCoeffReturnType cscalar = BaseEvaluator::coeff(index / 2);
    Index p = index & 1;
    return p ? numext::real(cscalar) : numext::imag(cscalar);
  }

  template <bool Enable = std::is_reference<ComplexCoeffReturnType>::value, typename = std::enable_if_t<Enable>>
  constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE const Scalar& coeff(Index index) const {
    ComplexCoeffReturnType cscalar = BaseEvaluator::coeff(index / 2);
    Index p = index & 1;
    return reinterpret_cast<const Scalar(&)[2]>(cscalar)[p];
  }

  constexpr EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Scalar& coeffRef(Index index) {
    ComplexScalar& cscalar = BaseEvaluator::coeffRef(index / 2);
    Index p = index & 1;
    return reinterpret_cast<Scalar(&)[2]>(cscalar)[p];
  }

  template <int LoadMode, typename PacketType>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE PacketType packet(Index row, Index col) const {
    constexpr int RealPacketSize = unpacket_traits<PacketType>::size;
    using ComplexPacket = typename find_packet_by_size<ComplexScalar, RealPacketSize / 2>::type;
    EIGEN_STATIC_ASSERT((find_packet_by_size<ComplexScalar, RealPacketSize / 2>::value),
                        MISSING COMPATIBLE COMPLEX PACKET TYPE)
    eigen_assert(((IsRowMajor ? col : row) % 2 == 0) && "the inner index must be even");

    Index crow = IsRowMajor ? row : row / 2;
    Index ccol = IsRowMajor ? col / 2 : col;
    ComplexPacket cpacket = BaseEvaluator::template packet<LoadMode, ComplexPacket>(crow, ccol);
    return preinterpret<PacketType, ComplexPacket>(cpacket);
  }

  template <int LoadMode, typename PacketType>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE PacketType packet(Index index) const {
    constexpr int RealPacketSize = unpacket_traits<PacketType>::size;
    using ComplexPacket = typename find_packet_by_size<ComplexScalar, RealPacketSize / 2>::type;
    EIGEN_STATIC_ASSERT((find_packet_by_size<ComplexScalar, RealPacketSize / 2>::value),
                        MISSING COMPATIBLE COMPLEX PACKET TYPE)
    eigen_assert((index % 2 == 0) && "the index must be even");

    Index cindex = index / 2;
    ComplexPacket cpacket = BaseEvaluator::template packet<LoadMode, ComplexPacket>(cindex);
    return preinterpret<PacketType, ComplexPacket>(cpacket);
  }

  template <int LoadMode, typename PacketType>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE PacketType packetSegment(Index row, Index col, Index begin, Index count) const {
    constexpr int RealPacketSize = unpacket_traits<PacketType>::size;
    using ComplexPacket = typename find_packet_by_size<ComplexScalar, RealPacketSize / 2>::type;
    EIGEN_STATIC_ASSERT((find_packet_by_size<ComplexScalar, RealPacketSize / 2>::value),
                        MISSING COMPATIBLE COMPLEX PACKET TYPE)
    eigen_assert(((IsRowMajor ? col : row) % 2 == 0) && "the inner index must be even");
    eigen_assert((begin % 2 == 0) && (count % 2 == 0) && "begin and count must be even");

    Index crow = IsRowMajor ? row : row / 2;
    Index ccol = IsRowMajor ? col / 2 : col;
    Index cbegin = begin / 2;
    Index ccount = count / 2;
    ComplexPacket cpacket = BaseEvaluator::template packetSegment<LoadMode, ComplexPacket>(crow, ccol, cbegin, ccount);
    return preinterpret<PacketType, ComplexPacket>(cpacket);
  }

  template <int LoadMode, typename PacketType>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE PacketType packetSegment(Index index, Index begin, Index count) const {
    constexpr int RealPacketSize = unpacket_traits<PacketType>::size;
    using ComplexPacket = typename find_packet_by_size<ComplexScalar, RealPacketSize / 2>::type;
    EIGEN_STATIC_ASSERT((find_packet_by_size<ComplexScalar, RealPacketSize / 2>::value),
                        MISSING COMPATIBLE COMPLEX PACKET TYPE)
    eigen_assert((index % 2 == 0) && "the index must be even");
    eigen_assert((begin % 2 == 0) && (count % 2 == 0) && "begin and count must be even");

    Index cindex = index / 2;
    Index cbegin = begin / 2;
    Index ccount = count / 2;
    ComplexPacket cpacket = BaseEvaluator::template packetSegment<LoadMode, ComplexPacket>(cindex, cbegin, ccount);
    return preinterpret<PacketType, ComplexPacket>(cpacket);
  }
};

}  // namespace internal

template <typename Xpr>
class RealView : public internal::dense_xpr_base<RealView<Xpr>>::type {
  using ExpressionTraits = internal::traits<RealView>;
  EIGEN_STATIC_ASSERT(NumTraits<typename Xpr::Scalar>::IsComplex, SCALAR MUST BE COMPLEX)
 public:
  using Scalar = typename ExpressionTraits::Scalar;
  using Nested = RealView;

  EIGEN_DEVICE_FUNC explicit RealView(Xpr& xpr) : m_xpr(xpr) {}
  EIGEN_DEVICE_FUNC constexpr Index rows() const noexcept { return Xpr::IsRowMajor ? m_xpr.rows() : 2 * m_xpr.rows(); }
  EIGEN_DEVICE_FUNC constexpr Index cols() const noexcept { return Xpr::IsRowMajor ? 2 * m_xpr.cols() : m_xpr.cols(); }
  EIGEN_DEVICE_FUNC constexpr Index size() const noexcept { return 2 * m_xpr.size(); }
  EIGEN_DEVICE_FUNC constexpr Index innerStride() const noexcept { return m_xpr.innerStride(); }
  EIGEN_DEVICE_FUNC constexpr Index outerStride() const noexcept { return 2 * m_xpr.outerStride(); }
  EIGEN_DEVICE_FUNC void resize(Index rows, Index cols) {
    m_xpr.resize(Xpr::IsRowMajor ? rows : rows / 2, Xpr::IsRowMajor ? cols / 2 : cols);
  }
  EIGEN_DEVICE_FUNC void resize(Index size) { m_xpr.resize(size / 2); }
  EIGEN_DEVICE_FUNC Scalar* data() { return reinterpret_cast<Scalar*>(m_xpr.data()); }
  EIGEN_DEVICE_FUNC const Scalar* data() const { return reinterpret_cast<const Scalar*>(m_xpr.data()); }

  EIGEN_DEVICE_FUNC RealView(const RealView&) = default;

  EIGEN_DEVICE_FUNC RealView& operator=(const RealView& other);

  template <typename OtherDerived>
  EIGEN_DEVICE_FUNC RealView& operator=(const RealView<OtherDerived>& other);

  template <typename OtherDerived>
  EIGEN_DEVICE_FUNC RealView& operator=(const DenseBase<OtherDerived>& other);

 protected:
  friend struct internal::evaluator<RealView<Xpr>>;
  Xpr& m_xpr;
};

template <typename Xpr>
EIGEN_DEVICE_FUNC RealView<Xpr>& RealView<Xpr>::operator=(const RealView& other) {
  internal::call_assignment(*this, other);
  return *this;
}

template <typename Xpr>
template <typename OtherDerived>
EIGEN_DEVICE_FUNC RealView<Xpr>& RealView<Xpr>::operator=(const RealView<OtherDerived>& other) {
  internal::call_assignment(*this, other);
  return *this;
}

template <typename Xpr>
template <typename OtherDerived>
EIGEN_DEVICE_FUNC RealView<Xpr>& RealView<Xpr>::operator=(const DenseBase<OtherDerived>& other) {
  internal::call_assignment(*this, other.derived());
  return *this;
}

template <typename Derived>
EIGEN_DEVICE_FUNC typename DenseBase<Derived>::RealViewReturnType DenseBase<Derived>::realView() {
  return RealViewReturnType(derived());
}

template <typename Derived>
EIGEN_DEVICE_FUNC typename DenseBase<Derived>::ConstRealViewReturnType DenseBase<Derived>::realView() const {
  return ConstRealViewReturnType(derived());
}

}  // namespace Eigen

#endif  // EIGEN_REALVIEW_H
