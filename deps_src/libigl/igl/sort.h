// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SORT_H
#define IGL_SORT_H
#include "igl_inline.h"

#include <vector>
#include <Eigen/Core>
namespace igl
{

  /// Sort the elements of a matrix X along a given dimension like matlabs sort
  /// function
  ///
  /// @tparam DerivedX derived scalar type, e.g. MatrixXi or MatrixXd
  /// @tparam DerivedIX derived integer type, e.g. MatrixXi
  /// @param[in] X  m by n matrix whose entries are to be sorted
  /// @param[in] dim  dimensional along which to sort:
  ///     1  sort each column (matlab default)
  ///     2  sort each row
  /// @param[in] ascending  sort ascending (true, matlab default) or descending (false)
  /// @param[out] Y  m by n matrix whose entries are sorted
  /// @param[out] IX  m by n matrix of indices so that if dim = 1, then in matlab notation
  ///     for j = 1:n, Y(:,j) = X(I(:,j),j); end
  template <typename DerivedX, typename DerivedY, typename DerivedIX>
  IGL_INLINE void sort(
    const Eigen::DenseBase<DerivedX>& X,
    const int dim,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedIX>& IX);
  /// \overload
  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void sort(
    const Eigen::DenseBase<DerivedX>& X,
    const int dim,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y);
  /// \overload
  ///
  /// \note This should be renamed to something like sort_small because it is
  /// only faster if size(X,dim) is small.
  template <typename DerivedX, typename DerivedY, typename DerivedIX>
  IGL_INLINE void sort_new(
    const Eigen::DenseBase<DerivedX>& X,
    const int dim,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedIX>& IX);
  /// \overload
  /// \brief Special case if size(X,dim) == 2
  template <typename DerivedX, typename DerivedY, typename DerivedIX>
  IGL_INLINE void sort2(
    const Eigen::DenseBase<DerivedX>& X,
    const int dim,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedIX>& IX);
  /// \overload
  /// \brief Special case if size(X,dim) == 3
  template <typename DerivedX, typename DerivedY, typename DerivedIX>
  IGL_INLINE void sort3(
    const Eigen::DenseBase<DerivedX>& X,
    const int dim,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedIX>& IX);
  /// Act like matlab's [Y,I] = SORT(X) for std library vectors
  /// @tparam T  should be a class that implements the '<' comparator operator
  /// @param[in] unsorted  unsorted vector
  /// @param[in] ascending  sort ascending (true, matlab default) or descending (false)
  /// @param[out] sorted     sorted vector, allowed to be same as unsorted
  /// @param[out] index_map  an index map such that sorted[i] = unsorted[index_map[i]]
  template <class T>
  IGL_INLINE void sort(
    const std::vector<T> &unsorted,
    const bool ascending,
    std::vector<T> &sorted,
    std::vector<size_t> &index_map);
}

#ifndef IGL_STATIC_LIBRARY
#  include "sort.cpp"
#endif

#endif
