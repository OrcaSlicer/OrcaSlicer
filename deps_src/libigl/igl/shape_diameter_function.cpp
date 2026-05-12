// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "shape_diameter_function.h"
#include "random_dir.h"
#include "barycenter.h"
#include "ray_mesh_intersect.h"
#include "per_vertex_normals.h"
#include "PlainMatrix.h"
#include "per_face_normals.h"
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
IGL_INLINE void igl::shape_diameter_function(
  const std::function<
    typename DerivedP::Scalar(
      const Eigen::Matrix<typename DerivedP::Scalar,3,1> &,
      const Eigen::Matrix<typename DerivedP::Scalar,3,1> &)
      > & shoot_ray,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  using namespace Eigen;
  using Scalar = typename DerivedP::Scalar;
  using Vector3S = Eigen::Matrix<typename DerivedP::Scalar,3,1>;
  const int n = P.rows();
  // Resize output
  S.resize(n,1);
  // Embree seems to be parallel when constructing but not when tracing rays
  const Matrix<Scalar,Eigen::Dynamic,3> D = random_dir_stratified(num_samples).cast<Scalar>();

  const auto & inner = [&P,&N,&num_samples,&D,&S,&shoot_ray](const int p)
  {
    const Vector3S origin = P.row(p);
    const Vector3S normal = N.row(p);
    int num_hits = 0;
    double total_distance = 0;
    for(int s = 0;s<num_samples;s++)
    {
      Vector3S d = D.row(s);
      // Shoot _inward_
      if(d.dot(normal) > 0)
      {
        // reverse ray
        d *= -1;
      }
      const double dist = shoot_ray(origin,d);
      if(std::isfinite(dist))
      {
        total_distance += dist;
        num_hits++;
      }
    }
    S(p) = total_distance/(double)num_hits;
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
IGL_INLINE void igl::shape_diameter_function(
  const igl::AABB<DerivedV,DIM> & aabb,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  using Scalar = typename DerivedP::Scalar;
  using Vector3S = Eigen::Matrix<typename DerivedP::Scalar,3,1>;
  const auto & shoot_ray = [&aabb,&V,&F](
    const Vector3S& _s,
    const Vector3S& dir)->double
  {
    Vector3S s = _s+1e-4*dir;
    igl::Hit<Scalar> hit;
    if(aabb.intersect_ray(
      V,
      F,
      s  ,
      dir,
      hit))
    {
      return hit.t;
    }else
    {
      return std::numeric_limits<double>::infinity();
    }
  };
  return shape_diameter_function(shoot_ray,P,N,num_samples,S);

}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedN,
  typename DerivedS >
IGL_INLINE void igl::shape_diameter_function(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  using Scalar = typename DerivedP::Scalar;
  using Vector3S = Eigen::Matrix<typename DerivedP::Scalar,3,1>;
  if(F.rows() < 100)
  {
    // Super naive
    const auto & shoot_ray = [&V,&F](
      const Vector3S& _s,
      const Vector3S& dir)->double
    {
      Vector3S s = _s+1e-4*dir;
      igl::Hit<Scalar> hit;
      if(ray_mesh_intersect(s,dir,V,F,hit))
      {
        return hit.t;
      }else
      {
        return std::numeric_limits<double>::infinity();
      }
    };
    return shape_diameter_function(shoot_ray,P,N,num_samples,S);
  }
  AABB<DerivedV,3> aabb;
  aabb.init(V,F);
  return shape_diameter_function(aabb,V,F,P,N,num_samples,S);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedS>
IGL_INLINE void igl::shape_diameter_function(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const bool per_face,
  const int num_samples,
  Eigen::PlainObjectBase<DerivedS> & S)
{
  if (per_face)
  {
    PlainMatrix<DerivedV> N;
    igl::per_face_normals(V, F, N);
    PlainMatrix<DerivedV> P;
    igl::barycenter(V, F, P);
    return igl::shape_diameter_function(V, F, P, N, num_samples, S);
  }
  else
  {
    PlainMatrix<DerivedV> N;
    igl::per_vertex_normals(V, F, N);
    return igl::shape_diameter_function(V, F, V, N, num_samples, S);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::shape_diameter_function<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(std::function<double (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::shape_diameter_function<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(std::function<double (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::shape_diameter_function<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(std::function<double (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::shape_diameter_function<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::function<double (Eigen::Matrix<double, 3, 1, 0, 3, 1> const&, Eigen::Matrix<double, 3, 1, 0, 3, 1> const&)> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::shape_diameter_function<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, bool, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif

