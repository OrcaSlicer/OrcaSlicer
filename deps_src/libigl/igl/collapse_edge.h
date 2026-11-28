// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COLLAPSE_EDGE_H
#define IGL_COLLAPSE_EDGE_H
#include "igl_inline.h"
#include "COLLAPSE_EDGE_NULL.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Attempt to collapse a given edge of a mesh. Assumes (V,F) is a closed
  /// manifold mesh (except for previously collapsed faces which should be set
  /// to: [IGL_COLLAPSE_EDGE_NULL IGL_COLLAPSE_EDGE_NULL
  /// IGL_COLLAPSE_EDGE_NULL]. Collapses exactly two faces and exactly 3 edges
  /// from E (e and one side of each face gets collapsed to the other). This is
  /// implemented in a way that it can be repeatedly called until satisfaction
  /// and then the garbage in F can be collected by removing NULL faces.
  ///
  /// @param[in] e  index into E of edge to try to collapse. E(e,:) = [s d] or [d s] so
  ///     that s<d, then d is collapsed to s.
  /// @param[in] p  dim list of vertex position where to place merged vertex
  /// [mesh inputs]
  /// @param[in,out] V  #V by dim list of vertex positions, lesser index of E(e,:) will be set
  ///     to midpoint of edge.
  /// @param[in,out] F  #F by 3 list of face indices into V.
  /// @param[in,out] E  #E by 2 list of edge indices into V.
  /// @param[in,out] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in,out] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[in,out] EI  #E by 2 list of edge flap corners (see above).
  /// [mesh inputs]
  /// @param[out] e1  index into E of edge collpased on left
  /// @param[out] e2  index into E of edge collpased on right
  /// @param[out] f1  index into F of face collpased on left
  /// @param[out] f2  index into F of face collpased on right
  /// @return true if edge was collapsed
  ///
  ///
  /// Define [s,d] = sort(E(e,:)) so that s<d, then d is "detached" from
  /// connectivity meaning all faces/edges incident on d will now be incident on
  /// s. (This reduces fragmentation by preferring to collapse toward the start
  /// of V)¹. If E(e,1)==s then we say the edge is "flipped" (`eflip` true in
  /// the implementation).
  ///
  /// f1 is set to EF(e,0) and f2 is set to EF(e,1). Let v1 be EI(e,0) the
  /// corner of F(f1,:) opposite e. _If_ (s<d) then e1 will be the edge after e
  /// within f1:
  ///
  ///         s<d
  ///     ✅s----e-----d☠️
  ///        \   ←    /
  ///         \ ↘f₁↗ /
  ///         e₁    /
  ///           \  /
  ///            \/
  ///
  /// _If_ (s>d) then e1 will be the edge after e within f1:
  ///    
  ///         s>d
  ///     ✅s----e-----d☠️
  ///        \   ←    /
  ///         \ ↘f₁↗ /
  ///          \    e₁
  ///           \  /
  ///            \/
  ///
  ///
  /// ¹Or at least it would if we templated these functions to allow using
  /// RowMajor V.
  ///
  /// It really seems that this callback should provide a meaningful edge on the
  /// _new_ mesh. Meanwhile – Oof – You can use this gross mechanism to find the faces incident on the
  /// collapsed vertex:
  ///
  /// ```cpp
  /// const auto survivors = 
  ///   [&F,&e,&EMAP](const int f1, const int e1, int & d1)
  /// {
  ///   for(int c=0;c<3;c++)
  ///   {
  ///     d1 = EMAP(f1+c*F.rows());
  ///     if((d1 != e) && (d1 != e1)) { break; }
  ///   }
  /// };
  /// int d1,d2;
  /// survivors(f1,e1,d1);
  /// survivors(f2,e2,d2);
  /// // Will circulating by continuing in the CCW direction of E(d1,:)
  /// // encircle the common edge? That is, is E(d1,1) the common vertex?
  /// const bool ccw = E(d1,1) == E(d2,0) || E(d1,1) == E(d2,1);
  /// std::vector<int> Nf;
  /// {
  ///   std::vector<int> Nv;
  ///   igl::circulation(d1,ccw,F,EMAP,EF,EI,Nv,Nf);
  /// }
  /// ```
  template <
    typename Derivedp,
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename DerivedEI>
  IGL_INLINE bool collapse_edge(
    const int e,
    const Eigen::MatrixBase<Derivedp> & p,
    Eigen::MatrixBase<DerivedV> & V,
    Eigen::MatrixBase<DerivedF> & F,
    Eigen::MatrixBase<DerivedE> & E,
    Eigen::MatrixBase<DerivedEMAP> & EMAP,
    Eigen::MatrixBase<DerivedEF> & EF,
    Eigen::MatrixBase<DerivedEI> & EI,
    int & e1,
    int & e2,
    int & f1,
    int & f2);
  /// \overload
  ///
  /// @param[in] Nsv #Nsv vertex circulation around s (see circulation)
  /// @param[in] Nsf #Nsf face circulation around s
  /// @param[in] Ndv #Ndv vertex circulation around d
  /// @param[in] Ndf #Ndf face circulation around d
  template
  <
    typename Derivedp,
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename DerivedEI>
  IGL_INLINE bool collapse_edge(
    const int e,
    const Eigen::MatrixBase<Derivedp> & p,
    /*const*/ std::vector<int> & Nsv,
    const std::vector<int> & Nsf,
    /*const*/ std::vector<int> & Ndv,
    const std::vector<int> & Ndf,
    Eigen::MatrixBase<DerivedV> & V,
    Eigen::MatrixBase<DerivedF> & F,
    Eigen::MatrixBase<DerivedE> & E,
    Eigen::MatrixBase<DerivedEMAP> & EMAP,
    Eigen::MatrixBase<DerivedEF> & EF,
    Eigen::MatrixBase<DerivedEI> & EI,
    int & a_e1,
    int & a_e2,
    int & a_f1,
    int & a_f2);
}

#ifndef IGL_STATIC_LIBRARY
#  include "collapse_edge.cpp"
#endif
#endif
