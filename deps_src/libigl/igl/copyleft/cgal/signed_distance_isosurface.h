// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_SIGNED_DISTANCE_ISOSURFACE_H
#define IGL_COPYLEFT_CGAL_SIGNED_DISTANCE_ISOSURFACE_H
#include "../../igl_inline.h"
#include "../../signed_distance.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Compute the contour of an iso-level of the signed distance field to a
      /// given mesh.
      ///
      /// @param[in] IV  #IV by 3 list of input mesh vertex positions
      /// @param[in] IF  #IF by 3 list of input triangle indices
      /// @param[in] level  iso-level to contour in world coords, negative is inside.
      /// @param[in] angle_bound  lower bound on triangle angles (mesh quality) (e.g. 28)
      /// @param[in] radius_bound  upper bound on triangle size (mesh density?) (e.g. 0.02)
      /// @param[in] distance_bound  cgal mysterious parameter (mesh density?) (e.g. 0.01)
      /// @param[in] sign_type  method for computing distance _sign_ (see
      ///     ../signed_distance.h)
      /// @param[out] V  #V by 3 list of input mesh vertex positions
      /// @param[out] F  #F by 3 list of input triangle indices
      /// @return true if complex_to_mesh is successful
      ///  
      IGL_INLINE bool signed_distance_isosurface(
        const Eigen::MatrixXd & IV,
        const Eigen::MatrixXi & IF,
        const double level,
        const double angle_bound,
        const double radius_bound,
        const double distance_bound,
        const SignedDistanceType sign_type,
        Eigen::MatrixXd & V,
        Eigen::MatrixXi & F);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "signed_distance_isosurface.cpp"
#endif

#endif

