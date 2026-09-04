// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CIRCULATION_H
#define IGL_CIRCULATION_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Return list of faces around the end point of an edge. Assumes
  /// data-structures are built from an edge-manifold **closed** mesh.
  ///
  /// @param[in] e  index into E of edge to circulate
  /// @param[in] ccw  whether to _continue_ in ccw direction of edge (circulate around
  ///     E(e,1))
  /// @param[in] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[in] EI  #E by 2 list of edge flap corners (see above).
  /// @return list of faces touched by circulation (in cyclically order).
  ///   
  /// \see edge_flaps
  template <typename DerivedEMAP, typename DerivedEF, typename DerivedEI>
  IGL_INLINE std::vector<int> circulation(
    const int e,
    const bool ccw,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DerivedEF> & EF,
    const Eigen::MatrixBase<DerivedEI> & EI);
  /// Return list of faces around the end point of an edge. Assumes
  /// data-structures are built from an edge-manifold **closed** mesh.
  ///
  /// @param[in] e  index into E of edge to circulate
  /// @param[in] ccw  whether to _continue_ in ccw direction of edge (circulate around
  ///     E(e,1))
  /// @param[in] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[in] EI  #E by 2 list of edge flap corners (see above).
  /// @param[out] #vN list of of faces touched by circulation (in cyclically order).
  ///   
  /// \see edge_flaps
  template <typename DerivedEMAP, typename DerivedEF, typename DerivedEI, typename DerivedvN>
  IGL_INLINE void circulation(
    const int e,
    const bool ccw,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DerivedEF> & EF,
    const Eigen::MatrixBase<DerivedEI> & EI,
    Eigen::PlainObjectBase<DerivedvN> & vN);
  /// Return list of faces around the end point of an edge. Assumes
  /// data-structures are built from an edge-manifold **closed** mesh.
  ///
  /// @param[in] e  index into E of edge to circulate
  /// @param[in] ccw  whether to _continue_ in ccw direction of edge (circulate around
  ///     E(e,1))
  /// @param[in] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[in] EI  #E by 2 list of edge flap corners (see above).
  ///  @param[out] Nv  #Nv list of "next" vertex indices
  ///  @param[out] Nf  #Nf list of face indices
  ///   
  /// \see edge_flaps
  template <typename DerivedF, typename DerivedEMAP, typename DerivedEF, typename DerivedEI, typename Nv_type>
  IGL_INLINE void circulation(
    const int e,
    const bool ccw,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DerivedEF> & EF,
    const Eigen::MatrixBase<DerivedEI> & EI,
    /*std::vector<int> & Ne,*/
    std::vector<Nv_type> & Nv,
    std::vector<Nv_type> & Nf);
}

#ifndef IGL_STATIC_LIBRARY
#  include "circulation.cpp"
#endif
#endif
