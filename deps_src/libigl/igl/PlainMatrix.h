// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2024 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_PLAINMATRIX_H
#define IGL_PLAINMATRIX_H
#include <Eigen/Core>

#include <type_traits>
#include <Eigen/Dense>

// Define void_t for compatibility if it's not in the standard library (C++11 and later)
#if __cplusplus < 201703L
namespace std {
  template <typename... Ts>
  using void_t = void;
}
#endif


#ifndef IGL_DEFAULT_MAJORING
#define IGL_DEFAULT_MAJORING Eigen::ColMajor
#endif

namespace igl
{
  template <typename Derived, int Rows, int Cols, int Options>
  struct PlainMatrixHelper {
    using Type = Eigen::Matrix<typename Derived::Scalar,Rows,Cols,((Rows == 1 && Cols != 1) ? Eigen::RowMajor : ((Cols == 1 && Rows != 1) ? Eigen::ColMajor : Options))>;
  };
  template <typename Derived, typename = void>
  struct get_options {
    static constexpr int value = IGL_DEFAULT_MAJORING;
  };
  
  template <typename Derived>
  struct get_options<Derived, std::void_t<decltype(Derived::Options)>> {
    static constexpr int value = Derived::Options;
  };
  /// Some libigl implementations would (still do?) use a pattern like:
  ///
  /// template <typename DerivedA>
  /// void foo(const Eigen::MatrixBase<DerivedA>& A)
  /// {
  ///    DerivedA B;
  ///    igl::unique_rows(A,true,B);
  /// }
  ///
  /// If `DerivedA` is `Eigen::Matrix`, then this may compile, but `DerivedA` might be
  /// from a Eigen::Map or Eigen::Ref and fail to compile due to missing
  /// construtor.
  ///
  /// Even worse, the code above will work if `DerivedA` has dynamic rows, but will
  /// throw a runtime error if `DerivedA` has fixed number of rows.
  ///
  /// Instead it's better to declare `B` as a `Eigen::Matrix`
  ///
  /// Eigen::Matrix<typename DerivedA::Scalar,Eigen::Dynamic,DerivedA::ColsAtCompileTime,DerivedA::Options> B;
  ///
  /// Using `Eigen::Dynamic` for dimensions that may not be known at compile
  /// time (or may be different from A).
  ///
  /// `igl::PlainMatrix` is just a helper to make this easier. So in this case
  /// we could write:
  ///
  /// igl::PlainMatrix<DerivedA,Eigen::Dynamic> B;
  ///
  /// IIUC, if the code in question looks like:
  ///
  /// template <typename DerivedC>
  /// void foo(Eigen::PlainObjectBase<DerivedC>& C)
  /// {
  ///    DerivedC B;
  ///    …
  ///    C.resize(not_known_at_compile_time,also_not_known_at_compile_time);
  /// }
  ///
  /// Then it's probably fine. If C can be resized to different sizes, then
  /// `DerivedC` should be `Eigen::Matrix`-like .
  // Helper to check if `Options` exists in Derived

  // Modify PlainMatrix to use get_options
  template <typename Derived, 
            int Rows = Derived::RowsAtCompileTime, 
            int Cols = Derived::ColsAtCompileTime, 
            int Options = get_options<Derived>::value>
  using PlainMatrix = typename PlainMatrixHelper<Derived, Rows, Cols, Options>::Type;

}

#endif
