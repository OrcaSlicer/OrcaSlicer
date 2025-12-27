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
#include "tetgen.h" // Defined REAL

namespace igl
{
  namespace copyleft
  {
    namespace tetgen
    {
      // Mesh the interior of a surface mesh (V,F) using tetgen
      //
      // Inputs:
      //   V  #V by 3 vertex position list
      //   F  #F list of polygon face indices into V (0-indexed)
      //   switches  string of tetgen options (See tetgen documentation) e.g.
      //     "pq1.414a0.01" tries to mesh the interior of a given surface with
      //       quality and area constraints
      //     "" will mesh the convex hull constrained to pass through V (ignores F)
      // Outputs:
      //   TV  #V by 3 vertex position list
      //   TT  #T by 4 list of tet face indices
      //   TF  #F by 3 list of triangle face indices
      // Returns status:
      //   0 success
      //   1 tetgen threw exception
      //   2 tetgen did not crash but could not create any tets (probably there are
      //     holes, duplicate faces etc.)
      //   -1 other error
      IGL_INLINE int tetrahedralize(
        const std::vector<std::vector<REAL > > & V, 
        const std::vector<std::vector<int> > & F, 
        const std::string switches,
        std::vector<std::vector<REAL > > & TV, 
        std::vector<std::vector<int > > & TT, 
        std::vector<std::vector<int> > & TF);
      
      // Wrapper with Eigen types
      // Templates:
      //   DerivedV  real-value: i.e. from MatrixXd
      //   DerivedF  integer-value: i.e. from MatrixXi
      template <
        typename DerivedV, 
        typename DerivedF, 
        typename DerivedTV, 
        typename DerivedTT, 
        typename DerivedTF>
      IGL_INLINE int tetrahedralize(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const std::string switches,
        Eigen::PlainObjectBase<DerivedTV>& TV,
        Eigen::PlainObjectBase<DerivedTT>& TT,
        Eigen::PlainObjectBase<DerivedTF>& TF);
      
			// Mesh the interior of a surface mesh (V,F) using tetgen
      //
      // Inputs:
      //   V  #V by 3 vertex position list
      //   F  #F list of polygon face indices into V (0-indexed)
			//   M  #V list of markers for vertices
      //   switches  string of tetgen options (See tetgen documentation) e.g.
      //     "pq1.414a0.01" tries to mesh the interior of a given surface with
      //       quality and area constraints
      //     "" will mesh the convex hull constrained to pass through V (ignores F)
      // Outputs:
      //   TV  #V by 3 vertex position list
      //   TT  #T by 4 list of tet face indices
      //   TF  #F by 3 list of triangle face indices
			//   TM  #V list of markers for vertices
      // Returns status:
      //   0 success
      //   1 tetgen threw exception
      //   2 tetgen did not crash but could not create any tets (probably there are
      //     holes, duplicate faces etc.)
      //   -1 other error
      IGL_INLINE int tetrahedralize(
        const std::vector<std::vector<REAL > > & V, 
        const std::vector<std::vector<int> > & F, 
				const std::vector<int> & VM,
				const std::vector<int> & FM,
        const std::string switches,
        std::vector<std::vector<REAL > > & TV, 
        std::vector<std::vector<int > > & TT, 
        std::vector<std::vector<int> > & TF,
				std::vector<int> & TM);
      
      // Wrapper with Eigen types
      // Templates:
      //   DerivedV  real-value: i.e. from MatrixXd
      //   DerivedF  integer-value: i.e. from MatrixXi
      template <
        typename DerivedV, 
        typename DerivedF, 
				typename DerivedVM,
				typename DerivedFM,
        typename DerivedTV, 
        typename DerivedTT, 
        typename DerivedTF, 
        typename DerivedTM>
      IGL_INLINE int tetrahedralize(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedVM>& VM,
        const Eigen::PlainObjectBase<DerivedFM>& FM,
        const std::string switches,
        Eigen::PlainObjectBase<DerivedTV>& TV,
        Eigen::PlainObjectBase<DerivedTT>& TT,
        Eigen::PlainObjectBase<DerivedTF>& TF,
        Eigen::PlainObjectBase<DerivedTM>& TM);
    }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "tetrahedralize.cpp"
#endif

#endif

