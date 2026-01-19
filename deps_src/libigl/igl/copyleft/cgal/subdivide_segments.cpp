// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "subdivide_segments.h"
#include "row_to_point.h"
#include "assign_scalar.h"
#include "../../unique.h"
#include "../../list_to_matrix.h"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Segment_2.h>
#include <CGAL/Point_2.h>
#include <algorithm>
#include <vector>

template <
  typename DerivedV, 
  typename DerivedE,
  typename Kernel, 
  typename DerivedVI, 
  typename DerivedEI,
  typename DerivedJ,
  typename DerivedIM>
IGL_INLINE void igl::copyleft::cgal::subdivide_segments(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedE> & E,
  const std::vector<std::vector<CGAL::Point_2<Kernel> > > & _steiner,
  Eigen::PlainObjectBase<DerivedVI> & VI,
  Eigen::PlainObjectBase<DerivedEI> & EI,
  Eigen::PlainObjectBase<DerivedJ> & J,
  Eigen::PlainObjectBase<DerivedIM> & IM)
{
  using namespace Eigen;
  using namespace igl;
  using namespace std;

  // Exact scalar type
  typedef Kernel K;
  typedef typename Kernel::FT EScalar;
  typedef CGAL::Segment_2<Kernel> Segment_2;
  typedef CGAL::Point_2<Kernel> Point_2;
  typedef Matrix<EScalar,Dynamic,Dynamic>  MatrixXE;

  // non-const copy
  std::vector<std::vector<CGAL::Point_2<Kernel> > > steiner = _steiner;

  // Convert vertex positions to exact kernel
  MatrixXE VE(V.rows(),V.cols());
  for(int i = 0;i<V.rows();i++)
  {
    for(int j = 0;j<V.cols();j++)
    {
      VE(i,j) = V(i,j);
    }
  }

  // number of original vertices
  const int n = V.rows();
  // number of original segments
  const int m = E.rows();
  // now steiner contains lists of points (unsorted) for each edge. Sort them
  // and count total number of vertices and edges
  int ni = 0;
  int mi = 0;
  // new steiner points
  std::vector<Point_2> S;
  std::vector<std::vector<typename DerivedE::Scalar> > vEI;
  std::vector<typename DerivedJ::Scalar> vJ;
  for(int i = 0;i<m;i++)
  {
    {
      const Point_2 s = row_to_point<K>(VE,E(i,0));
      std::sort(
        steiner[i].begin(),
        steiner[i].end(),
        [&s](const Point_2 & A, const Point_2 & B)->bool
        {
          return (A-s).squared_length() < (B-s).squared_length();
        });
    }
    // remove duplicates
    steiner[i].erase(
      std::unique(steiner[i].begin(), steiner[i].end()), 
      steiner[i].end());
    {
      int s = E(i,0);
      // legs to each steiner in order
      for(int j = 1;j<steiner[i].size()-1;j++)
      {
        int d = n+S.size();
        S.push_back(steiner[i][j]);
        vEI.push_back({s,d});
        vJ.push_back(i);
        s = d;
      }
      // don't forget last (which might only) leg
      vEI.push_back({s,E(i,1)});
      vJ.push_back(i);
    }
  }
  // potentially unnecessary copying ...
  VI.resize(n+S.size(),2);
  for(int i = 0;i<V.rows();i++)
  {
    for(int j = 0;j<V.cols();j++)
    {
      assign_scalar(V(i,j),VI(i,j));
    }
  }
  for(int i = 0;i<S.size();i++)
  {
    assign_scalar(S[i].x(),VI(n+i,0));
    assign_scalar(S[i].y(),VI(n+i,1));
  }
  list_to_matrix(vEI,EI);
  list_to_matrix(vJ,J);
  {
    // Find unique mapping
    std::vector<Point_2> vVES,_1;
    for(int i = 0;i<n;i++)
    {
      vVES.push_back(row_to_point<K>(VE,i));
    }
    vVES.insert(vVES.end(),S.begin(),S.end());
    std::vector<size_t> vA,vIM;
    igl::unique(vVES,_1,vA,vIM);
    // Push indices back into vVES
    for_each(vIM.data(),vIM.data()+vIM.size(),[&vA](size_t & i){i=vA[i];});
    list_to_matrix(vIM,IM);
  }
}
