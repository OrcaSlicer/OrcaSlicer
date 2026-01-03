// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_TETGEN_CDT_H
#define IGL_COPYLEFT_TETGEN_CDT_H
#include "../../igl_inline.h"

#include <Eigen/Core>
#include <string>
#ifndef TETLIBRARY
#  define TETLIBRARY 
#endif
#include <tetgen.h> // Defined REAL

namespace igl
{
  namespace copyleft
  {
    namespace tetgen
    {
      /// Parameters for controling the CDT
      struct CDTParam
      {
        /// Tetgen can compute mesh of convex hull of input (i.e. "c") but often
        /// chokes. One workaround is to force it to mesh the entire bounding box.
        /// {false}
        bool use_bounding_box = false;
        /// Scale the bounding box a bit so that vertices near it do not give tetgen
        /// problems. {1.01}
        double bounding_box_scale = 1.01;
        /// Flags to tetgen. Do not include the "c" flag here! {"Y"}
        std::string flags = "Y";
      };
      /// Create a constrained delaunay tessellation containing convex hull of the
      /// given **non-selfintersecting** mesh.
      ///
      /// @param[in] V  #V by 3 list of input mesh vertices
      /// @param[in] F  #F by 3 list of input mesh facets
      /// @param[in] param  see above
      /// @param[out] TV  #TV by 3 list of output mesh vertices (V come first)
      /// @param[out] TT  #TT by 3 list of tetrahedra indices into TV.
      /// @param[out] TF  #TF by 3 list of facets from F potentially subdivided.
      /// 
      template <
        typename DerivedV, 
        typename DerivedF, 
        typename DerivedTV, 
        typename DerivedTT, 
        typename DerivedTF>
      IGL_INLINE bool cdt(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const CDTParam & param,
        Eigen::PlainObjectBase<DerivedTV>& TV,
        Eigen::PlainObjectBase<DerivedTT>& TT,
        Eigen::PlainObjectBase<DerivedTF>& TF);
    }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "cdt.cpp"
#endif

#endif


