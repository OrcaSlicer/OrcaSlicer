// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "ray_box_intersect.h"
#include <array>

template <
  typename Derivedsource,
  typename Deriveddir,
  typename Scalar>
IGL_INLINE bool igl::ray_box_intersect(
  const Eigen::MatrixBase<Derivedsource> & origin,
  const Eigen::MatrixBase<Deriveddir> & dir,
  const Eigen::AlignedBox<Scalar,3> & box,
  const Scalar & t0,
  const Scalar & t1,
  Scalar & tmin,
  Scalar & tmax)
{
#ifdef false
  // https://github.com/RMonica/basic_next_best_view/blob/master/src/RayTracer.cpp
  const auto & intersectRayBox = [](
    const Eigen::Vector3f& rayo,
    const Eigen::Vector3f& rayd,
    const Eigen::Vector3f& bmin,
    const Eigen::Vector3f& bmax,
    float & tnear,
    float & tfar
    )->bool
  {
    Eigen::Vector3f bnear;
    Eigen::Vector3f bfar;
    // Checks for intersection testing on each direction coordinate
    // Computes
    float t1, t2;
    tnear = -1e+6f, tfar = 1e+6f; //, tCube;
    bool intersectFlag = true;
    for (int i = 0; i < 3; ++i) {
  //    std::cout << "coordinate " << i << ": bmin " << bmin(i) << ", bmax " << bmax(i) << std::endl;
      assert(bmin(i) <= bmax(i));
      if (::fabs(rayd(i)) < 1e-6) {   // Ray parallel to axis i-th
        if (rayo(i) < bmin(i) || rayo(i) > bmax(i)) {
          intersectFlag = false;
        }
      }
      else {
        // Finds the nearest and the farthest vertices of the box from the ray origin
        if (::fabs(bmin(i) - rayo(i)) < ::fabs(bmax(i) - rayo(i))) {
          bnear(i) = bmin(i);
          bfar(i) = bmax(i);
        }
        else {
          bnear(i) = bmax(i);
          bfar(i) = bmin(i);
        }
  //      std::cout << "  bnear " << bnear(i) << ", bfar " << bfar(i) << std::endl;
        // Finds the distance parameters t1 and t2 of the two ray-box intersections:
        // t1 must be the closest to the ray origin rayo.
        t1 = (bnear(i) - rayo(i)) / rayd(i);
        t2 = (bfar(i) - rayo(i)) / rayd(i);
        if (t1 > t2) {
          std::swap(t1,t2);
        }
        // The two intersection values are used to saturate tnear and tfar
        if (t1 > tnear) {
          tnear = t1;
        }
        if (t2 < tfar) {
          tfar = t2;
        }
  //      std::cout << "  t1 " << t1 << ", t2 " << t2 << ", tnear " << tnear << ", tfar " << tfar
  //        << "  tnear > tfar? " << (tnear > tfar) << ", tfar < 0? " << (tfar < 0) << std::endl;
        if(tnear > tfar) {
          intersectFlag = false;
        }
        if(tfar < 0) {
        intersectFlag = false;
        }
      }
    }
    // Checks whether intersection occurs or not
    return intersectFlag;
  };
  float tmin_f, tmax_f;
  bool ret = intersectRayBox(
      origin.   template cast<float>(),
      dir.      template cast<float>(),
      box.min().template cast<float>(),
      box.max().template cast<float>(),
      tmin_f,
      tmax_f);
  tmin = tmin_f;
  tmax = tmax_f;
  return ret;
#else
  using namespace Eigen;
  // This should be precomputed and provided as input
  typedef Matrix<Scalar,1,3>  RowVector3S;
  const RowVector3S inv_dir( 1./dir(0),1./dir(1),1./dir(2));
  const std::array<bool, 3> sign = { inv_dir(0)<0, inv_dir(1)<0, inv_dir(2)<0};
  // http://people.csail.mit.edu/amy/papers/box-jgt.pdf
  // "An Efficient and Robust Ray–Box Intersection Algorithm"
  Scalar tymin, tymax, tzmin, tzmax;
  std::array<RowVector3S, 2> bounds = {box.min(),box.max()};
  tmin = ( bounds[sign[0]](0)   - origin(0)) * inv_dir(0);
  tmax = ( bounds[1-sign[0]](0) - origin(0)) * inv_dir(0);
  tymin = (bounds[sign[1]](1)   - origin(1)) * inv_dir(1);
  tymax = (bounds[1-sign[1]](1) - origin(1)) * inv_dir(1);
  if ( (tmin > tymax) || (tymin > tmax) )
  {
    return false;
  }
  if (tymin > tmin)
  {
    tmin = tymin;
  }
  if (tymax < tmax)
  {
    tmax = tymax;
  }
  tzmin = (bounds[sign[2]](2) - origin(2))   * inv_dir(2);
  tzmax = (bounds[1-sign[2]](2) - origin(2)) * inv_dir(2);
  if ( (tmin > tzmax) || (tzmin > tmax) )
  {
    return false;
  }
  if (tzmin > tmin)
  {
    tmin = tzmin;
  }
  if (tzmax < tmax)
  {
    tmax = tzmax;
  }
  if(!( (tmin < t1) && (tmax > t0) ))
  {
    return false;
  }
  return true;
#endif
}

#ifdef IGL_STATIC_LIBRARY
template bool igl::ray_box_intersect<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double>(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::AlignedBox<double, 3> const&, double const&, double const&, double&, double&);
#endif
