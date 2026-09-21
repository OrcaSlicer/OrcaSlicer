// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HARMONIC_H
#define IGL_HARMONIC_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Compute k-harmonic weight functions "coordinates".
  ///
  /// @param[in] V  #V by dim vertex positions
  /// @param[in] F  #F by simplex-size list of element indices
  /// @param[in] b  #b boundary indices into V
  /// @param[in] bc #b by #W list of boundary values
  /// @param[in] k  power of harmonic operation (1: harmonic, 2: biharmonic, etc)
  /// @param[out] W  #V by #W list of weights
  ///
  template <
    typename DerivedV,
    typename DerivedF,
    typename Derivedb,
    typename Derivedbc,
    typename DerivedW>
  IGL_INLINE bool harmonic(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<Derivedb> & b,
    const Eigen::MatrixBase<Derivedbc> & bc,
    const int k,
    Eigen::PlainObjectBase<DerivedW> & W);
  /// \overload
  /// \brief Compute harmonic map using uniform laplacian operator
  template <
    typename DerivedF,
    typename Derivedb,
    typename Derivedbc,
    typename DerivedW>
  IGL_INLINE bool harmonic(
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<Derivedb> & b,
    const Eigen::MatrixBase<Derivedbc> & bc,
    const int k,
    Eigen::PlainObjectBase<DerivedW> & W);
  /// \overload
  /// Compute a harmonic map using a given Laplacian and mass matrix
  ///
  /// @param[in] L  #V by #V discrete (integrated) Laplacian  
  ///  @param[in] M  #V by #V mass matrix
  template <
    typename DerivedL,
    typename DerivedM,
    typename Derivedb,
    typename Derivedbc,
    typename DerivedW>
  IGL_INLINE bool harmonic(
    const Eigen::SparseCompressedBase<DerivedL> & L,
    const Eigen::SparseCompressedBase<DerivedM> & M,
    const Eigen::MatrixBase<Derivedb> & b,
    const Eigen::MatrixBase<Derivedbc> & bc,
    const int k,
    Eigen::PlainObjectBase<DerivedW> & W);
  /// Build the discrete k-harmonic operator (computing integrated quantities).
  /// That is, if the k-harmonic PDE is Q x = 0, then this minimizes x' Q x
  ///
  /// @param[in] L  #V by #V discrete (integrated) Laplacian  
  /// @param[in] M  #V by #V mass matrix
  /// @param[in] k  power of harmonic operation (1: harmonic, 2: biharmonic, etc)
  /// @param[out] Q  #V by #V discrete (integrated) k-Laplacian  
  template <
    typename DerivedL,
    typename DerivedM,
    typename DerivedQ>
  IGL_INLINE void harmonic(
    const Eigen::SparseCompressedBase<DerivedL> & L,
    const Eigen::SparseCompressedBase<DerivedM> & M,
    const int k,
    DerivedQ & Q);
  /// \overload
  /// @param[in] V  #V by dim vertex positions
  /// @param[in] F  #F by simplex-size list of element indices
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedQ>
  IGL_INLINE void harmonic(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const int k,
    DerivedQ & Q);
};

#ifndef IGL_STATIC_LIBRARY
#include "harmonic.cpp"
#endif
#endif
