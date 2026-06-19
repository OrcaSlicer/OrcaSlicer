// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "point_mesh_squared_distance.h"
#include "AABB.h"
#include <cassert>

namespace 
{
  // Sigh. This is a somewhat elaborate way of making
  // igl::point_mesh_squared_distance build a correctly dimensioned AABB tree
  // regardless of whether it has #columns known at compile time or not.
  //
  // The pattern below uses a class which can be partially specialized on
  // whether columns are dynamic. For each case, we call this helper function at
  // the top which has a known dimension as an extra template argument.
  //
  // In this case, the _code_ for the 2D and 3D cases is the same, so we avoid
  // duplicating lines of code that only differ in the dimension. The cost
  // appears to be spelling out _all_ of the template types over and over again
  // making this all harder to parse.
template <
  int DIM,
  typename DerivedP,
  typename DerivedV,
  typename DerivedEle,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
IGL_INLINE void point_mesh_squared_distance(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C)
{
  static_assert(DIM == 2 || DIM == 3, "DIM must be 2 or 3");
  // Common code for 2D and 3D
  igl::AABB<DerivedV, DIM> tree;
  tree.init(V,Ele);
  tree.squared_distance(V,Ele,P,sqrD,I,C);
}

// Class whose templates can be specialized on whether V has dynamic columns or
// not.
template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedEle,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC,
  bool DynamicCols> 
struct point_mesh_squared_distance_DIM_Handler;

// Handle the case where V has dynamic columns
template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedEle,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
struct point_mesh_squared_distance_DIM_Handler<
  DerivedP, DerivedV, DerivedEle, DerivedsqrD, DerivedI, DerivedC,
  true>
{
  static void compute(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedC> & C)
  {
    if(V.cols() == 2)
    {
      point_mesh_squared_distance<2>(P,V,Ele,sqrD,I,C);
    }else if(V.cols() == 3)
    {
      point_mesh_squared_distance<3>(P,V,Ele,sqrD,I,C);
    }else
    {
      assert(false && "V must be 2D or 3D");
    }
  }
};

// Handle the case where V has fixed size columns
template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedEle,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
struct point_mesh_squared_distance_DIM_Handler<
  DerivedP, DerivedV, DerivedEle, DerivedsqrD, DerivedI, DerivedC, 
  false>
{
  static void compute(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedC> & C)
  {
    constexpr int DIM = 
      DerivedP::ColsAtCompileTime != Eigen::Dynamic ? DerivedP::ColsAtCompileTime :
      DerivedV::ColsAtCompileTime != Eigen::Dynamic ? DerivedV::ColsAtCompileTime :
      DerivedC::ColsAtCompileTime != Eigen::Dynamic ? DerivedC::ColsAtCompileTime :
      Eigen::Dynamic;
    static_assert(DIM == 2 || DIM == 3, "DIM must be 2 or 3");
    point_mesh_squared_distance<DIM>(P,V,Ele,sqrD,I,C);
  }
};
}

template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedEle,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
IGL_INLINE void igl::point_mesh_squared_distance(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C)
{
  constexpr int DIM = 
    DerivedP::ColsAtCompileTime != Eigen::Dynamic ? DerivedP::ColsAtCompileTime :
    DerivedV::ColsAtCompileTime != Eigen::Dynamic ? DerivedV::ColsAtCompileTime :
    DerivedC::ColsAtCompileTime != Eigen::Dynamic ? DerivedC::ColsAtCompileTime :
    Eigen::Dynamic;

  point_mesh_squared_distance_DIM_Handler<
    DerivedP, 
    DerivedV, 
    DerivedEle, 
    DerivedsqrD, 
    DerivedI, 
    DerivedC,
    DIM == Eigen::Dynamic>::compute(P,V,Ele,sqrD,I,C);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::point_mesh_squared_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> &);
template void igl::point_mesh_squared_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::point_mesh_squared_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::point_mesh_squared_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::point_mesh_squared_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);

#ifdef WIN32
template void igl::point_mesh_squared_distance<class Eigen::Matrix<double, -1, -1, 0, -1, -1>, class Eigen::Matrix<double, -1, -1, 0, -1, -1>, class Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<__int64, -1, 1, 0, -1, 1>, class Eigen::Matrix<double, -1, 3, 0, -1, 3>>(class Eigen::MatrixBase<class Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, class Eigen::MatrixBase<class Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, class Eigen::MatrixBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<double, -1, 1, 0, -1, 1>> &, class Eigen::PlainObjectBase<class Eigen::Matrix<__int64, -1, 1, 0, -1, 1>> &, class Eigen::PlainObjectBase<class Eigen::Matrix<double, -1, 3, 0, -1, 3>> &);
template void igl::point_mesh_squared_distance<class Eigen::Matrix<double, -1, -1, 0, -1, -1>, class Eigen::Matrix<double, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<double, -1, 1, 0, -1, 1>, class Eigen::Matrix<__int64, -1, 1, 0, -1, 1>, class Eigen::Matrix<double, -1, 3, 0, -1, 3>>(class Eigen::MatrixBase<class Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, class Eigen::MatrixBase<class Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, class Eigen::MatrixBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<double, -1, 1, 0, -1, 1>> &, class Eigen::PlainObjectBase<class Eigen::Matrix<__int64, -1, 1, 0, -1, 1>> &, class Eigen::PlainObjectBase<class Eigen::Matrix<double, -1, 3, 0, -1, 3>> &);
#endif
#endif
