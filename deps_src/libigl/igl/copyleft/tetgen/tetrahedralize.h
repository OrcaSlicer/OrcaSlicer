// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_TETGEN_TETRAHEDRALIZE_H
#define IGL_COPYLEFT_TETGEN_TETRAHEDRALIZE_H
#include "../../igl_inline.h"

#include <vector>
#include <string>
#include <Eigen/Core>
#ifndef TETLIBRARY
#define TETLIBRARY 
#endif
#include <tetgen.h> // Defined REAL

namespace igl
{
  namespace copyleft
  {
    namespace tetgen
    {
      /// Mesh the interior of a surface mesh (V,F) using tetgen
      ///
      /// @param[in] V  #V by 3 vertex position list
      /// @param[in] F  #F list of polygon face indices into V (0-indexed)
      /// @param[in] H  #H by 3 list of seed points inside holes
      /// @param[in] VM  #VM list of vertex markers
      /// @param[in] FM  #FM list of face markers
      /// @param[in] R  #R by 5 list of region attributes            
      /// @param[in] switches  string of tetgen options (See tetgen documentation) e.g.
      ///     "pq1.414a0.01" tries to mesh the interior of a given surface with
      ///       quality and area constraints
      ///     "" will mesh the convex hull constrained to pass through V (ignores F)
      /// @param[out] TV  #TV by 3 vertex position list
      /// @param[out] TT  #TT by 4 list of tet face indices
      /// @param[out] TF  #TF by 3 list of triangle face indices ('f', else
      ///   `boundary_facets` is called on TT)
      /// @param[out] TR  #TT list of region ID for each tetrahedron      
      /// @param[out] TN  #TT by 4 list of indices neighbors for each tetrahedron ('n')
      /// @param[out] PT  #TV list of incident tetrahedron for a vertex ('m')
      /// @param[out] FT  #TF by 2 list of tetrahedrons sharing a triface ('nn')
      /// @param[out] num_regions Number of regions in output mesh
      /// @return status:
      ///   0 success
      ///   1 tetgen threw exception
      ///   2 tetgen did not crash but could not create any tets (probably there are
      ///     holes, duplicate faces etc.)
      ///   -1 other error
      ///
      /// \note The polygons F can contain polygons with different number of vertices.
      /// Trailing unused columns are filled with -1. For example, triangles and
      /// segments can be specified using a #F x 3 matrix: for segments the third 
      /// column contains -1.
      ///
      /// \note Tetgen mixes integer region ids in with other region data `attr
      /// = (int) in->regionlist[i + 3];`. So it's declared safe to use integer
      /// types for `TR` since this also assumes that there's a single tet
      /// attribute and that it's the region id.
      ///
      /// #### Example
      ///
      /// ```cpp
      /// Eigen::MatrixXd V;
      /// Eigen::MatrixXi F;
      /// …
      /// Eigen::VectorXi VM,FM;
      /// Eigen::MatrixXd H,R;
      /// Eigen::VectorXi TM,TR,PT;
      /// Eigen::MatrixXi FT,TN;
      /// int numRegions;
      /// tetrahedralize(V,F,H,VM,FM,R,switches,TV,TT,TF,TM,TR,TN,PT,FT,numRegions);
      /// ```
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedH,
        typename DerivedVM,
        typename DerivedFM,
        typename DerivedR,
        typename DerivedTV,
        typename DerivedTT,
        typename DerivedTF,
        typename DerivedTM,
        typename DerivedTR,
        typename DerivedTN,
        typename DerivedPT,
        typename DerivedFT>
      IGL_INLINE int tetrahedralize(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedH>& H,
        const Eigen::MatrixBase<DerivedVM>& VM,
        const Eigen::MatrixBase<DerivedFM>& FM,
        const Eigen::MatrixBase<DerivedR>& R,
        const std::string switches,
        Eigen::PlainObjectBase<DerivedTV>& TV,
        Eigen::PlainObjectBase<DerivedTT>& TT,
        Eigen::PlainObjectBase<DerivedTF>& TF,
        Eigen::PlainObjectBase<DerivedTM>& TM,
        Eigen::PlainObjectBase<DerivedTR>& TR, 
        Eigen::PlainObjectBase<DerivedTN>& TN, 
        Eigen::PlainObjectBase<DerivedPT>& PT, 
        Eigen::PlainObjectBase<DerivedFT>& FT, 
        int & num_regions);
      /// \overload
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedTV,
        typename DerivedTT,
        typename DerivedTF>
      IGL_INLINE int tetrahedralize(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const std::string switches,
        Eigen::PlainObjectBase<DerivedTV>& TV,
        Eigen::PlainObjectBase<DerivedTT>& TT,
        Eigen::PlainObjectBase<DerivedTF>& TF);
   }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "tetrahedralize.cpp"
#endif

#endif

