// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Alec Jacobson
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "delaunay_triangulation.h"
#include "../../delaunay_triangulation.h"
#include "orient2D.h"
#include "incircle.h"

template<
  typename DerivedV,
  typename DerivedF>
IGL_INLINE void igl::copyleft::cgal::delaunay_triangulation(
    const Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F)
{
  typedef typename DerivedV::Scalar Scalar;
  igl::delaunay_triangulation(V, orient2D<Scalar>, incircle<Scalar>, F);
  // This function really exists to test our igl::delaunay_triangulation
  // 
  // It's currently much faster to call cgal's native Delaunay routine
  //
//#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
//#include <CGAL/Delaunay_triangulation_2.h>
//#include <CGAL/Triangulation_vertex_base_with_info_2.h>
//#include <vector>
//  const auto delaunay = 
//    [&](const Eigen::MatrixXd & V,Eigen::MatrixXi & F)
//  {
//    typedef CGAL::Exact_predicates_inexact_constructions_kernel            Kernel;
//    typedef CGAL::Triangulation_vertex_base_with_info_2<unsigned int, Kernel> Vb;
//    typedef CGAL::Triangulation_data_structure_2<Vb>                       Tds;
//    typedef CGAL::Delaunay_triangulation_2<Kernel, Tds>                    Delaunay;
//    typedef Kernel::Point_2                                                Point;
//    std::vector< std::pair<Point,unsigned> > points(V.rows());
//    for(int i = 0;i<V.rows();i++)
//    {
//      points[i] = std::make_pair(Point(V(i,0),V(i,1)),i);
//    }
//    Delaunay triangulation;
//    triangulation.insert(points.begin(),points.end());
//    F.resize(triangulation.number_of_faces(),3);
//    {
//      int j = 0;
//      for(Delaunay::Finite_faces_iterator fit = triangulation.finite_faces_begin();
//          fit != triangulation.finite_faces_end(); ++fit) 
//      {
//        Delaunay::Face_handle face = fit;
//        F(j,0) = face->vertex(0)->info();
//        F(j,1) = face->vertex(1)->info();
//        F(j,2) = face->vertex(2)->info();
//        j++;
//      }
//    }
//  };
}

