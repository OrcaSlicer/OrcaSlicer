// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "minkowski_sum.h"
#include "mesh_boolean.h"

#include "../../slice.h"
#include "../../slice_mask.h"
#include "../../LinSpaced.h"
#include "../../unique_rows.h"
#include "../../get_seconds.h"
#include "../../edges.h"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <cassert>
#include <vector>
#include <iostream>


template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedW,
  typename DerivedG,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::minkowski_sum(
  const Eigen::MatrixBase<DerivedVA> & VA,
  const Eigen::MatrixBase<DerivedFA> & FA,
  const Eigen::MatrixBase<DerivedVB> & VB,
  const Eigen::MatrixBase<DerivedFB> & FB,
  const bool resolve_overlaps,
  Eigen::PlainObjectBase<DerivedW> & W,
  Eigen::PlainObjectBase<DerivedG> & G,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  using namespace std;
  using namespace Eigen;
  assert(FA.cols() == 3 && "FA must contain a closed triangle mesh");
  assert(FB.cols() <= FA.cols() && 
    "FB must contain lower diemnsional simplices than FA");
  const auto tictoc = []()->double
  {
    static double t_start;
    double now = igl::get_seconds();
    double interval = now-t_start;
    t_start = now;
    return interval;
  };
  tictoc();
  Matrix<typename DerivedFB::Scalar,Dynamic,2> EB;
  edges(FB,EB);
  Matrix<typename DerivedFA::Scalar,Dynamic,2> EA(0,2);
  if(FB.cols() == 3)
  {
    edges(FA,EA);
  }
  // number of copies of A along edges of B
  const int n_ab = EB.rows();
  // number of copies of B along edges of A
  const int n_ba = EA.rows();

  vector<DerivedW> vW(n_ab + n_ba);
  vector<DerivedG> vG(n_ab + n_ba);
  vector<DerivedJ> vJ(n_ab + n_ba);
  vector<int> offsets(n_ab + n_ba + 1);
  offsets[0] = 0;
  // sweep A along edges of B
  for(int e = 0;e<n_ab;e++)
  {
    Matrix<typename DerivedJ::Scalar,Dynamic,1> eJ;
    minkowski_sum(
      VA,
      FA,
      VB.row(EB(e,0)).eval(),
      VB.row(EB(e,1)).eval(),
      false,
      vW[e],
      vG[e],
      eJ);
    assert(vG[e].rows() == eJ.rows());
    assert(eJ.cols() == 1);
    vJ[e].resize(vG[e].rows(),2);
    vJ[e].col(0) = eJ;
    vJ[e].col(1).setConstant(e);
    offsets[e+1] = offsets[e] + vW[e].rows();
  }
  // sweep B along edges of A
  for(int e = 0;e<n_ba;e++)
  {
    Matrix<typename DerivedJ::Scalar,Dynamic,1> eJ;
    const int ee = n_ab+e;
    minkowski_sum(
      VB,
      FB,
      VA.row(EA(e,0)).eval(),
      VA.row(EA(e,1)).eval(),
      false,
      vW[ee],
      vG[ee],
      eJ);
    vJ[ee].resize(vG[ee].rows(),2);
    vJ[ee].col(0) = eJ.array() + (FA.rows()+1);
    vJ[ee].col(1).setConstant(ee);
    offsets[ee+1] = offsets[ee] + vW[ee].rows();
  }
  // Combine meshes
  int n=0,m=0;
  for_each(vW.begin(),vW.end(),[&n](const DerivedW & w){n+=w.rows();});
  for_each(vG.begin(),vG.end(),[&m](const DerivedG & g){m+=g.rows();});
  assert(n == offsets.back());

  W.resize(n,3);
  G.resize(m,3);
  J.resize(m,2);
  {
    int m_off = 0,n_off = 0;
    for(int i = 0;i<vG.size();i++)
    {
      W.block(n_off,0,vW[i].rows(),3) = vW[i];
      G.block(m_off,0,vG[i].rows(),3) = vG[i].array()+offsets[i];
      J.block(m_off,0,vJ[i].rows(),2) = vJ[i];
      n_off += vW[i].rows();
      m_off += vG[i].rows();
    }
    assert(n == n_off);
    assert(m == m_off);
  }
  if(resolve_overlaps)
  {
    Eigen::Matrix<typename DerivedJ::Scalar, Eigen::Dynamic,1> SJ;
    mesh_boolean(
      DerivedW(W),
      DerivedG(G),
      Matrix<typename DerivedW::Scalar,Dynamic,Dynamic>(),
      Matrix<typename DerivedG::Scalar,Dynamic,Dynamic>(),
      MESH_BOOLEAN_TYPE_UNION,
      W,
      G,
      SJ);
    slice(DerivedJ(J),SJ,1,J);
  }
}

template <
  typename DerivedVA,
  typename DerivedFA,
  typename sType, int sCols, int sOptions,
  typename dType, int dCols, int dOptions,
  typename DerivedW,
  typename DerivedG,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::minkowski_sum(
  const Eigen::MatrixBase<DerivedVA> & VA,
  const Eigen::MatrixBase<DerivedFA> & FA,
  const Eigen::Matrix<sType,1,sCols,sOptions> & s,
  const Eigen::Matrix<dType,1,dCols,dOptions> & d,
  const bool resolve_overlaps, 
  Eigen::PlainObjectBase<DerivedW> & W,
  Eigen::PlainObjectBase<DerivedG> & G,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  using namespace Eigen;
  using namespace std;
  assert(s.cols() == 3 && "s should be a 3d point");
  assert(d.cols() == 3 && "d should be a 3d point");
  // silly base case
  if(FA.size() == 0)
  {
    W.resize(0,3);
    G.resize(0,3);
    return;
  }
  const int dim = VA.cols();
  assert(dim == 3 && "dim must be 3D");
  assert(s.size() == 3 && "s must be 3D point");
  assert(d.size() == 3 && "d must be 3D point");
  // segment vector
  const CGAL::Vector_3<CGAL::Epeck> v(d(0)-s(0),d(1)-s(1),d(2)-s(2));
  // number of vertices
  const int n = VA.rows();
  // duplicate vertices at s and d, we'll remove unreferernced later
  W.resize(2*n,dim);
  for(int i = 0;i<n;i++)
  {
    for(int j = 0;j<dim;j++)
    {
      W  (i,j) = VA(i,j) + s(j);
      W(i+n,j) = VA(i,j) + d(j);
    }
  }
  // number of faces
  const int m = FA.rows();
  //// Mask whether positive dot product, or negative: because of exactly zero,
  //// these are not necessarily complementary
  // Nevermind, actually P = !N
  Matrix<bool,Dynamic,1> P(m,1),N(m,1);
  // loop over faces
  int mp = 0,mn = 0;
  for(int f = 0;f<m;f++)
  {
    const CGAL::Plane_3<CGAL::Epeck> plane(
      CGAL::Point_3<CGAL::Epeck>(VA(FA(f,0),0),VA(FA(f,0),1),VA(FA(f,0),2)),
      CGAL::Point_3<CGAL::Epeck>(VA(FA(f,1),0),VA(FA(f,1),1),VA(FA(f,1),2)),
      CGAL::Point_3<CGAL::Epeck>(VA(FA(f,2),0),VA(FA(f,2),1),VA(FA(f,2),2)));
    const auto normal = plane.orthogonal_vector();
    const auto dt = normal * v;
    if(dt > 0)
    {
      P(f) = true;
      N(f) = false;
      mp++;
    }else
    //}else if(dt < 0)
    {
      P(f) = false;
      N(f) = true;
      mn++;
    //}else
    //{
    //  P(f) = false;
    //  N(f) = false;
    }
  }

  typedef Matrix<typename DerivedG::Scalar,Dynamic,Dynamic> MatrixXI;
  typedef Matrix<typename DerivedG::Scalar,Dynamic,1> VectorXI;
  MatrixXI GT(mp+mn,3);
  GT<< slice_mask(FA,N,1), slice_mask((FA.array()+n).eval(),P,1);
  // J indexes FA for parts at s and m+FA for parts at d
  J.derived() = igl::LinSpaced<DerivedJ >(m,0,m-1);
  DerivedJ JT(mp+mn);
  JT << slice_mask(J,P,1), slice_mask(J,N,1);
  JT.block(mp,0,mn,1).array()+=m;

  // Original non-co-planar faces with positively oriented reversed
  MatrixXI BA(mp+mn,3);
  BA << slice_mask(FA,P,1).rowwise().reverse(), slice_mask(FA,N,1);
  // Quads along **all** sides
  MatrixXI GQ((mp+mn)*3,4);
  GQ<< 
    BA.col(1), BA.col(0), BA.col(0).array()+n, BA.col(1).array()+n,
    BA.col(2), BA.col(1), BA.col(1).array()+n, BA.col(2).array()+n,
    BA.col(0), BA.col(2), BA.col(2).array()+n, BA.col(0).array()+n;

  MatrixXI uGQ;
  VectorXI S,sI,sJ;
  // Inputs:
  //   F  #F by d list of polygons
  // Outputs:
  //   S  #uF list of signed incidences for each unique face
  //  uF  #uF by d list of unique faces
  //   I  #uF index vector so that uF = sort(F,2)(I,:)
  //   J  #F index vector so that sort(F,2) = uF(J,:)
  [](
      const MatrixXI & F,
      VectorXI & S,
      MatrixXI & uF,
      VectorXI & I,
      VectorXI & J)
  {
    const int m = F.rows();
    const int d = F.cols();
    MatrixXI sF = F;
    const auto MN = sF.rowwise().minCoeff().eval();
    // rotate until smallest index is first
    for(int p = 0;p<d;p++)
    {
      for(int f = 0;f<m;f++)
      {
        if(sF(f,0) != MN(f))
        {
          for(int r = 0;r<d-1;r++)
          {
            std::swap(sF(f,r),sF(f,r+1));
          }
        }
      }
    }
    // swap orienation so that last index is greater than first
    for(int f = 0;f<m;f++)
    {
      if(sF(f,d-1) < sF(f,1))
      {
        sF.block(f,1,1,d-1) = sF.block(f,1,1,d-1).reverse().eval();
      }
    }
    Matrix<bool,Dynamic,1> M = Matrix<bool,Dynamic,1>::Zero(m,1);
    {
      VectorXI P = igl::LinSpaced<VectorXI >(d,0,d-1);
      for(int p = 0;p<d;p++)
      {
        for(int f = 0;f<m;f++)
        {
          bool all = true;
          for(int r = 0;r<d;r++)
          {
            all = all && (sF(f,P(r)) == F(f,r));
          }
          M(f) = M(f) || all;
        }
        for(int r = 0;r<d-1;r++)
        {
          std::swap(P(r),P(r+1));
        }
      }
    }
    unique_rows(sF,uF,I,J);
    S = VectorXI::Zero(uF.rows(),1);
    assert(m == J.rows());
    for(int f = 0;f<m;f++)
    {
      S(J(f)) += M(f) ? 1 : -1;
    }
  }(MatrixXI(GQ),S,uGQ,sI,sJ);
  assert(S.rows() == uGQ.rows());
  const int nq = (S.array().abs()==2).count();
  GQ.resize(nq,4);
  {
    int k = 0;
    for(int q = 0;q<uGQ.rows();q++)
    {
      switch(S(q))
      {
        case -2:
          GQ.row(k++) = uGQ.row(q).reverse().eval();
          break;
        case 2:
          GQ.row(k++) = uGQ.row(q);
          break;
        default:
        // do not add
          break;
      }
    }
    assert(nq == k);
  }

  G.resize(GT.rows()+2*GQ.rows(),3);
  G<< 
    GT,
    GQ.col(0), GQ.col(1), GQ.col(2), 
    GQ.col(0), GQ.col(2), GQ.col(3);
  J.resize(JT.rows()+2*GQ.rows(),1);
  J<<JT,DerivedJ::Constant(2*GQ.rows(),1,2*m+1);
  if(resolve_overlaps)
  {
    Eigen::Matrix<typename DerivedJ::Scalar, Eigen::Dynamic,1> SJ;
    mesh_boolean(
      DerivedW(W),DerivedG(G),
      Matrix<typename DerivedVA::Scalar,Dynamic,Dynamic>(),MatrixXI(),
      MESH_BOOLEAN_TYPE_UNION,
      W,G,SJ);
    J.derived() = slice(DerivedJ(J),SJ,1);
  }
}

template <
  typename DerivedVA,
  typename DerivedFA,
  typename sType, int sCols, int sOptions,
  typename dType, int dCols, int dOptions,
  typename DerivedW,
  typename DerivedG,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::minkowski_sum(
  const Eigen::MatrixBase<DerivedVA> & VA,
  const Eigen::MatrixBase<DerivedFA> & FA,
  const Eigen::Matrix<sType,1,sCols,sOptions> & s,
  const Eigen::Matrix<dType,1,dCols,dOptions> & d,
  Eigen::PlainObjectBase<DerivedW> & W,
  Eigen::PlainObjectBase<DerivedG> & G,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  return minkowski_sum(VA,FA,s,d,true,W,G,J);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::copyleft::cgal::minkowski_sum<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, CGAL::Lazy_exact_nt<CGAL::Gmpq>, 3, 1, CGAL::Lazy_exact_nt<CGAL::Gmpq>, 3, 1, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, 1, 3, 1, 1, 3> const&, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, 1, 3, 1, 1, 3> const&, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::minkowski_sum<
  Eigen::Matrix<float, -1, 3, 1, -1, 3>, 
  Eigen::Matrix<int, -1, 3, 1, -1, 3>, 
  double, 3, 1, 
  float, 3, 1, 
  Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, 
  Eigen::Matrix<int, -1, -1, 0, -1, -1>, 
  Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
