// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_REORIENT_FACETS_RAYCAST_H
#define IGL_EMBREE_REORIENT_FACETS_RAYCAST_H
#include "../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace embree
  {
    /// Orient each component (identified by C) of a mesh (V,F) using ambient
    /// occlusion such that the front side is less occluded than back side, as
    /// described in "A Simple Method for Correcting Facet Orientations in
    /// Polygon Meshes Based on Ray Casting" [Takayama et al. 2014].
    ///
    /// @param[in] V  #V by 3 list of vertex positions
    /// @param[in] F  #F by 3 list of triangle indices
    /// @param[in] rays_total  Total number of rays that will be shot
    /// @param[in] rays_minimum  Minimum number of rays that each patch should receive
    /// @param[in] facet_wise  Decision made for each face independently, no use of patches
    ///     (i.e., each face is treated as a patch)
    /// @param[in] use_parity  Use parity mode
    /// @param[in] is_verbose  Verbose output to cout
    /// @param[out] I  #F list of whether face has been flipped
    /// @param[out] C  #F list of patch ID (output of bfs_orient > manifold patches)
    template <
      typename DerivedV, 
      typename DerivedF, 
      typename DerivedI,
      typename DerivedC>
    IGL_INLINE void reorient_facets_raycast(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      int rays_total,
      int rays_minimum,
      bool facet_wise,
      bool use_parity,
      bool is_verbose,
      Eigen::PlainObjectBase<DerivedI> & I,
      Eigen::PlainObjectBase<DerivedC> & C);
    /// @param[out]  FF  #F by 3 list of reoriented faces
    ///
    /// #### Defaults:
    ///      rays_total = F.rows()*100;
    ///      rays_minimum = 10;
    ///      facet_wise = false;
    ///      use_parity = false;
    ///      is_verbose = false;
    template <
      typename DerivedV, 
      typename DerivedF, 
      typename DerivedFF,
      typename DerivedI>
    IGL_INLINE void reorient_facets_raycast(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      Eigen::PlainObjectBase<DerivedFF> & FF,
      Eigen::PlainObjectBase<DerivedI> & I);
  }
};

#ifndef IGL_STATIC_LIBRARY
#  include "reorient_facets_raycast.cpp"
#endif

#endif
