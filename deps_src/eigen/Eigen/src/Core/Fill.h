// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2024 Charles Schlosser <cs.schlosser@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_FILL_H
#define EIGEN_FILL_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

template <typename Xpr>
struct eigen_fill_helper : std::false_type {};

template <typename Scalar, int Rows, int Cols, int Options, int MaxRows, int MaxCols>
struct eigen_fill_helper<Matrix<Scalar, Rows, Cols, Options, MaxRows, MaxCols>> : std::true_type {};

template <typename Scalar, int Rows, int Cols, int Options, int MaxRows, int MaxCols>
struct eigen_fill_helper<Array<Scalar, Rows, Cols, Options, MaxRows, MaxCols>> : std::true_type {};

template <typename Xpr, int BlockRows, int BlockCols>
struct eigen_fill_helper<Block<Xpr, BlockRows, BlockCols, /*InnerPanel*/ true>> : eigen_fill_helper<Xpr> {};

template <typename Xpr, int BlockRows, int BlockCols>
struct eigen_fill_helper<Block<Xpr, BlockRows, BlockCols, /*InnerPanel*/ false>>
    : std::integral_constant<bool, eigen_fill_helper<Xpr>::value &&
                                       (Xpr::IsRowMajor ? (BlockRows == 1) : (BlockCols == 1))> {};

template <typename Xpr, int Options>
struct eigen_fill_helper<Map<Xpr, Options, Stride<0, 0>>> : eigen_fill_helper<Xpr> {};

template <typename Xpr, int Options, int OuterStride_>
struct eigen_fill_helper<Map<Xpr, Options, Stride<OuterStride_, 0>>>
    : std::integral_constant<bool, eigen_fill_helper<Xpr>::value &&
                                       enum_eq_not_dynamic(OuterStride_, Xpr::InnerSizeAtCompileTime)> {};

template <typename Xpr, int Options, int OuterStride_>
struct eigen_fill_helper<Map<Xpr, Options, Stride<OuterStride_, 1>>>
    : eigen_fill_helper<Map<Xpr, Options, Stride<OuterStride_, 0>>> {};

template <typename Xpr, int Options, int InnerStride_>
struct eigen_fill_helper<Map<Xpr, Options, InnerStride<InnerStride_>>>
    : eigen_fill_helper<Map<Xpr, Options, Stride<0, InnerStride_>>> {};

template <typename Xpr, int Options, int OuterStride_>
struct eigen_fill_helper<Map<Xpr, Options, OuterStride<OuterStride_>>>
    : eigen_fill_helper<Map<Xpr, Options, Stride<OuterStride_, 0>>> {};

template <typename Xpr>
struct eigen_fill_impl<Xpr, /*use_fill*/ false> {
  using Scalar = typename Xpr::Scalar;
  using Func = scalar_constant_op<Scalar>;
  using PlainObject = typename Xpr::PlainObject;
  using Constant = typename PlainObject::ConstantReturnType;
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void run(Xpr& dst, const Scalar& val) {
    const Constant src(dst.rows(), dst.cols(), val);
    run(dst, src);
  }
  template <typename SrcXpr>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void run(Xpr& dst, const SrcXpr& src) {
    call_dense_assignment_loop(dst, src, assign_op<Scalar, Scalar>());
  }
};

#if EIGEN_COMP_MSVC || defined(EIGEN_GPU_COMPILE_PHASE)
template <typename Xpr>
struct eigen_fill_impl<Xpr, /*use_fill*/ true> : eigen_fill_impl<Xpr, /*use_fill*/ false> {};
#else
template <typename Xpr>
struct eigen_fill_impl<Xpr, /*use_fill*/ true> {
  using Scalar = typename Xpr::Scalar;
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(Xpr& dst, const Scalar& val) {
    const Scalar val_copy = val;
    using std::fill_n;
    fill_n(dst.data(), dst.size(), val_copy);
  }
  template <typename SrcXpr>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(Xpr& dst, const SrcXpr& src) {
    resize_if_allowed(dst, src, assign_op<Scalar, Scalar>());
    const Scalar& val = src.functor()();
    run(dst, val);
  }
};
#endif

template <typename Xpr>
struct eigen_memset_helper {
  static constexpr bool value =
      std::is_trivially_copyable<typename Xpr::Scalar>::value && eigen_fill_helper<Xpr>::value;
};

template <typename Xpr>
struct eigen_zero_impl<Xpr, /*use_memset*/ false> {
  using Scalar = typename Xpr::Scalar;
  using PlainObject = typename Xpr::PlainObject;
  using Zero = typename PlainObject::ZeroReturnType;
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void run(Xpr& dst) {
    const Zero src(dst.rows(), dst.cols());
    run(dst, src);
  }
  template <typename SrcXpr>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void run(Xpr& dst, const SrcXpr& src) {
    call_dense_assignment_loop(dst, src, assign_op<Scalar, Scalar>());
  }
};

template <typename Xpr>
struct eigen_zero_impl<Xpr, /*use_memset*/ true> {
  using Scalar = typename Xpr::Scalar;
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(Xpr& dst) {
    const std::ptrdiff_t num_bytes = dst.size() * static_cast<std::ptrdiff_t>(sizeof(Scalar));
    if (num_bytes <= 0) return;
    void* dst_ptr = static_cast<void*>(dst.data());
#ifndef EIGEN_NO_DEBUG
    eigen_assert((dst_ptr != nullptr) && "null pointer dereference error!");
#endif
    EIGEN_USING_STD(memset);
    memset(dst_ptr, 0, static_cast<std::size_t>(num_bytes));
  }
  template <typename SrcXpr>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void run(Xpr& dst, const SrcXpr& src) {
    resize_if_allowed(dst, src, assign_op<Scalar, Scalar>());
    run(dst);
  }
};

}  // namespace internal
}  // namespace Eigen

#endif  // EIGEN_FILL_H
