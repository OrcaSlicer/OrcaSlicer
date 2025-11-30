// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_PEEL_OUTER_HULL_LAYERS_H
#define IGL_COPYLEFT_CGAL_PEEL_OUTER_HULL_LAYERS_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Computes necessary generic information for boolean operations by
      /// successively "peeling" off the "outer hull" of a mesh (V,F) resulting from
      /// "resolving" all (self-)intersections.
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[out] I  #F list of which peel Iation a facet belongs 
      /// @param[out] flip  #F list of whether a facet's orientation was flipped when facet
      ///     "peeled" into its associated outer hull layer.
      /// @return number of peels
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedI,
        typename Derivedflip>
      IGL_INLINE int peel_outer_hull_layers(
        const Eigen::MatrixBase<DerivedV > & V,
        const Eigen::MatrixBase<DerivedF > & F,
        Eigen::PlainObjectBase<DerivedI > & I,
        Eigen::PlainObjectBase<Derivedflip > & flip);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "peel_outer_hull_layers.cpp"
#endif
#endif
