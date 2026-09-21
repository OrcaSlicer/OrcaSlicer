// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2023 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SPECTRA_EIGS_H
#define IGL_SPECTRA_EIGS_H
#include "../igl_inline.h"
#include "../eigs.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
#include <Eigen/SparseLU>

namespace igl
{
  namespace spectra
  {
    /// Act like MATLAB's eigs function. Compute the first/last k eigen pairs of
    /// the generalized eigen value problem:
    ///
    ///      A u = s B u
    ///
    /// Solutions are approximate and sorted. 
    ///
    /// Ideally one should use ARPACK and the Eigen unsupported ARPACK module.
    /// This implementation does simple, naive power iterations.
    ///
    /// @param[in] A  #A by #A symmetric matrix
    /// @param[in] B  #A by #A symmetric positive-definite matrix
    /// @param[in] k  number of eigen pairs to compute
    /// @param[in] type  whether to extract from the high or low end
    /// @param[out] sU  #A by k list of sorted eigen vectors (descending)
    /// @param[out] sS  k list of sorted eigen values (descending)
    ///
    /// \bug only the 'sm' small magnitude eigen values are well supported
    ///   
    template <
      typename EigsScalar,
      typename DerivedU,
      typename DerivedS,
      typename Solver = Eigen::SparseLU<Eigen::SparseMatrix<EigsScalar>> >
    IGL_INLINE bool eigs(
      const Eigen::SparseMatrix<EigsScalar> & A,
      const Eigen::SparseMatrix<EigsScalar> & B,
      const int k,
      const igl::EigsType type,
      Eigen::PlainObjectBase<DerivedU> & U,
      Eigen::PlainObjectBase<DerivedS> & S);
    /// \overload
    /// @param[in] sigma  shift to apply to A, as in A ← A + sigma B
    template <
      typename EigsScalar,
      typename DerivedU,
      typename DerivedS,
      typename Solver = Eigen::SparseLU<Eigen::SparseMatrix<EigsScalar>> >
    IGL_INLINE bool eigs(
      const Eigen::SparseMatrix<EigsScalar> & A,
      const Eigen::SparseMatrix<EigsScalar> & B,
      const int k,
      const EigsScalar sigma,
      Eigen::PlainObjectBase<DerivedU> & U,
      Eigen::PlainObjectBase<DerivedS> & S);
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "eigs.cpp"
#endif
#endif
