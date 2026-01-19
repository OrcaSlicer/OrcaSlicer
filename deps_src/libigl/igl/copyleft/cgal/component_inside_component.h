// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_COMONENT_INSIDE_COMPONENT
#define IGL_COPYLEFT_CGAL_COMONENT_INSIDE_COMPONENT

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl {
  namespace copyleft
  {
    namespace cgal {

        // Determine if connected facet component (V1, F1, I1) is inside of
        // connected facet component (V2, F2, I2).
        //
        // Precondition:
        // Both components must represent closed, self-intersection free,
        // non-degenerated surfaces that are the boundary of 3D volumes. In
        // addition, (V1, F1, I1) must not intersect with (V2, F2, I2).
        //
        // Inputs:
        //   V1  #V1 by 3 list of vertex position of mesh 1
        //   F1  #F1 by 3 list of triangles indices into V1
        //   I1  #I1 list of indices into F1, indicate the facets of component
        //   V2  #V2 by 3 list of vertex position of mesh 2
        //   F2  #F2 by 3 list of triangles indices into V2
        //   I2  #I2 list of indices into F2, indicate the facets of component
        //
        // Outputs:
        //   return true iff (V1, F1, I1) is entirely inside of (V2, F2, I2).
        template<typename DerivedV, typename DerivedF, typename DerivedI>
            IGL_INLINE bool component_inside_component(
                    const Eigen::PlainObjectBase<DerivedV>& V1,
                    const Eigen::PlainObjectBase<DerivedF>& F1,
                    const Eigen::PlainObjectBase<DerivedI>& I1,
                    const Eigen::PlainObjectBase<DerivedV>& V2,
                    const Eigen::PlainObjectBase<DerivedF>& F2,
                    const Eigen::PlainObjectBase<DerivedI>& I2);

        // Determine if mesh (V1, F1) is inside of mesh (V2, F2).
        //
        // Precondition:
        // Both meshes must be closed, self-intersection free, non-degenerated
        // surfaces that are the boundary of 3D volumes.  They should not
        // intersect each other.
        //
        // Inputs:
        //   V1  #V1 by 3 list of vertex position of mesh 1
        //   F1  #F1 by 3 list of triangles indices into V1
        //   V2  #V2 by 3 list of vertex position of mesh 2
        //   F2  #F2 by 3 list of triangles indices into V2
        //
        // Outputs:
        //   return true iff (V1, F1) is entirely inside of (V2, F2).
        template<typename DerivedV, typename DerivedF>
            IGL_INLINE bool component_inside_component(
                    const Eigen::PlainObjectBase<DerivedV>& V1,
                    const Eigen::PlainObjectBase<DerivedF>& F1,
                    const Eigen::PlainObjectBase<DerivedV>& V2,
                    const Eigen::PlainObjectBase<DerivedF>& F2);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "component_inside_component.cpp"
#endif
#endif
