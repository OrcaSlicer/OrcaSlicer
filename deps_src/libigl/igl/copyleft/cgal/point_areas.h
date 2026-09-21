// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Gavin Barill <gavinpcb@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/

#ifndef IGL_POINT_AREAS_H
#define IGL_POINT_AREAS_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Given a 3D set of points P, each with a list of k-nearest-neighbours,
      /// estimate the geodesic voronoi area associated with each point.
      ///
      /// The k nearest neighbours may be known from running igl::knn_octree on
      /// the output data from igl::octree. We reccomend using a k value
      /// between 15 and 20 inclusive for accurate area estimation.
      ///
      /// N is used filter the neighbours, to ensure area estimation only occurs
      /// using neighbors that are on the same side of the surface (ie for thin
      /// sheets), as well as to solve the orientation ambiguity of the tangent
      /// plane normal.
      ///
      /// \note This function *should* be implemented by pre-filtering I, rather
      /// than filtering in this function using N. In this case, the function
      /// would only take P and I as input.
      ///
      /// @param[in] P  #P by 3 list of point locations
      /// @param[in] I  #P by k list of k-nearest-neighbor indices into P
      /// @param[in] N  #P by 3 list of point normals
      /// @param[out] A  #P list of estimated areas
      //  @param[out] T  #P by 3 list of tangent plane normals for each point
      ///
      /// \see igl::knn
      template <typename DerivedP, typename DerivedI, typename DerivedN,
      typename DerivedA, typename DerivedT>
      IGL_INLINE void point_areas(
        const Eigen::MatrixBase<DerivedP>& P,
        const Eigen::MatrixBase<DerivedI>& I,
        const Eigen::MatrixBase<DerivedN>& N,
        Eigen::PlainObjectBase<DerivedA> & A,
        Eigen::PlainObjectBase<DerivedT> & T);
      /// \overload
      template <typename DerivedP, typename DerivedI, typename DerivedN,
               typename DerivedA>
      IGL_INLINE void point_areas(
        const Eigen::MatrixBase<DerivedP>& P,
        const Eigen::MatrixBase<DerivedI>& I,
        const Eigen::MatrixBase<DerivedN>& N,
        Eigen::PlainObjectBase<DerivedA> & A);
        
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "point_areas.cpp"
#endif

#endif

