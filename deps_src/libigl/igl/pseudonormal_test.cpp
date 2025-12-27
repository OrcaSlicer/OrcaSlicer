// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "pseudonormal_test.h"
#include "barycentric_coordinates.h"
#include "doublearea.h"
#include "project_to_line_segment.h"
#include <cassert>
template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedFN,
  typename DerivedVN,
  typename DerivedEN,
  typename DerivedEMAP,
  typename Derivedq,
  typename Derivedc,
  typename Scalar,
  typename Derivedn>
IGL_INLINE void igl::pseudonormal_test(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedFN> & FN,
  const Eigen::MatrixBase<DerivedVN> & VN,
  const Eigen::MatrixBase<DerivedEN> & EN,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<Derivedq> & q,
  const int f,
  Eigen::PlainObjectBase<Derivedc> & c,
  Scalar & s,
  Eigen::PlainObjectBase<Derivedn> & n)
{
  using namespace Eigen;
  const auto & qc = q-c;
  typedef Eigen::Matrix<Scalar,1,3> RowVector3S;
  RowVector3S b;
  // Using barycentric coorindates to determine whether close to a vertex/edge
  // seems prone to error when dealing with nearly degenerate triangles: Even
  // the barycenter (1/3,1/3,1/3) can be made arbitrarily close to an
  // edge/vertex
  //
  const RowVector3S A = V.row(F(f,0));
  const RowVector3S B = V.row(F(f,1));
  const RowVector3S C = V.row(F(f,2));

  const double area = [&A,&B,&C]()
  {
    Matrix<double,1,1> area;
    doublearea(A,B,C,area);
    return area(0);
  }();
  // These were chosen arbitrarily. In a floating point scenario, I'm not sure
  // the best way to determine if c is on a vertex/edge or in the middle of the
  // face: specifically, I'm worrying about degenerate triangles where
  // barycentric coordinates are error-prone.
  const double MIN_DOUBLE_AREA = 1e-4;
  const double epsilon = 1e-12;
  if(area>MIN_DOUBLE_AREA)
  {
    barycentric_coordinates( c,A,B,C,b);
    // Determine which normal to use
    const int type = (b.array()<=epsilon).template cast<int>().sum();
    switch(type)
    {
      case 2:
        // Find vertex
        for(int x = 0;x<3;x++)
        {
          if(b(x)>epsilon)
          {
            n = VN.row(F(f,x));
            break;
          }
        }
        break;
      case 1:
        // Find edge
        for(int x = 0;x<3;x++)
        {
          if(b(x)<=epsilon)
          {
            n = EN.row(EMAP(F.rows()*x+f));
            break;
          }
        }
        break;
      default:
        assert(false && "all barycentric coords zero.");
      case 0:
        n = FN.row(f);
        break;
    }
  }else
  {
    // Check each vertex
    bool found = false;
    for(int v = 0;v<3 && !found;v++)
    {
      if( (c-V.row(F(f,v))).norm() < epsilon)
      {
        found = true;
        n = VN.row(F(f,v));
      }
    }
    // Check each edge
    for(int e = 0;e<3 && !found;e++)
    {
      const RowVector3S s = V.row(F(f,(e+1)%3));
      const RowVector3S d = V.row(F(f,(e+2)%3));
      Matrix<double,1,1> sqr_d_j_x(1,1);
      Matrix<double,1,1> t(1,1);
      project_to_line_segment(c,s,d,t,sqr_d_j_x);
      if(sqrt(sqr_d_j_x(0)) < epsilon)
      {
        n = EN.row(EMAP(F.rows()*e+f));
        found = true;
      }
    }
    // Finally just use face
    if(!found)
    {
      n = FN.row(f);
    }
  }
  s = (qc.dot(n) >= 0 ? 1. : -1.);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedEN,
  typename DerivedVN,
  typename Derivedq,
  typename Derivedc,
  typename Scalar,
  typename Derivedn>
IGL_INLINE void igl::pseudonormal_test(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & E,
  const Eigen::MatrixBase<DerivedEN> & EN,
  const Eigen::MatrixBase<DerivedVN> & VN,
  const Eigen::MatrixBase<Derivedq> & q,
  const int e,
  Eigen::PlainObjectBase<Derivedc> & c,
  Scalar & s,
  Eigen::PlainObjectBase<Derivedn> & n)
{
  using namespace Eigen;
  const auto & qc = q-c;
  const double len = (V.row(E(e,1))-V.row(E(e,0))).norm();
  // barycentric coordinates
  // this .head() nonsense is for "ridiculus" templates instantiations that AABB
  // needs to compile
  Eigen::Matrix<Scalar,1,2>
    b((c-V.row(E(e,1))).norm()/len,(c-V.row(E(e,0))).norm()/len);
    //b((c-V.row(E(e,1)).head(c.size())).norm()/len,(c-V.row(E(e,0)).head(c.size())).norm()/len);
  // Determine which normal to use
  const double epsilon = 1e-12;
  const int type = (b.array()<=epsilon).template cast<int>().sum();
  switch(type)
  {
    case 1:
      // Find vertex
      for(int x = 0;x<2;x++)
      {
        if(b(x)>epsilon)
        {
          n = VN.row(E(e,x)).head(2);
          break;
        }
      }
      break;
    default:
      assert(false && "all barycentric coords zero.");
    case 0:
      n = EN.row(e).head(2);
      break;
  }
  s = (qc.dot(n) >= 0 ? 1. : -1.);
}

// This is a bullshit template because AABB annoyingly needs templates for bad
// combinations of 3D V with DIM=2 AABB
// 
// _Define_ as a no-op rather than monkeying around with the proper code above
namespace igl
{
  template <> IGL_INLINE void pseudonormal_test(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&){assert(false);};
  template <> IGL_INLINE void pseudonormal_test(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&){assert(false);};
  template <> IGL_INLINE void pseudonormal_test(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&){assert(false);};
  template <> IGL_INLINE void pseudonormal_test(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&){assert(false);};
  template <> IGL_INLINE void pseudonormal_test(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&) {assert(false);};
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, float, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
// generated by autoexplicit.sh
// generated by autoexplicit.sh
// generated by autoexplicit.sh
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, -1, 1, 1, -1>, float, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, 1, 2, 1, 1, 2>, Eigen::Matrix<float, 1, -1, 1, 1, -1>, float, Eigen::Matrix<float, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&);
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, 1, 2, 1, 1, 2>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, -1, 1, 1, -1>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, 1, -1, 1, 1, -1>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
// generated by autoexplicit.sh
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
//template void igl::pseudonormal_test<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, 1, 2, 1, 1, 2>, Eigen::Matrix<float, 1, 2, 1, 1, 2>, float, Eigen::Matrix<float, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&);
template void igl::pseudonormal_test<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, float, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&, float&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::pseudonormal_test<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Matrix<double, 1, 2, 1, 1, 2>, double, Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&);
#endif
