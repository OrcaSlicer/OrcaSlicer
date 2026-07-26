// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_TRIM_WITH_SOLID_H
#define IGL_COPYLEFT_CGAL_TRIM_WITH_SOLID_H

#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      enum TrimWithSolidMethod
      {
        /// Resolve intersections only between A and B and then check whether
        /// every face in A is inside/outside of B. (Included for debugging).
        CHECK_EACH_FACE = 1,
        /// Resolve intersections only between A and B, then separate into
        /// patches of connected faces --- where connected means sharing an edge
        /// and _not_ the same original face in A --- then label each patch as
        /// inside or outside. This should always be strictly equivalent in
        /// output and strictly fewer FLOPS than CHECK_EACH_FACE. There will be
        /// many tiny patches along the intersection of A and B. 
        CHECK_EACH_PATCH = 2,
        /// Merge A and B into the same mesh and resolve all self-itnersections.
        /// Then "undo" remeshing on faces of A not involved in intersections
        /// with B (i.e., self-intersections in A). Then seperate into patches
        /// based on connected faces --- where connected means sharing a
        /// _manifold edge_ --- then label each aptch as inside or outside.
        /// Results in fewer patches than CHECK_EACH_PATCH but finding, meshing and
        /// mesh-undoing self-intersections in A can be costly. This
        /// could result in different output from CHECK_EACH_PATCH because of
        /// shared remeshing with (i.e., faces in A that both intersect B and
        /// other faces in A). If A has no self-intersections then I claim the
        /// outputs should be the same.
        RESOLVE_BOTH_AND_RESTORE_THEN_CHECK_EACH_PATCH = 3,
      };
      /// Given an arbitrary mesh (VA,FA) and the boundary mesh
      /// (VB,FB) of a solid (as defined in [Zhou et al. 2016]), Resolve intersections
      /// between A and B subdividing faces of A so that intersections with B exists
      /// only along edges and vertices (and coplanar faces). Then determine whether
      /// each of these faces is inside or outside of B. This can be used to extract
      /// the part of A inside or outside of B.
      ///
      /// @param[in] VA  #VA by 3 list of mesh vertex positions of A
      /// @param[in] FA  #FA by 3 list of mesh triangle indices into VA
      /// @param[in] VB  #VB by 3 list of mesh vertex positions of B
      /// @param[in] FB  #FB by 3 list of mesh triangle indices into VB
      /// @param[out] V  #V by 3 list of mesh vertex positions of output
      /// @param[out] F  #F by 3 list of mesh triangle indices into V
      /// @param[out] D  #F list of bools whether face is inside B
      /// @param[out] J  #F list of indices into FA revealing birth parent
      ///
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedV,
        typename DerivedF,
        typename DerivedD,
        typename DerivedJ>
      IGL_INLINE void trim_with_solid(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::MatrixBase<DerivedVB> & VB,
        const Eigen::MatrixBase<DerivedFB> & FB,
        Eigen::PlainObjectBase<DerivedV> & Vd,
        Eigen::PlainObjectBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedD> & D,
        Eigen::PlainObjectBase<DerivedJ> & J);
      /// \overload
      ///
      /// @param[in] method  which method for extracting and determining the
      /// trim contents.
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedV,
        typename DerivedF,
        typename DerivedD,
        typename DerivedJ>
      IGL_INLINE void trim_with_solid(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::MatrixBase<DerivedVB> & VB,
        const Eigen::MatrixBase<DerivedFB> & FB,
        const TrimWithSolidMethod method,
        Eigen::PlainObjectBase<DerivedV> & Vd,
        Eigen::PlainObjectBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedD> & D,
        Eigen::PlainObjectBase<DerivedJ> & J);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "trim_with_solid.cpp"
#endif
#endif
