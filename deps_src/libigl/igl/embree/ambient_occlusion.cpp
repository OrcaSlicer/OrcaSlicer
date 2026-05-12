// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "ambient_occlusion.h"
#include "../ambient_occlusion.h"
#include "EmbreeIntersector.h"
#include "../Hit.h"

template <
  typename DerivedP,
  typename DerivedN,
  typename DerivedS >
IGL_INLINE void igl::embree::ambient_occlusion(
  const igl::embree::EmbreeIntersector & ei,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  const auto & shoot_ray = [&ei](
    const Eigen::Vector3f& s,
    const Eigen::Vector3f& dir)->bool
  {
    igl::Hit<float> hit;
    const float tnear = 1e-4f;
    return ei.intersectRay(s,dir,hit,tnear);
  };
  const auto Pf = P.template cast<float>().eval();
  const auto Nf = N.template cast<float>().eval();
  Eigen::Matrix<float,Eigen::Dynamic,1> Sf;
  igl::ambient_occlusion(shoot_ray,Pf,Nf,num_samples,Sf);
  S = Sf.template cast<typename DerivedS::Scalar>();
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedN,
  typename DerivedS >
IGL_INLINE void igl::embree::ambient_occlusion(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  using namespace Eigen;
  EmbreeIntersector ei;
  ei.init(V.template cast<float>(),F.template cast<int>());
  ambient_occlusion(ei,P,N,num_samples,S);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::embree::ambient_occlusion<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(igl::embree::EmbreeIntersector const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::embree::ambient_occlusion<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(igl::embree::EmbreeIntersector const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::embree::ambient_occlusion<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::embree::ambient_occlusion<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::embree::ambient_occlusion<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
