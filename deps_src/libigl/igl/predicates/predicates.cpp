// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Qingnan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "./predicates.h"
// This is a different file also called predicates.h
#include <predicates.h>
#include <type_traits>

namespace igl {
namespace predicates {

using REAL = IGL_PREDICATES_REAL;

#ifdef LIBIGL_PREDICATES_USE_FLOAT
#define IGL_PREDICATES_ASSERT_SCALAR(Vector)                        \
  static_assert(                                         \
    std::is_same<typename Vector::Scalar, float>::value, \
    "Shewchuk's exact predicates only support float")
#else
#define IGL_PREDICATES_ASSERT_SCALAR(Vector)                           \
  static_assert(                                            \
    std::is_same<typename Vector::Scalar, double>::value || \
    std::is_same<typename Vector::Scalar, float>::value,    \
    "Shewchuk's exact predicates only support float and double")
#endif

IGL_INLINE void exactinit() {
  // Thread-safe initialization using Meyers' singleton
  class MySingleton {
  public:
    static MySingleton& instance() {
      static MySingleton instance;
      return instance;
    }
  private:
    MySingleton() { ::exactinit(); }
  };
  MySingleton::instance();
}

template<typename Vector2D>
IGL_INLINE Orientation orient2d(
    const Eigen::MatrixBase<Vector2D>& pa,
    const Eigen::MatrixBase<Vector2D>& pb,
    const Eigen::MatrixBase<Vector2D>& pc)
{
  EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Vector2D, 2);
  IGL_PREDICATES_ASSERT_SCALAR(Vector2D);

  using Point = Eigen::Matrix<REAL, 2, 1>;
  Point a{pa[0], pa[1]};
  Point b{pb[0], pb[1]};
  Point c{pc[0], pc[1]};

  const auto r = ::orient2d(a.data(), b.data(), c.data());

  if (r > 0) return Orientation::POSITIVE;
  else if (r < 0) return Orientation::NEGATIVE;
  else return Orientation::COLLINEAR;
}

template<typename Vector3D>
IGL_INLINE Orientation orient3d(
    const Eigen::MatrixBase<Vector3D>& pa,
    const Eigen::MatrixBase<Vector3D>& pb,
    const Eigen::MatrixBase<Vector3D>& pc,
    const Eigen::MatrixBase<Vector3D>& pd)
{
  EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Vector3D, 3);
  IGL_PREDICATES_ASSERT_SCALAR(Vector3D);

  using Point = Eigen::Matrix<REAL, 3, 1>;
  Point a{pa[0], pa[1], pa[2]};
  Point b{pb[0], pb[1], pb[2]};
  Point c{pc[0], pc[1], pc[2]};
  Point d{pd[0], pd[1], pd[2]};

  const auto r = ::orient3d(a.data(), b.data(), c.data(), d.data());

  if (r > 0) return Orientation::POSITIVE;
  else if (r < 0) return Orientation::NEGATIVE;
  else return Orientation::COPLANAR;
}

template<typename Vector2D>
IGL_INLINE Orientation incircle(
    const Eigen::MatrixBase<Vector2D>& pa,
    const Eigen::MatrixBase<Vector2D>& pb,
    const Eigen::MatrixBase<Vector2D>& pc,
    const Eigen::MatrixBase<Vector2D>& pd)
{
  EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Vector2D, 2);
  IGL_PREDICATES_ASSERT_SCALAR(Vector2D);

  using Point = Eigen::Matrix<REAL, 2, 1>;
  Point a{pa[0], pa[1]};
  Point b{pb[0], pb[1]};
  Point c{pc[0], pc[1]};
  Point d{pd[0], pd[1]};

  const auto r = ::incircle(a.data(), b.data(), c.data(), d.data());

  if (r > 0) return Orientation::INSIDE;
  else if (r < 0) return Orientation::OUTSIDE;
  else return Orientation::COCIRCULAR;
}

template<typename Vector3D>
IGL_INLINE Orientation insphere(
    const Eigen::MatrixBase<Vector3D>& pa,
    const Eigen::MatrixBase<Vector3D>& pb,
    const Eigen::MatrixBase<Vector3D>& pc,
    const Eigen::MatrixBase<Vector3D>& pd,
    const Eigen::MatrixBase<Vector3D>& pe)
{
  EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Vector3D, 3);
  IGL_PREDICATES_ASSERT_SCALAR(Vector3D);

  using Point = Eigen::Matrix<REAL, 3, 1>;
  Point a{pa[0], pa[1], pa[2]};
  Point b{pb[0], pb[1], pb[2]};
  Point c{pc[0], pc[1], pc[2]};
  Point d{pd[0], pd[1], pd[2]};
  Point e{pe[0], pe[1], pe[2]};

  const auto r = ::insphere(a.data(), b.data(), c.data(), d.data(), e.data());

  if (r > 0) return Orientation::INSIDE;
  else if (r < 0) return Orientation::OUTSIDE;
  else return Orientation::COSPHERICAL;
}

}
}

#undef IGL_PREDICATES_ASSERT_SCALAR

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation

#define IGL_ORIENT2D(Vector) template igl::predicates::Orientation igl::predicates::orient2d<Vector>(const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&)
#define IGL_INCIRCLE(Vector) template igl::predicates::Orientation igl::predicates::incircle<Vector>(const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&)
#define IGL_ORIENT3D(Vector) template igl::predicates::Orientation igl::predicates::orient3d<Vector>(const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&)
#define IGL_INSPHERE(Vector) template igl::predicates::Orientation igl::predicates::insphere<Vector>(const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&, const Eigen::MatrixBase<Vector>&)

#define IGL_MATRIX(T, R, C) Eigen::Matrix<T, R, C>
IGL_ORIENT2D(IGL_MATRIX(float, 1, 2));
IGL_INCIRCLE(IGL_MATRIX(float, 1, 2));
IGL_ORIENT2D(IGL_MATRIX(float, 2, 1));
IGL_INCIRCLE(IGL_MATRIX(float, 2, 1));
IGL_ORIENT3D(IGL_MATRIX(float, 1, 3));
IGL_INSPHERE(IGL_MATRIX(float, 1, 3));
IGL_ORIENT3D(IGL_MATRIX(float, 3, 1));
IGL_INSPHERE(IGL_MATRIX(float, 3, 1));

#ifndef LIBIGL_PREDICATES_USE_FLOAT
IGL_ORIENT2D(IGL_MATRIX(double, 1, 2));
IGL_INCIRCLE(IGL_MATRIX(double, 1, 2));
IGL_ORIENT2D(IGL_MATRIX(double, 2, 1));
IGL_INCIRCLE(IGL_MATRIX(double, 2, 1));
IGL_ORIENT3D(IGL_MATRIX(double, 1, 3));
IGL_INSPHERE(IGL_MATRIX(double, 1, 3));
IGL_ORIENT3D(IGL_MATRIX(double, 3, 1));
IGL_INSPHERE(IGL_MATRIX(double, 3, 1));
#endif
#undef IGL_MATRIX

#undef IGL_ORIENT2D
#undef IGL_ORIENT3D
#undef IGL_INCIRCLE
#undef IGL_INSPHERE

#endif
