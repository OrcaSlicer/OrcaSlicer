// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "ambient_occlusion.h"
#include "random_dir.h"
#include "ray_mesh_intersect.h"
#include "EPS.h"
#include "Hit.h"
#include "parallel_for.h"
#include <functional>
#include <vector>
#include <algorithm>

template <
  typename DerivedP,
  typename DerivedN,
  typename DerivedS >
IGL_INLINE void igl::ambient_occlusion(
  const std::function<
    bool(
      const Eigen::Matrix<typename DerivedP::Scalar,3,1> &,
      const Eigen::Matrix<typename DerivedP::Scalar,3,1> &)
      > & shoot_ray,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  using namespace Eigen;
  const int n = P.rows();
  // Resize output
  S.resize(n,1);
  // Embree seems to be parallel when constructing but not when tracing rays
  typedef typename DerivedP::Scalar Scalar;
  typedef Eigen::Matrix<Scalar,3,1> Vector3N;

  const Matrix<Scalar,Eigen::Dynamic,3> D = random_dir_stratified(num_samples).cast<Scalar>();

  const auto & inner = [&P,&N,&num_samples,&D,&S,&shoot_ray](const int p)
  {
    const Vector3N origin = P.row(p);
    const Vector3N normal = N.row(p);
    int num_hits = 0;
    for(int s = 0;s<num_samples;s++)
    {
      Vector3N d = D.row(s);
      if(d.dot(normal) < 0)
      {
        // reverse ray
        d *= -1;
      }
      if(shoot_ray(origin,d))
      {
        num_hits++;
      }
    }
    S(p) = (double)num_hits/(double)num_samples;
  };
  parallel_for(n,inner,1000);
}

template <
  typename DerivedV,
  int DIM,
  typename DerivedF,
  typename DerivedP,
  typename DerivedN,
  typename DerivedS >
IGL_INLINE void igl::ambient_occlusion(
  const igl::AABB<DerivedV,DIM> & aabb,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  typedef typename DerivedV::Scalar Scalar;
  using Vector3S = Eigen::Matrix<Scalar,3,1>;
  const auto & shoot_ray = [&aabb,&V,&F](
    const Eigen::Matrix<Scalar,3,1> & _s,
    const Eigen::Matrix<Scalar,3,1> & dir)->bool
  {
    Vector3S s = _s+1e-4*dir;
    igl::Hit<Scalar> hit;
    return aabb.intersect_ray(
      V,
      F,
      s,
      dir,
      hit);
  };
  return ambient_occlusion(shoot_ray,P,N,num_samples,S);

}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedN,
  typename DerivedS >
IGL_INLINE void igl::ambient_occlusion(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  typedef typename DerivedV::Scalar Scalar;
  using Vector3S = Eigen::Matrix<Scalar,3,1>;
  if(F.rows() < 100)
  {
    // Super naive
    const auto & shoot_ray = [&V,&F](
      const Eigen::Matrix<Scalar,3,1> & _s,
      const Eigen::Matrix<Scalar,3,1> & dir)->bool
    {
      Vector3S s = _s+1e-4*dir;
      igl::Hit<Scalar> hit;
      return ray_mesh_intersect(s,dir,V,F,hit);
    };
    return ambient_occlusion(shoot_ray,P,N,num_samples,S);
  }
  AABB<DerivedV,3> aabb;
  aabb.init(V,F);
  return ambient_occlusion(aabb,V,F,P,N,num_samples,S);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::ambient_occlusion<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::ambient_occlusion<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(std::function<bool (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::ambient_occlusion<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(std::function<bool (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::ambient_occlusion<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(std::function<bool (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::ambient_occlusion<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::function<bool (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::ambient_occlusion<Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, -1, 1, 0, -1, 1>>(std::function<bool (Eigen::Matrix<Eigen::Matrix<float, 1, 3, 1, 1, 3>::Scalar, 3, 1, 0, 3, 1> const&, Eigen::Matrix<Eigen::Matrix<float, 1, 3, 1, 1, 3>::Scalar, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3>> const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3>> const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1>>&);
template void igl::ambient_occlusion<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 1, 0, -1, 1>>(std::function<bool (Eigen::Matrix<Eigen::Matrix<float, -1, 3, 0, -1, 3>::Scalar, 3, 1, 0, 3, 1> const&, Eigen::Matrix<Eigen::Matrix<float, -1, 3, 0, -1, 3>::Scalar, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3>> const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3>> const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1>>&);
template void igl::ambient_occlusion<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, 1, 0, -1, 1>>(std::function<bool (Eigen::Matrix<Eigen::Matrix<float, -1, -1, 0, -1, -1>::Scalar, 3, 1, 0, 3, 1> const&, Eigen::Matrix<Eigen::Matrix<float, -1, -1, 0, -1, -1>::Scalar, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1>> const&, int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1>>&);
#endif
