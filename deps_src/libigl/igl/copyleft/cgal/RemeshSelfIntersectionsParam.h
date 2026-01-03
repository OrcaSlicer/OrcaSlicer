// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_REMESH_SELF_INTERSECTIONS_PARAM_H
#define IGL_COPYLEFT_CGAL_REMESH_SELF_INTERSECTIONS_PARAM_H

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Parameters for SelfIntersectMesh, remesh_self_intersections and
      /// remesh_intersections, and intersect_other
      ///
      struct RemeshSelfIntersectionsParam
      {
        /// avoid constructing intersections results when possible
        bool detect_only;
        /// return after detecting the first intersection (if first_only==true,
        /// then detect_only should also be true)
        bool first_only;
        /// whether to stitch all resulting constructed elements into a
        /// (non-manifold) mesh 
        bool stitch_all;
        /// whether to use slow and more precise rounding (see assign_scalar)
        bool slow_and_more_precise_rounding;
        /// cutoff parameter for box_self_intersection_d (based on some casual
        /// testing 1000 works better than 1,10,100,10000 etc. and better than
        /// the recommended √n)
        int cutoff;
        inline RemeshSelfIntersectionsParam(
          bool _detect_only=false, 
          bool _first_only=false,
          bool _stitch_all=false,
          bool _slow_and_more_precise_rounding=false,
          int _cutoff=1000
          ):
          detect_only(_detect_only),
          first_only(_first_only),
          stitch_all(_stitch_all),
          slow_and_more_precise_rounding(_slow_and_more_precise_rounding),
          cutoff(_cutoff)
        {};
      };
    }
  }
}

#endif
