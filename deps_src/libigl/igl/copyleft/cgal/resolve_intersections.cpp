// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "resolve_intersections.h"
#include "subdivide_segments.h"
#include "row_to_point.h"
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
  typename DerivedVI, 
  typename DerivedEI,
  typename DerivedJ,
  typename DerivedIM>
IGL_INLINE void igl::copyleft::cgal::resolve_intersections(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedVI> & VI,
  Eigen::PlainObjectBase<DerivedEI> & EI,
  Eigen::PlainObjectBase<DerivedJ> & J,
  Eigen::PlainObjectBase<DerivedIM> & IM)
{
  using namespace Eigen;
  using namespace igl;
  using namespace std;
  // Exact scalar type
  typedef CGAL::Epeck K;
  typedef K::FT EScalar;
  typedef CGAL::Segment_2<K> Segment_2;
  typedef CGAL::Point_2<K> Point_2;
  typedef Matrix<EScalar,Dynamic,Dynamic>  MatrixXE;

  // Convert vertex positions to exact kernel
  MatrixXE VE(V.rows(),V.cols());
  for(int i = 0;i<V.rows();i++)
  {
    for(int j = 0;j<V.cols();j++)
    {
      VE(i,j) = V(i,j);
    }
  }

  const int m = E.rows();
  // resolve all intersections: silly O(n²) implementation
  std::vector<std::vector<Point_2> > steiner(m);
  for(int i = 0;i<m;i++)
  {
    Segment_2 si(row_to_point<K>(VE,E(i,0)),row_to_point<K>(VE,E(i,1)));
    steiner[i].push_back(si.vertex(0));
    steiner[i].push_back(si.vertex(1));
    for(int j = i+1;j<m;j++)
    {
      Segment_2 sj(row_to_point<K>(VE,E(j,0)),row_to_point<K>(VE,E(j,1)));
      // do they intersect?
      if(CGAL::do_intersect(si,sj))
      {
        CGAL::Object result = CGAL::intersection(si,sj);
        if(const Point_2 * p = CGAL::object_cast<Point_2 >(&result))
        {
          steiner[i].push_back(*p);
          steiner[j].push_back(*p);
          // add intersection point
        }else if(const Segment_2 * s = CGAL::object_cast<Segment_2 >(&result))
        {
          // add both endpoints
          steiner[i].push_back(s->vertex(0));
          steiner[j].push_back(s->vertex(0));
          steiner[i].push_back(s->vertex(1));
          steiner[j].push_back(s->vertex(1));
        }else
        {
          assert(false && "Unknown intersection type");
        }
      }
    }
  }

  subdivide_segments(V,E,steiner,VI,EI,J,IM);
}
