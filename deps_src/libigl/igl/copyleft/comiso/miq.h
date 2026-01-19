// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>, Kevin Walliman <wkevin@student.ethz.ch>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COMISO_MIQ_H
#define IGL_COMISO_MIQ_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace copyleft
  {
  namespace comiso
  {
  // Global seamless parametrization aligned with a given per-face jacobian (PD1,PD2).
    // The algorithm is based on
    // "Mixed-Integer Quadrangulation" by D. Bommes, H. Zimmer, L. Kobbelt
    // ACM SIGGRAPH 2009, Article No. 77 (http://dl.acm.org/citation.cfm?id=1531383)
    // We thank Nico Pietroni for providing a reference implementation of MIQ
    // on which our code is based.

    // Inputs:
    //   V              #V by 3 list of mesh vertex 3D positions
    //   F              #F by 3 list of faces indices in V
    //   PD1            #V by 3 first line of the Jacobian per triangle
    //   PD2            #V by 3 second line of the Jacobian per triangle
    //                  (optional, if empty it will be a vector in the tangent plane orthogonal to PD1)
    //   scale          global scaling for the gradient (controls the quads resolution)
    //   stiffness      weight for the stiffness iterations
    //   direct_round   greedily round all integer variables at once (greatly improves optimization speed but lowers quality)
    //   iter           stiffness iterations (0 = no stiffness)
    //   local_iter     number of local iterations for the integer rounding
    //   do_round       enables the integer rounding (disabling it could be useful for debugging)
    //   round_vertices id of additional vertices that should be snapped to integer coordinates
    //   hard_features  #H by 2 list of pairs of vertices that belongs to edges that should be snapped to integer coordinates
    //
    // Output:
    //   UV             #UV by 2 list of vertices in 2D
    //   FUV            #FUV by 3 list of face indices in UV
    //
    // TODO: rename the parameters name in the cpp consistently
    //       improve the handling of hard_features, right now it might fail in difficult cases

    template <typename DerivedV, typename DerivedF, typename DerivedU>
    IGL_INLINE void miq(
      const Eigen::PlainObjectBase<DerivedV> &V,
      const Eigen::PlainObjectBase<DerivedF> &F,
      const Eigen::PlainObjectBase<DerivedV> &PD1,
      const Eigen::PlainObjectBase<DerivedV> &PD2,
      Eigen::PlainObjectBase<DerivedU> &UV,
      Eigen::PlainObjectBase<DerivedF> &FUV,
      double scale = 30.0,
      double stiffness = 5.0,
      bool direct_round = false,
      int iter = 5,
      int local_iter = 5,
      bool DoRound = true,bool SingularityRound=true,
      std::vector<int> round_vertices = std::vector<int>(),
      std::vector<std::vector<int> > hard_features = std::vector<std::vector<int> >());

    // Helper function that allows to directly provided pre-combed bisectors for an already cut mesh
    // Additional input:
    // PD1_combed, PD2_combed  :   #F by 3 combed jacobian
    // BIS1_combed, BIS2_combed:   #F by 3 pre combed bi-sectors
    // MMatch:                     #F by 3 list of per-corner integer PI/2 rotations
    // Singular:                   #V list of flag that denotes if a vertex is singular or not
    // SingularDegree:             #V list of flag that denotes the degree of the singularity
    // Seams:                      #F by 3 list of per-corner flag that denotes seams

    template <typename DerivedV, typename DerivedF, typename DerivedU>
    IGL_INLINE void miq(const Eigen::PlainObjectBase<DerivedV> &V,
      const Eigen::PlainObjectBase<DerivedF> &F,
      const Eigen::PlainObjectBase<DerivedV> &PD1_combed,
      const Eigen::PlainObjectBase<DerivedV> &PD2_combed,
      const Eigen::Matrix<int, Eigen::Dynamic, 3> &MMatch,
      const Eigen::Matrix<int, Eigen::Dynamic, 1> &Singular,
      const Eigen::Matrix<int, Eigen::Dynamic, 3> &Seams,
      Eigen::PlainObjectBase<DerivedU> &UV,
      Eigen::PlainObjectBase<DerivedF> &FUV,
      double GradientSize = 30.0,
      double Stiffness = 5.0,
      bool DirectRound = false,
      int iter = 5,
      int localIter = 5, bool DoRound = true,bool SingularityRound=true,
      std::vector<int> roundVertices = std::vector<int>(),
      std::vector<std::vector<int> > hardFeatures = std::vector<std::vector<int> >());
  };
};
};
#ifndef IGL_STATIC_LIBRARY
#include "miq.cpp"
#endif

#endif
