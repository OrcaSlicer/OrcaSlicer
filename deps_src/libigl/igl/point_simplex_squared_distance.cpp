// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "point_simplex_squared_distance.h"
#include "project_to_line_segment.h"
#include "barycentric_coordinates.h"
#include <Eigen/Geometry>
#include <limits>
#include <cassert>

template <
  int DIM,
  typename Derivedp,
  typename DerivedV,
  typename DerivedEle,
  typename Derivedsqr_d,
  typename Derivedc,
  typename Derivedb>
IGL_INLINE void igl::point_simplex_squared_distance(
  const Eigen::MatrixBase<Derivedp> & p,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const typename DerivedEle::Index primitive,
  Derivedsqr_d & sqr_d,
  Eigen::PlainObjectBase<Derivedc> & c,
  Eigen::PlainObjectBase<Derivedb> & bary)
{
  static_assert(DerivedV::ColsAtCompileTime == DIM || DerivedV::ColsAtCompileTime == Eigen::Dynamic,"DerivedV::ColsAtCompileTime == DIM || DerivedV::ColsAtCompileTime == Eigen::Dynamic");

  typedef typename Derivedp::Scalar Scalar;
  typedef typename Eigen::Matrix<Scalar,1,DIM> Vector;
  typedef Vector Point;
  //typedef Derivedb BaryPoint;
  typedef Eigen::Matrix<typename Derivedb::Scalar,1,3> BaryPoint;

  const auto & Dot = [](const Point & a, const Point & b)->Scalar
  {
    return a.dot(b);
  };
  // Real-time collision detection, Ericson, Chapter 5
  const auto & ClosestBaryPtPointTriangle =
    [&Dot](Point p, Point a, Point b, Point c, BaryPoint& bary_out )->Point
  {
    // Check if P in vertex region outside A
    Vector ab = b - a;
    Vector ac = c - a;
    Vector ap = p - a;
    Scalar d1 = Dot(ab, ap);
    Scalar d2 = Dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
      // barycentric coordinates (1,0,0)
      bary_out << 1, 0, 0;
      return a;
    }
    // Check if P in vertex region outside B
    Vector bp = p - b;
    Scalar d3 = Dot(ab, bp);
    Scalar d4 = Dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
      // barycentric coordinates (0,1,0)
      bary_out << 0, 1, 0;
      return b;
    }
    // Check if P in edge region of AB, if so return projection of P onto AB
    Scalar vc = d1*d4 - d3*d2;
    if( a != b)
    {
      if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        Scalar v = d1 / (d1 - d3);
        // barycentric coordinates (1-v,v,0)
        bary_out << 1-v, v, 0;
        return a + v * ab;
      }
    }
    // Check if P in vertex region outside C
    Vector cp = p - c;
    Scalar d5 = Dot(ab, cp);
    Scalar d6 = Dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
      // barycentric coordinates (0,0,1)
      bary_out << 0, 0, 1;
      return c;
    }
    // Check if P in edge region of AC, if so return projection of P onto AC
    Scalar vb = d5*d2 - d1*d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
      Scalar w = d2 / (d2 - d6);
      // barycentric coordinates (1-w,0,w)
      bary_out << 1-w, 0, w;
      return a + w * ac;
    }
    // Check if P in edge region of BC, if so return projection of P onto BC
    Scalar va = d3*d6 - d5*d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
      Scalar w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      // barycentric coordinates (0,1-w,w)
      bary_out << 0, 1-w, w;
      return b + w * (c - b);
    }
    // P inside face region. Compute Q through its barycentric coordinates (u,v,w)
    Scalar denom = 1.0 / (va + vb + vc);
    Scalar v = vb * denom;
    Scalar w = vc * denom;
    bary_out << 1.0-v-w, v, w;
    return a + ab * v + ac * w; // = u*a + v*b + w*c, u = va * denom = 1.0-v-w
  };

  assert(p.size() == DIM);
  assert(V.cols() == DIM);
  assert(Ele.cols() <= DIM+1);
  assert(Ele.cols() <= 3 && "Only simplices up to triangles are considered");

  assert((Derivedb::RowsAtCompileTime == 1 || Derivedb::ColsAtCompileTime == 1) && "bary must be Eigen Vector or Eigen RowVector");
  assert(
    ((Derivedb::RowsAtCompileTime == -1 || Derivedb::ColsAtCompileTime == -1) ||
      (Derivedb::RowsAtCompileTime == Ele.cols() || Derivedb::ColsAtCompileTime == Ele.cols())
    ) && "bary must be Dynamic or size of Ele.cols()");

  BaryPoint tmp_bary;
  c = (const Derivedc &)ClosestBaryPtPointTriangle(
    p,
    V.row(Ele(primitive,0)),
    // modulo is a HACK to handle points, segments and triangles. Because of
    // this, we need 3d buffer for bary
    V.row(Ele(primitive,1%Ele.cols())),
    V.row(Ele(primitive,2%Ele.cols())),
    tmp_bary);
  bary.resize( Derivedb::RowsAtCompileTime == 1 ? 1 : Ele.cols(), Derivedb::ColsAtCompileTime == 1 ? 1 : Ele.cols());
  bary.head(Ele.cols()) = tmp_bary.head(Ele.cols());
  sqr_d = (p-c).squaredNorm();
}

template <
  int DIM,
  typename Derivedp,
  typename DerivedV,
  typename DerivedEle,
  typename Derivedsqr_d,
  typename Derivedc>
IGL_INLINE void igl::point_simplex_squared_distance(
  const Eigen::MatrixBase<Derivedp> & p,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const typename DerivedEle::Index primitive,
  Derivedsqr_d & sqr_d,
  Eigen::PlainObjectBase<Derivedc> & c)
{
  // DerivedEle::ColsAtCompileTime will fallback to Eigen::Dynamic if unknown.
  Eigen::Matrix<typename Derivedc::Scalar,1,DerivedEle::ColsAtCompileTime> b;
  point_simplex_squared_distance<DIM>( p, V, Ele, primitive, sqr_d, c, b );
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
// generated by autoexplicit.sh
template void igl::point_simplex_squared_distance<2, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::Matrix<int, -1, 2, 0, -1, 2>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<int, -1, 3, 1, -1, 3>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, float, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, float, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<int, -1, 3, 1, -1, 3>::Index, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, float, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<2, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 1, 1, 3> >&);
template void igl::point_simplex_squared_distance<2, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
template void igl::point_simplex_squared_distance<2, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 1, 1, 1, 2> >&);
template void igl::point_simplex_squared_distance<2, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<int, -1, 1, 0, -1, 1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
template void igl::point_simplex_squared_distance<3, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<int, -1, 1, 0, -1, 1>::Index, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
#endif
