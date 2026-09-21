// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2023 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.


#ifndef IGL_ISOLINES_INTRINSIC_H
#define IGL_ISOLINES_INTRINSIC_H
#include "igl_inline.h"

#include <Eigen/Core>


namespace igl
{
  /// Compute isolines of a scalar field on a triangle mesh intrinsically.
  ///
  /// See isolines.h for details.
  ///
  /// @param[in] F  #F by 3 list of mesh triangle indices into some V
  /// @param[in] S  #S by 1 list of per-vertex scalar values
  /// @param[in] vals  #vals by 1 list of values to compute isolines for
  /// @param[out] iB  #iB by 3 list of barycentric coordinates so that 
  ///   iV.row(i) = iB(i,0)*V.row(F(iFI(i,0)) +
  ///               iB(i,1)*V.row(F(iFI(i,1)) +
  ///               iB(i,2)*V.row(F(iFI(i,2))
  /// @param[out] iF  #iB list of triangle indices for each row of iB (all
  ///   points will either lie on an edge or vertex: an arbitrary incident face
  ///   will be given).
  /// @param[out] iE  #iE by 2 list of edge indices into iB
  /// @param[out] I  #iE by 1 list of indices into vals indicating which value
  ///   each segment belongs to
  ///
  /// \see isolines, edge_crossings
  template <
    typename DerivedF,
    typename DerivedS,
    typename Derivedvals,
    typename DerivediB,
    typename DerivediFI,
    typename DerivediE,
    typename DerivedI>
  void isolines_intrinsic(
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<Derivedvals> & vals,
    Eigen::PlainObjectBase<DerivediB> & iB,
    Eigen::PlainObjectBase<DerivediFI> & iFI,
    Eigen::PlainObjectBase<DerivediE> & iE,
    Eigen::PlainObjectBase<DerivedI> & I);
  /// \overload
  ///
  /// @param[in] val  scalar value to compute isoline at
  /// @param[in] uE  #uE by 2 list of unique undirected edges
  /// @param[in] EMAP #F*3 list of indices into uE, mapping each directed edge to unique
  ///     undirected edge so that uE(EMAP(f+#F*c)) is the unique edge
  ///     corresponding to E.row(f+#F*c)
  /// @param[in] uEC  #uE+1 list of cumulative counts of directed edges sharing each
  ///     unique edge so the uEC(i+1)-uEC(i) is the number of directed edges
  ///     sharing the ith unique edge.
  /// @param[in] uEE  #E list of indices into E, so that the consecutive segment of
  ///     indices uEE.segment(uEC(i),uEC(i+1)-uEC(i)) lists all directed edges
  ///     sharing the ith unique edge.
  ///
  /// \see unique_edge_map
  template <
    typename DerivedF,
    typename DerivedS,
    typename DeriveduE,
    typename DerivedEMAP,
    typename DeriveduEC,
    typename DeriveduEE,
    typename DerivediB,
    typename DerivediFI,
    typename DerivediE>
  void isolines_intrinsic(
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<DeriveduE> & uE,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DeriveduEC> & uEC,
    const Eigen::MatrixBase<DeriveduEE> & uEE,
    const typename DerivedS::Scalar val,
    Eigen::PlainObjectBase<DerivediB> & iB,
    Eigen::PlainObjectBase<DerivediFI> & iFI,
    Eigen::PlainObjectBase<DerivediE> & iE);
}

#ifndef IGL_STATIC_LIBRARY
#  include "isolines_intrinsic.cpp"
#endif

#endif

