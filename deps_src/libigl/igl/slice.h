// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SLICE_H
#define IGL_SLICE_H
#include "igl_inline.h"
#include "placeholders.h"

/// @file slice.h
///
/// \deprecated The following dense versions are deprecated in favor of using
/// [Eigen v3.4's native
/// slicing](https://eigen.tuxfamily.org/dox-devel/group__TutorialSlicingIndexing.html)
/// which is more efficient, more flexible, and has better syntax.
///
/// | igl                        | Eigen v3.4                         |
/// |-------------------0--------|------------------------------------| |
/// `igl::slice(X,I,J,Y)`      | `Y = X(I,J)`                       | |
/// `igl::slice(X,I,1,Y)`      | `Y = X(I,igl::placeholders::all)`              | |
/// `igl::slice(X,J,2,Y)`      | `Y = X(igl::placeholders::all,J)`              | |
/// `igl::slice_into(Z,I,J,X)` | `X(I,J) = Z`                       | |
/// `igl::slice_into(Z,I,1,X)` | `X(I,igl::placeholders::all) = Z`              | |
/// `igl::slice_into(Z,J,2,X)` | `X(igl::placeholders::all,J) = Z`              | |
/// `igl::slice_mask(X,M,N,Y)` | `Y = X(igl::find(M),igl::find(N))` | | _not
/// available_            | `X(igl::find(M),igl::find(N)) = Z` |
///
/// Eigen's slicing supports much more than arrays of indices as input, as well.
///
/// Unfortunately, Eigen v3.4 does not support slicing on sparse matrices.

#include <Eigen/Sparse>
#include <vector>
namespace igl
{
  /// Act like the matlab X(row_indices,col_indices) operator, where
  /// row_indices, col_indices are non-negative integer indices.
  ///
  /// @param[in] X  m by n matrix
  /// @param[in] R  list of row indices
  /// @param[in] C  list of column indices
  /// @param[out] Y  #R by #C matrix
  ///
  /// \see slice_mask, slice_into
  ///
  /// \note See also Eigen's unaryExpr https://stackoverflow.com/a/49411587/148668
  template <
    typename TX,
    typename TY,
    typename DerivedR,
    typename DerivedC>
  IGL_INLINE void slice(
    const Eigen::SparseMatrix<TX>& X,
    const Eigen::DenseBase<DerivedR> & R,
    const Eigen::DenseBase<DerivedC> & C,
    Eigen::SparseMatrix<TY>& Y);
  /// \overload
  /// \brief Wrapper to only slice in one direction
  ///
  /// @param[in] dim  dimension to slice in 1 or 2, dim=1 --> X(R,:), dim=2 --> X(:,R)
  ///
  /// \note For now this is just a cheap wrapper.
  template <
    typename MatX,
    typename DerivedR,
    typename MatY>
  IGL_INLINE void slice(
    const MatX& X,
    const Eigen::DenseBase<DerivedR> & R,
    const int dim,
    MatY& Y);
  /// \overload
  template< class T >
  IGL_INLINE void slice(
    const std::vector<T> & X,
    std::vector<size_t> const & R,
    std::vector<T> & Y);
  /// \overload
  /// \brief Vector version
  /// \bug these templates are out of order
  template <typename DerivedX, typename DerivedY, typename DerivedR>
  IGL_INLINE void slice(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::DenseBase<DerivedR> & R,
    Eigen::PlainObjectBase<DerivedY> & Y);
  /// \overload
  ///
  /// \deprecated
  /// 
  /// See slice.h for more details
  template <
    typename DerivedX,
    typename DerivedR,
    typename DerivedC,
    typename DerivedY>
  IGL_INLINE void slice(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::DenseBase<DerivedR> & R,
    const Eigen::DenseBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedY> & Y);
  /// \overload
  /// \brief VectorXi Y = slice(X,R);
  /// This templating is bad because the return type might not have the same
  /// size as `DerivedX`. This will probably only work if DerivedX has Dynamic
  /// as it's non-trivial sizes or if the number of rows in R happens to equal
  /// the number of rows in `DerivedX`.
  template <typename DerivedX, typename DerivedR>
  IGL_INLINE DerivedX slice(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::DenseBase<DerivedR> & R);
  /// \overload
  template <typename DerivedX, typename DerivedR>
  IGL_INLINE DerivedX slice(
    const Eigen::DenseBase<DerivedX>& X,
    const Eigen::DenseBase<DerivedR> & R,
    const int dim);
}

#ifndef IGL_STATIC_LIBRARY
#  include "slice.cpp"
#endif

#endif
