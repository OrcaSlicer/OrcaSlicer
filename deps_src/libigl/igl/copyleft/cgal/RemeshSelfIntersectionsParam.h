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
      // Optional Parameters
      //   DetectOnly  Only compute IF, leave VV and FF alone
      struct RemeshSelfIntersectionsParam
      {
        bool detect_only;
        bool first_only;
        bool stitch_all;
        inline RemeshSelfIntersectionsParam(
          bool _detect_only=false, 
          bool _first_only=false,
          bool _stitch_all=false):
          detect_only(_detect_only),
          first_only(_first_only),
          stitch_all(_stitch_all){};
      };
    }
  }
}

#endif
