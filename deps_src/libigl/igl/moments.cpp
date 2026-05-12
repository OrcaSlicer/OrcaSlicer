// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2022 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "moments.h"

// C++17 would avoid this with an if constexpr below
//
// This makes it so that m1 can be:
//   - RowVector3d
//   - Vector3d
//   - RowVectorXd
//   - VectorXd
namespace igl
{
  template <bool SingleRow, bool SingleCol> struct moments_resize_3;
  template <> struct moments_resize_3<true,false>
  {
    template <typename Derivedm1>
    static void run(Eigen::PlainObjectBase<Derivedm1>& m1)
    {
      static_assert(Derivedm1::ColsAtCompileTime == Eigen::Dynamic || 
          Derivedm1::ColsAtCompileTime == 3,"#cols must be 3 or dynamic");
      m1.resize(1,3);
    }
  };
  template <> struct moments_resize_3<false,true>
  {
    template <typename Derivedm1>
    static void run(Eigen::PlainObjectBase<Derivedm1>& m1)
    {
      static_assert(Derivedm1::RowsAtCompileTime == Eigen::Dynamic || 
          Derivedm1::RowsAtCompileTime == 3,"#rows must be 3 or dynamic");
      m1.resize(3,1);
    }
  };
}

template <
  typename DerivedV, 
  typename DerivedF, 
  typename Derivedm0,
  typename Derivedm1,
  typename Derivedm2>
IGL_INLINE void igl::moments(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Derivedm0 & m0,
  Eigen::PlainObjectBase<Derivedm1>& m1,
  Eigen::PlainObjectBase<Derivedm2>& m2)
{
  assert(V.cols() == 3 && "V should be #V by 3");
  typedef typename Derivedm2::Scalar Scalar;

  m0 = 0;
  moments_resize_3<Derivedm1::RowsAtCompileTime == 1,Derivedm1::ColsAtCompileTime == 1>::run(m1);

  m1 << 0,0,0;
  Scalar _xx=0;
  Scalar _yy=0;
  Scalar _zz=0;
  Scalar _yx=0;
  Scalar _zx=0;
  Scalar _zy=0;
  for(int f = 0;f<F.rows();f++)
  {
    // "Computing the Moment of Inertia of a Solid Defined by a Triangle Mesh"
    // (The attached code has a sign bug in I, fixed below.) 
    const Scalar x1 = V(F(f,0),0); 
    const Scalar y1 = V(F(f,0),1); 
    const Scalar z1 = V(F(f,0),2);
    const Scalar x2 = V(F(f,1),0); 
    const Scalar y2 = V(F(f,1),1); 
    const Scalar z2 = V(F(f,1),2);
    const Scalar x3 = V(F(f,2),0); 
    const Scalar y3 = V(F(f,2),1); 
    const Scalar z3 = V(F(f,2),2);
    // Signed volume
    const Scalar v = 
      x1*y2*z3 + y1*z2*x3 + x2*y3*z1 - (x3*y2*z1 + x2*y1*z3 + y3*z2*x1);
    // Contribution to the mass
    m0 += v;
    // Contribution to the centroid
    const Scalar x4 = x1 + x2 + x3; 
    const Scalar y4 = y1 + y2 + y3; 
    const Scalar z4 = z1 + z2 + z3; 
    m1(0) += (v * x4);
    m1(1) += (v * y4);
    m1(2) += (v * z4);
    // Contribution to moment of inertia monomials
    _xx += v * (x1*x1 + x2*x2 + x3*x3 + x4*x4);
    _yy += v * (y1*y1 + y2*y2 + y3*y3 + y4*y4);
    _zz += v * (z1*z1 + z2*z2 + z3*z3 + z4*z4);
    _yx += v * (y1*x1 + y2*x2 + y3*x3 + y4*x4);
    _zx += v * (z1*x1 + z2*x2 + z3*x3 + z4*x4);
    _zy += v * (z1*y1 + z2*y2 + z3*y3 + z4*y4);        
  }
  m0 /= 6.0;
  m1 /= 24.0;

  const double r = 1.0/120.0;
  m2.setZero(3,3);
  m2(1,0) = m1(1)*m1(0)/m0 - _yx * r;
  m2(2,0) = m1(2)*m1(0)/m0 - _zx * r;
  m2(2,1) = m1(2)*m1(1)/m0 - _zy * r;
  m2(0,1) = m2(1,0);
  m2(0,2) = m2(2,0);
  m2(1,2) = m2(2,1);
  _xx = _xx * r - m1(0)*m1(0)/m0;
  _yy = _yy * r - m1(1)*m1(1)/m0;
  _zz = _zz * r - m1(2)*m1(2)/m0;
  m2(0,0) = _yy + _zz;
  m2(1,1) = _zz + _xx;
  m2(2,2) = _xx + _yy;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::moments<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, 1, 3, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1, 3, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, -1>, double, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1, 3, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, -1>, double, Eigen::Matrix<double, 1, 3, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 1, -1, -1>, double, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 1, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 1, -1, -1>, double, Eigen::Matrix<double, 1, 3, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 1, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1,  3, 1, -1, -1>, Eigen::Matrix<int, -1,  3, 1, -1, -1>, double, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1,  3, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1,  3, 1, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::moments<Eigen::Matrix<double, -1,  3, 1, -1, -1>, Eigen::Matrix<int, -1,  3, 1, -1, -1>, double, Eigen::Matrix<double, 1, 3, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1,  3, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1,  3, 1, -1, -1> > const&, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
#endif
