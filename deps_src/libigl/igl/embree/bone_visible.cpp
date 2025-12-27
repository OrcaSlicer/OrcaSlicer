// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "bone_visible.h"
#include "../project_to_line.h"
#include "../EPS.h"
#include "../Hit.h"
#include "../Timer.h"
#include <iostream>

template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedSD,
  typename Derivedflag>
IGL_INLINE void igl::embree::bone_visible(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  const Eigen::PlainObjectBase<DerivedSD> & s,
  const Eigen::PlainObjectBase<DerivedSD> & d,
  Eigen::PlainObjectBase<Derivedflag>  & flag)
{
  // "double sided lighting"
  Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,Eigen::Dynamic> FF;
  FF.resize(F.rows()*2,F.cols());
  FF << F, F.rowwise().reverse();
  // Initialize intersector
  EmbreeIntersector ei;
  ei.init(V.template cast<float>(),FF.template cast<int>());
  return bone_visible(V,F,ei,s,d,flag);
}

template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedSD,
  typename Derivedflag>
IGL_INLINE void igl::embree::bone_visible(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  const EmbreeIntersector & ei,
  const Eigen::PlainObjectBase<DerivedSD> & s,
  const Eigen::PlainObjectBase<DerivedSD> & d,
  Eigen::PlainObjectBase<Derivedflag>  & flag)
{
  using namespace std;
  using namespace Eigen;
  flag.resize(V.rows());
  const double sd_norm = (s-d).norm();
  // Embree seems to be parallel when constructing but not when tracing rays
#pragma omp parallel for
  // loop over mesh vertices
  for(int v = 0;v<V.rows();v++)
  {
    const Vector3d Vv = V.row(v);
    // Project vertex v onto line segment sd
    //embree.intersectSegment
    double t,sqrd;
    Vector3d projv;
    // degenerate bone, just snap to s
    if(sd_norm < DOUBLE_EPS)
    {
      t = 0;
      sqrd = (Vv-s).array().pow(2).sum();
      projv = s;
    }else
    {
      // project onto (infinite) line
      project_to_line(
        Vv(0),Vv(1),Vv(2),s(0),s(1),s(2),d(0),d(1),d(2),
        projv(0),projv(1),projv(2),t,sqrd);
      // handle projections past endpoints
      if(t<0)
      {
        t = 0;
        sqrd = (Vv-s).array().pow(2).sum();
        projv = s;
      } else if(t>1)
      {
        t = 1;
        sqrd = (Vv-d).array().pow(2).sum();
        projv = d;
      }
    }
    igl::Hit hit;
    // perhaps 1.0 should be 1.0-epsilon, or actually since we checking the
    // incident face, perhaps 1.0 should be 1.0+eps
    const Vector3d dir = (Vv-projv)*1.0;
    if(ei.intersectSegment(
       projv.template cast<float>(),
       dir.template cast<float>(), 
       hit))
    {
      // mod for double sided lighting
      const int fi = hit.id % F.rows();

      //if(v == 1228-1)
      //{
      //  Vector3d bc,P;
      //  bc << 1 - hit.u - hit.v, hit.u, hit.v; // barycentric
      //  P = V.row(F(fi,0))*bc(0) + 
      //      V.row(F(fi,1))*bc(1) + 
      //      V.row(F(fi,2))*bc(2);
      //  cout<<(fi+1)<<endl;
      //  cout<<bc.transpose()<<endl;
      //  cout<<P.transpose()<<endl;
      //  cout<<hit.t<<endl;
      //  cout<<(projv + dir*hit.t).transpose()<<endl;
      //  cout<<Vv.transpose()<<endl;
      //}

      // Assume hit is valid, so not visible
      flag(v) = false;
      // loop around corners of triangle
      for(int c = 0;c<F.cols();c++)
      {
        if(F(fi,c) == v)
        {
          // hit self, so no hits before, so vertex v is visible
          flag(v) = true;
          break;
        }
      }
      // Hit is actually past v
      if(!flag(v) && (hit.t*hit.t*dir.squaredNorm())>sqrd)
      {
        flag(v) = true;
      }
    }else
    {
      // no hit so vectex v is visible
      flag(v) = true;
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::embree::bone_visible<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<bool, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >&);
template void igl::embree::bone_visible<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<bool, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >&);
#endif
