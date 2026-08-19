// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "per_face_normals.h"
#include "parallel_for.h"
#include <Eigen/Geometry>

#define SQRT_ONE_OVER_THREE 0.57735026918962573
template <typename DerivedV, typename DerivedF, typename DerivedZ, typename DerivedN>
IGL_INLINE void igl::per_face_normals(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedZ> & Z,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  N.resize(F.rows(),3);
  // loop over faces
  int Frows = F.rows();
  parallel_for(Frows,[&](const int i)
  {
    const Eigen::Matrix<typename DerivedV::Scalar, 1, 3> v1 = V.row(F(i,1)) - V.row(F(i,0));
    const Eigen::Matrix<typename DerivedV::Scalar, 1, 3> v2 = V.row(F(i,2)) - V.row(F(i,0));
    N.row(i) = v1.cross(v2);//.normalized();
    typename DerivedV::Scalar r = N.row(i).norm();
    if(r == 0)
    {
      N.row(i) = Z;
    }else
    {
      N.row(i) /= r;
    }
  },10000);
}

template <typename DerivedV, typename DerivedF, typename DerivedN>
IGL_INLINE void igl::per_face_normals(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  Eigen::Matrix<typename DerivedN::Scalar,3,1> Z(0,0,0);
  return per_face_normals(V,F,Z,N);
}

template <typename DerivedV, typename DerivedF, typename DerivedN>
IGL_INLINE void igl::per_face_normals_stable(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  typedef Eigen::Matrix<typename DerivedV::Scalar,1,3> RowVectorV3;
  typedef typename DerivedV::Scalar Scalar;

  const size_t m = F.rows();

  N.resize(F.rows(),3);
  // Grad all points
  parallel_for(m,[&](const int f)
  {
    const RowVectorV3 p0 = V.row(F(f,0));
    const RowVectorV3 p1 = V.row(F(f,1));
    const RowVectorV3 p2 = V.row(F(f,2));
    const RowVectorV3 n0 = (p1 - p0).cross(p2 - p0);
    const RowVectorV3 n1 = (p2 - p1).cross(p0 - p1);
    const RowVectorV3 n2 = (p0 - p2).cross(p1 - p2);

    // careful sum
    for(int d = 0;d<3;d++)
    {
      // This is a little _silly_ in terms of complexity, but its recursive
      // implementation is clean looking...
      const std::function<Scalar(Scalar,Scalar,Scalar)> sum3 =
        [&sum3](Scalar a, Scalar b, Scalar c)->Scalar
      {
        if(fabs(c)>fabs(a))
        {
          return sum3(c,b,a);
        }
        // c < a
        if(fabs(c)>fabs(b))
        {
          return sum3(a,c,b);
        }
        // c < a, c < b
        if(fabs(b)>fabs(a))
        {
          return sum3(b,a,c);
        }
        return (a+b)+c;
      };

      N(f,d) = sum3(n0(d),n1(d),n2(d));
    }
    // sum better not be sure, or else NaN
    N.row(f) /= N.row(f).norm();
  },10000);

}

#include "cotmatrix.h"

template <
  typename DerivedV,
  typename DerivedI,
  typename DerivedC,
  typename DerivedN,
  typename DerivedVV,
  typename DerivedFF,
  typename DerivedJ>
IGL_INLINE void igl::per_face_normals(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedI> & I,
  const Eigen::MatrixBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedVV> & VV,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  assert(V.cols() == 3);
  typedef Eigen::Index Index;
  typedef typename DerivedN::Scalar Scalar;
  // Use Bunge et al. algorithm in igl::cotmatrix to insert a point for each
  // polygon which minimizes squared area.
  {
    Eigen::SparseMatrix<Scalar> _1,_2,P;
    igl::cotmatrix(V,I,C,_1,_2,P);
    VV = P*V;
  }
  // number of polygons
  const Eigen::Index m = C.size()-1;
  N.resize(m,3);
  FF.resize(C(m),3);
  J.resize(C(m));
  {
    Eigen::Index k = 0;
    for(Eigen::Index p = 0;p<m;p++)
    {
      N.row(p).setZero();
      // number of faces/vertices in this simple polygon
      const Index np = C(p+1)-C(p);
      for(Eigen::Index i = 0;i<np;i++)
      {
        FF.row(k) << 
          I(C(p)+((i+0)%np)),
          I(C(p)+((i+1)%np)),
          V.rows()+p;
        J(k) = p;
        k++;
        typedef Eigen::Matrix<Scalar,1,3> V3;
        N.row(p) +=
          V3(VV.row(I(C(p)+((i+0)%np)))-VV.row(V.rows()+p)).cross(
          V3(VV.row(I(C(p)+((i+1)%np)))-VV.row(V.rows()+p)));
      }
      // normalize to take average
      N.row(p) /= N.row(p).stableNorm();
    }
    assert(k == FF.rows());
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::per_face_normals<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&);
// generated by autoexplicit.sh
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::per_face_normals<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
// Nonsense template. Where'd this come from? AABB nonsense?
namespace igl{template<> void per_face_normals<Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&){} }
template void igl::per_face_normals<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&);
template void igl::per_face_normals<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_face_normals<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&);
template void igl::per_face_normals<Eigen::Matrix<float, -1, -1, 1, -1, -1>, Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1>, Eigen::Matrix<float, -1, -1, 1, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 1, -1, -1> >&);
template void igl::per_face_normals<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&);
template void igl::per_face_normals<class Eigen::Matrix<double,-1,3,0,-1,3>,class Eigen::Matrix<int,-1,-1,0,-1,-1>,class Eigen::Matrix<double,-1,-1,0,-1,-1> >(class Eigen::MatrixBase<class Eigen::Matrix<double,-1,3,0,-1,3> > const &,class Eigen::MatrixBase<class Eigen::Matrix<int,-1,-1,0,-1,-1> > const &,class Eigen::PlainObjectBase<class Eigen::Matrix<double,-1,-1,0,-1,-1> > &);
template void igl::per_face_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, 2, 3, 0, 2, 3>, Eigen::Matrix<double, 2, 3, 0, 2, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, 2, 3, 0, 2, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 3, 0, 2, 3> >&);
template void igl::per_face_normals_stable<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::per_face_normals_stable<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_face_normals_stable<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
#endif
