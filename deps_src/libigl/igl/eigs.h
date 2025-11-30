// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EIGS_H
#define IGL_EIGS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  /// Type of eigen values to extract
  enum EigsType
  {
    /// Smallest magnitude eigen values
    EIGS_TYPE_SM = 0,
    /// Largest magnitude eigen values
    EIGS_TYPE_LM = 1,
    /// Total number of types
    NUM_EIGS_TYPES = 2
  };
  /// Compute the first/last k eigen pairs of
  /// the generalized eigen value problem:
  ///
  ///     A u = s B u
  ///
  /// Solutions are approximate and sorted. 
  ///
  /// \note Ideally one should use ARPACK and the Eigen unsupported ARPACK module.
  /// This implementation does simple, naive power iterations.
  /// 
  /// \see spectra::eigs
  ///
  /// @param[in] A  #A by #A symmetric matrix
  /// @param[in] B  #A by #A symmetric positive-definite matrix
  /// @param[in] k  number of eigen pairs to compute
  /// @param[in] type  whether to extract from the high or low end
  /// @param[out] sU  #A by k list of sorted eigen vectors (descending)
  /// @param[out] sS  k list of sorted eigen values (descending)
  ///
  /// \warning only the 'sm' small magnitude eigen values are well supported
  ///   
  template <
    typename Atype,
    typename Btype,
    typename DerivedU,
    typename DerivedS>
  IGL_INLINE bool eigs(
    const Eigen::SparseMatrix<Atype> & A,
    const Eigen::SparseMatrix<Btype> & B,
    const size_t k,
    const EigsType type,
    Eigen::PlainObjectBase<DerivedU> & sU,
    Eigen::PlainObjectBase<DerivedS> & sS);
}

#ifndef IGL_STATIC_LIBRARY
#include "eigs.cpp"
#endif
#endif
