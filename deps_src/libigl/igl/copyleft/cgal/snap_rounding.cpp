// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "snap_rounding.h"
#include "resolve_intersections.h"
#include "subdivide_segments.h"
#include "../../remove_unreferenced.h"
#include "../../unique.h"
#include <CGAL/Segment_2.h>
#include <CGAL/Point_2.h>
#include <CGAL/Vector_2.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <algorithm>

template <
  typename DerivedV, 
  typename DerivedE, 
  typename DerivedVI, 
  typename DerivedEI,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::snap_rounding(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedVI> & VI,
  Eigen::PlainObjectBase<DerivedEI> & EI,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  using namespace Eigen;
  using namespace igl;
  using namespace igl::copyleft::cgal;
  using namespace std;
  // Exact scalar type
  typedef CGAL::Epeck Kernel;
  typedef Kernel::FT EScalar;
  typedef CGAL::Segment_2<Kernel> Segment_2;
  typedef CGAL::Point_2<Kernel> Point_2;
  typedef CGAL::Vector_2<Kernel> Vector_2;
  typedef Matrix<EScalar,Dynamic,Dynamic>  MatrixXE;
  // Convert vertex positions to exact kernel

  MatrixXE VE;
  {
    VectorXi IM;
    resolve_intersections(V,E,VE,EI,J,IM);
    for_each(
      EI.data(),
      EI.data()+EI.size(),
      [&IM](typename DerivedEI::Scalar& i){i=IM(i);});
    VectorXi _;
    remove_unreferenced( MatrixXE(VE), DerivedEI(EI), VE,EI,_);
  }

  // find all hot pixels
  //// southwest and north east corners
  //const RowVector2i SW(
  //  round(VE.col(0).minCoeff()),
  //  round(VE.col(1).minCoeff()));
  //const RowVector2i NE(
  //  round(VE.col(0).maxCoeff()),
  //  round(VE.col(1).maxCoeff()));

  // https://github.com/CGAL/cgal/issues/548
  // Round an exact scalar to the nearest integer. A priori can't just round
  // double. Suppose e=0.5+ε but double(e)<0.5
  //
  // Inputs:
  //   e  exact number
  // Outputs:
  //   i  closest integer
  const auto & round = [](const EScalar & e)->int
  {
    const double d = CGAL::to_double(e);
    // get an integer that's near the closest int
    int i = std::round(d);
    EScalar di_sqr = CGAL::square((e-EScalar(i)));
    const auto & search = [&i,&di_sqr,&e](const int dir)
    {
      while(true)
      {
        const int j = i+dir;
        const EScalar dj_sqr = CGAL::square((e-EScalar(j)));
        if(dj_sqr < di_sqr)
        {
          i = j;
          di_sqr = dj_sqr;
        }else
        {
          break;
        }
      }
    };
    // Try to increase/decrease int
    search(1);
    search(-1);
    return i;
  };
  vector<Point_2> hot;
  for(int i = 0;i<VE.rows();i++)
  {
    hot.emplace_back(round(VE(i,0)),round(VE(i,1)));
  }
  {
    std::vector<size_t> _1,_2;
    igl::unique(vector<Point_2>(hot),hot,_1,_2);
  }

  // find all segments intersecting hot pixels
  //   split edge at closest point to hot pixel center
  vector<vector<Point_2>>  steiner(EI.rows());
  // initialize each segment with endpoints
  for(int i = 0;i<EI.rows();i++)
  {
    steiner[i].emplace_back(VE(EI(i,0),0),VE(EI(i,0),1));
    steiner[i].emplace_back(VE(EI(i,1),0),VE(EI(i,1),1));
  }
  // silly O(n²) implementation
  for(const Point_2 & h : hot)
  {
    // North, East, South, West
    Segment_2 wall[4] = 
    {
      {h+Vector_2(-0.5, 0.5),h+Vector_2( 0.5, 0.5)},
      {h+Vector_2( 0.5, 0.5),h+Vector_2( 0.5,-0.5)},
      {h+Vector_2( 0.5,-0.5),h+Vector_2(-0.5,-0.5)},
      {h+Vector_2(-0.5,-0.5),h+Vector_2(-0.5, 0.5)}
    };
    // consider all segments
    for(int i = 0;i<EI.rows();i++)
    {
      // endpoints
      const Point_2 s(VE(EI(i,0),0),VE(EI(i,0),1));
      const Point_2 d(VE(EI(i,1),0),VE(EI(i,1),1));
      // if either end-point is in h's pixel then ignore
      const Point_2 rs(round(s.x()),round(s.y()));
      const Point_2 rd(round(d.x()),round(d.y()));
      if(h == rs || h == rd)
      {
        continue;
      }
      // otherwise check for intersections with walls consider all walls
      const Segment_2 si(s,d);
      vector<Point_2> hits;
      for(int j = 0;j<4;j++)
      {
        const Segment_2 & sj = wall[j];
        if(CGAL::do_intersect(si,sj))
        {
          CGAL::Object result = CGAL::intersection(si,sj);
          if(const Point_2 * p = CGAL::object_cast<Point_2 >(&result))
          {
            hits.push_back(*p);
          }else if(const Segment_2 * s = CGAL::object_cast<Segment_2 >(&result))
          {
            // add both endpoints
            hits.push_back(s->vertex(0));
            hits.push_back(s->vertex(1));
          }
        }
      }
      if(hits.size() == 0)
      {
        continue;
      }
      // centroid of hits
      Vector_2 cen(0,0);
      for(const Point_2 & hit : hits)
      {
        cen = Vector_2(cen.x()+hit.x(), cen.y()+hit.y());
      }
      cen = Vector_2(cen.x()/EScalar(hits.size()),cen.y()/EScalar(hits.size()));
      const Point_2 rcen(round(cen.x()),round(cen.y()));
      // after all of that, don't add as a steiner unless it's going to round
      // to h
      if(rcen == h)
      {
        steiner[i].emplace_back(cen.x(),cen.y());
      }
    }
  }
  {
    DerivedJ prevJ = J;
    VectorXi IM;
    subdivide_segments(MatrixXE(VE),MatrixXi(EI),steiner,VE,EI,J,IM);
    for_each(J.data(),J.data()+J.size(),[&prevJ](typename DerivedJ::Scalar & j){j=prevJ(j);});
    for_each(
      EI.data(),
      EI.data()+EI.size(),
      [&IM](typename DerivedEI::Scalar& i){i=IM(i);});
    VectorXi _;
    remove_unreferenced( MatrixXE(VE), DerivedEI(EI), VE,EI,_);
  }


  VI.resizeLike(VE);
  for(int i = 0;i<VE.rows();i++)
  {
    for(int j = 0;j<VE.cols();j++)
    {
      VI(i,j) = round(CGAL::to_double(VE(i,j)));
    }
  }
}
