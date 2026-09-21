// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2021 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "per_corner_normals.h"
#include "vertex_triangle_adjacency.h"
#include "per_face_normals.h"
#include "PI.h"
#include "parallel_for.h"
#include "doublearea.h"
#include <Eigen/Geometry>

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedCN>
IGL_INLINE void igl::per_corner_normals(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const typename DerivedV::Scalar corner_threshold_degrees,
  Eigen::PlainObjectBase<DerivedCN> & CN)
{
  Eigen::Matrix<Eigen::Index,Eigen::Dynamic,1> VF,NK;
  vertex_triangle_adjacency(F,V.rows(),VF,NK);
  return per_corner_normals(V,F,corner_threshold_degrees,VF,NK,CN);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedVF,
  typename DerivedNI,
  typename DerivedCN>
IGL_INLINE void igl::per_corner_normals(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const typename DerivedV::Scalar corner_threshold_degrees,
  const Eigen::MatrixBase<DerivedVF> & VF,
  const Eigen::MatrixBase<DerivedNI> & NI,
  Eigen::PlainObjectBase<DerivedCN> & CN)
{
  typedef typename DerivedV::Scalar Scalar;
  typedef Eigen::Index Index;
  // unit normals
  Eigen::Matrix<Scalar,Eigen::Dynamic,3,Eigen::RowMajor> FN(F.rows(),3);
  // face areas
  Eigen::Matrix<Scalar,Eigen::Dynamic,1> FA(F.rows());
  igl::parallel_for(F.rows(),[&](const Index f)
  {
    const Eigen::Matrix<Scalar,1,3> v10 = V.row(F(f,1))-V.row(F(f,0));
    const Eigen::Matrix<Scalar,1,3> v20 = V.row(F(f,2))-V.row(F(f,0));
    const Eigen::Matrix<Scalar,1,3> n = v10.cross(v20);
    const Scalar a = n.norm();
    FA(f) = a;
    FN.row(f) = n/a;
  },10000);

  // number of faces
  const Index m = F.rows();
  // valence of faces
  const Index n = F.cols();
  assert(n == 3);

  // initialize output to ***zero***
  CN.setZero(m*n,3);

  const Scalar cos_thresh = cos(corner_threshold_degrees*igl::PI/180);
  // loop over faces
  //for(Index i = 0;i<m;i++)
  igl::parallel_for(F.rows(),[&](const Index i)
  {
    // Normal of this face
    const auto & fnhat = FN.row(i);
    // loop over corners
    for(Index j = 0;j<n;j++)
    {
      const auto & v = F(i,j);
      for(int k = NI[v]; k<NI[v+1]; k++)
      {
        const auto & ifn = FN.row(VF[k]);
        // dot product between face's normal and other face's normal
        const Scalar dp = fnhat.dot(ifn);
        // if difference in normal is slight then add to average
        if(dp > cos_thresh)
        {
          // add to running sum
          CN.row(i*n+j) += ifn*FA(VF[k]);
        }
      }
      // normalize to take average
      CN.row(i*n+j).normalize();
    }
  },10000);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedCI,
  typename DerivedCC,
  typename DerivedCN>
IGL_INLINE void igl::per_corner_normals(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedCI> & CI,
  const Eigen::MatrixBase<DerivedCC> & CC,
  Eigen::PlainObjectBase<DerivedCN> & CN)
{
  typedef typename DerivedV::Scalar Scalar;
  typedef Eigen::Index Index;
  assert(CC.rows() == F.rows()*3+1);
  // area weighted normals
  Eigen::Matrix<Scalar,Eigen::Dynamic,3,Eigen::RowMajor> FN(F.rows(),3);
  //for(Index f = 0;f<F.rows();f++)
  igl::parallel_for(F.rows(),[&](const Index f)
  {
    const Eigen::Matrix<Scalar,1,3> v10 = V.row(F(f,1))-V.row(F(f,0));
    const Eigen::Matrix<Scalar,1,3> v20 = V.row(F(f,2))-V.row(F(f,0));
    FN.row(f) = v10.cross(v20);
  },10000);

  // number of faces
  const Index m = F.rows();
  // valence of faces
  const Index n = F.cols();
  assert(n == 3);

  // initialize output to ***zero***
  CN.setZero(m*n,3);
  // loop over faces
  igl::parallel_for(m*n,[&](const Index ci)
  {
    for(int k = CC(ci); k<CC(ci+1); k++)
    {
      // add to running sum
      const auto cfk = CI(k);
      CN.row(ci) += FN.row(cfk);
    }
    // normalize to take average
    CN.row(ci).normalize();
  },10000);
}

template <typename DerivedNV, typename DerivedNF, typename DerivedCN>
IGL_INLINE void igl::per_corner_normals(
  const Eigen::MatrixBase<DerivedNV> & NV,
  const Eigen::MatrixBase<DerivedNF> & NF,
  Eigen::PlainObjectBase<DerivedCN> & CN)
{
  const auto m = NF.rows();
  const auto nc = NF.cols();
  CN.resize(m*nc,3);
  for(Eigen::Index i = 0;i<m;i++)
  {
    for(Eigen::Index c = 0;c<nc;c++)
    {
      CN.row(i*nc+c) = NV.row(NF(i,c));
    }
  }
}

template <
  typename DerivedV, 
  typename DerivedI, 
  typename DerivedC, 
  typename DerivedN,
  typename DerivedVV,
  typename DerivedFF,
  typename DerivedJ,
  typename DerivedNN>
IGL_INLINE void igl::per_corner_normals(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedI> & I,
  const Eigen::MatrixBase<DerivedC> & C,
  const typename DerivedV::Scalar corner_threshold_degrees,
  Eigen::PlainObjectBase<DerivedN>  & N,
  Eigen::PlainObjectBase<DerivedVV> & VV,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedJ>  & J,
  Eigen::PlainObjectBase<DerivedNN> & NN)
{
  const Eigen::Index m = C.size()-1;
  typedef Eigen::Index Index;
  Eigen::MatrixXd FN;
  per_face_normals(V,I,C,FN,VV,FF,J);
  typedef typename DerivedN::Scalar Scalar;
  Eigen::Matrix<Scalar,Eigen::Dynamic,1> AA;
  doublearea(VV,FF,AA);
  // VF[i](j) = p means p is the jth face incident on vertex i
  // to-do micro-optimization to avoid vector<vector>
  std::vector<std::vector<Eigen::Index>> VF(V.rows());
  for(Eigen::Index p = 0;p<m;p++)
  {
    // number of faces/vertices in this simple polygon
    const Index np = C(p+1)-C(p);
    for(Eigen::Index i = 0;i<np;i++)
    {
      VF[I(C(p)+i)].push_back(p);
    }
  }

  N.resize(I.rows(),3);
  for(Eigen::Index p = 0;p<m;p++)
  {
    // number of faces/vertices in this simple polygon
    const Index np = C(p+1)-C(p);
    Eigen::Matrix<Scalar,3,1> fn = FN.row(p);
    for(Eigen::Index i = 0;i<np;i++)
    {
      N.row(C(p)+i).setZero();
      // Loop over faces sharing this vertex
      for(const auto & n : VF[I(C(p)+i)])
      {
        Eigen::Matrix<Scalar,3,1> ifn = FN.row(n);
        // dot product between face's normal and other face's normal
        Scalar dp = fn.dot(ifn);
        if(dp > cos(corner_threshold_degrees*igl::PI/180))
        {
          // add to running sum
          N.row(C(p)+i) += AA(n) * ifn;
        }
      }
      N.row(C(p)+i).normalize();
    }
  }

  // Relies on order of FF 
  NN.resize(FF.rows()*3,3);
  {
    Eigen::Index k = 0;
    for(Eigen::Index p = 0;p<m;p++)
    {
      // number of faces/vertices in this simple polygon
      const Index np = C(p+1)-C(p);
      for(Eigen::Index i = 0;i<np;i++)
      {
        assert(FF(k,0) == I(C(p)+((i+0)%np)));
        assert(FF(k,1) == I(C(p)+((i+1)%np)));
        assert(FF(k,2) == V.rows()+p);
        NN.row(k*3+0) = N.row(C(p)+((i+0)%np));
        NN.row(k*3+1) = N.row(C(p)+((i+1)%np));
        NN.row(k*3+2) = FN.row(p);
        k++;
      }
    }
    assert(k == FF.rows());
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
// Clang thinks this is the same as the one below, but Windows doesn't?
// template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::per_corner_normals<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double, Eigen::PlainObjectBase< Eigen::Matrix<double, -1, -1, 0, -1, -1> > &);
#endif
