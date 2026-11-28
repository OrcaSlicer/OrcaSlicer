// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_OUTER_HULL_LEGACY_H
#define IGL_COPYLEFT_CGAL_OUTER_HULL_LEGACY_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Compute the "outer hull" of a potentially non-manifold mesh (V,F) whose
      /// intersections have been "resolved" (e.g. using `cork` or
      /// `igl::copyleft::cgal::selfintersect`). The outer hull is defined to be all facets
      /// (regardless of orientation) for which there exists some path from infinity
      /// to the face without intersecting any other facets. For solids, this is the
      /// surface of the solid. In general this includes any thin "wings" or
      /// "flaps".  This implementation largely follows Section 3.6 of "Direct
      /// repair of self-intersecting meshes" [Attene 2014].
      ///
      /// \note This doesn't require the input mesh to be piecewise constant
      /// winding number, but won't handle multiple non-nested connected
      /// components.
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[out] G  #G by 3 list of output triangle indices into V
      /// @param[out] J  #G list of indices into F
      /// @param[out] flip  #F list of whether facet was added to G **and** flipped orientation
      ///     (false for faces not added to G)
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedG,
        typename DerivedJ,
        typename Derivedflip>
      IGL_INLINE void outer_hull_legacy(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedG> & G,
        Eigen::PlainObjectBase<DerivedJ> & J,
        Eigen::PlainObjectBase<Derivedflip> & flip);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "outer_hull_legacy.cpp"
#endif
#endif

