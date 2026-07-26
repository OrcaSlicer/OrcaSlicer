// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "cotmatrix.h"
#include <vector>

// For error printing
#include <cstdio>
#include "cotmatrix_entries.h"

// Bug in unsupported/Eigen/SparseExtra needs iostream first
#include <iostream>

template <typename DerivedV, typename DerivedF, typename Scalar>
IGL_INLINE void igl::cotmatrix(
  const Eigen::MatrixBase<DerivedV> & V, 
  const Eigen::MatrixBase<DerivedF> & F, 
  Eigen::SparseMatrix<Scalar>& L)
{
  using namespace Eigen;
  using namespace std;

  L.resize(V.rows(),V.rows());
  Matrix<int,Dynamic,2> edges;
  int simplex_size = F.cols();
  // 3 for triangles, 4 for tets
  assert(simplex_size == 3 || simplex_size == 4);
  if(simplex_size == 3)
  {
    // This is important! it could decrease the comptuation time by a factor of 2
    // Laplacian for a closed 2d manifold mesh will have on average 7 entries per
    // row
    L.reserve(10*V.rows());
    edges.resize(3,2);
    edges << 
      1,2,
      2,0,
      0,1;
  }else if(simplex_size == 4)
  {
    L.reserve(17*V.rows());
    edges.resize(6,2);
    edges << 
      1,2,
      2,0,
      0,1,
      3,0,
      3,1,
      3,2;
  }else
  {
    return;
  }
  // Gather cotangents
  Matrix<Scalar,Dynamic,Dynamic> C;
  cotmatrix_entries(V,F,C);
  
  vector<Triplet<Scalar> > IJV;
  IJV.reserve(F.rows()*edges.rows()*4);
  // Loop over triangles
  for(int i = 0; i < F.rows(); i++)
  {
    // loop over edges of element
    for(int e = 0;e<edges.rows();e++)
    {
      int source = F(i,edges(e,0));
      int dest = F(i,edges(e,1));
      IJV.push_back(Triplet<Scalar>(source,dest,C(i,e)));
      IJV.push_back(Triplet<Scalar>(dest,source,C(i,e)));
      IJV.push_back(Triplet<Scalar>(source,source,-C(i,e)));
      IJV.push_back(Triplet<Scalar>(dest,dest,-C(i,e)));
    }
  }
  L.setFromTriplets(IJV.begin(),IJV.end());
}

#include "massmatrix.h"
#include "cotmatrix_entries.h"
#include "massmatrix.h"
#include <Eigen/Geometry>
#include <Eigen/QR>

template <
  typename DerivedV, 
  typename DerivedI, 
  typename DerivedC, 
  typename Scalar>
IGL_INLINE void igl::cotmatrix(
  const Eigen::MatrixBase<DerivedV> & V, 
  const Eigen::MatrixBase<DerivedI> & I, 
  const Eigen::MatrixBase<DerivedC> & C, 
  Eigen::SparseMatrix<Scalar>& L,
  Eigen::SparseMatrix<Scalar>& M,
  Eigen::SparseMatrix<Scalar>& P)
{
  typedef Eigen::Matrix<Scalar,1,3> RowVector3S;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> MatrixXS;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,1> VectorXS;
  typedef Eigen::Index Index;
  // number of vertices
  const Index n = V.rows();
  // number of polyfaces
  const Index m = C.size()-1;
  assert(V.cols() == 2 || V.cols() == 3);
  std::vector<Eigen::Triplet<Scalar> > Lfijv;
  std::vector<Eigen::Triplet<Scalar> > Mfijv;
  std::vector<Eigen::Triplet<Scalar> > Pijv;
  // loop over vertices; set identity for original vertices
  for(Index i = 0;i<V.rows();i++) { Pijv.emplace_back(i,i,1); }
  // loop over faces
  for(Index p = 0;p<C.size()-1;p++)
  {
    // number of faces/vertices in this simple polygon
    const Index np = C(p+1)-C(p);
    // Working "local" list of vertices; last vertex is new one
    // this needs to have 3 columns so Eigen doesn't complain about cross
    // products below.
    Eigen::Matrix<Scalar,Eigen::Dynamic,3> X = decltype(X)::Zero(np+1,3);
    for(Index i = 0;i<np;i++){ X.row(i).head(V.cols()) = V.row(I(C(p)+i)); };
    // determine weights definig position of inserted vertex
    {
      MatrixXS A = decltype(A)::Zero(np+1,np);
      // My equation (38) would be A w = b.
      VectorXS b = decltype(b)::Zero(np+1);
      for(Index k = 0;k<np;k++)
      { 
        const RowVector3S Xkp1mk = X.row((k+1)%np)-X.row(k);
        const RowVector3S Xkp1mkck = Xkp1mk.cross(X.row(k));
        for(Index i = 0;i<np;i++)
        { 
          b(i) -= 2.*(X.row(i).cross(Xkp1mk)).dot(Xkp1mkck);
          for(Index j = 0;j<np;j++)
          { 
            A(i,j) += 2.*(X.row(j).cross(Xkp1mk)).dot(X.row(i).cross(Xkp1mk));
          }
        }
      }
      A.row(np).setConstant(1);
      b(np) = 1;
      const VectorXS w =
        Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd>(A).solve(b);
      X.row(np) = w.transpose()*X.topRows(np);
      // scatter w into new row of P
      for(Index i = 0;i<np;i++) { Pijv.emplace_back(n+p,I(C(p)+i),w(i)); }
    }
    // "local" fan of faces. These could be statically cached, but this will
    // not be the bottleneck.
    Eigen::MatrixXi F(np,3);
    for(Index i = 0;i<np;i++)
    { 
      F(i,0) = i; 
      F(i,1) = (i+1)%np; 
      F(i,2) = np; 
    }
    // Cotangent contributions
    MatrixXS K;
    igl::cotmatrix_entries(X,F,K);
    // Massmatrix entried
    VectorXS Mp;
    {
      Eigen::SparseMatrix<Scalar> M;
      igl::massmatrix(X,F,igl::MASSMATRIX_TYPE_DEFAULT,M);
      Mp = M.diagonal();
    }
    // Scatter into fine Laplacian and mass matrices
    const auto J = [&n,&np,&p,&I,&C](Index i)->Index{return i==np?n+p:I(C(p)+i);};
    // Should just build Mf as a vector...
    for(Index i = 0;i<np+1;i++) { Mfijv.emplace_back(J(i),J(i),Mp(i)); }
    // loop over faces
    for(Index f = 0;f<np;f++)
    {
      for(Index c = 0;c<3;c++)
      {
        const Index i = F(f,(c+1)%3);
        const Index j = F(f,(c+2)%3);
        // symmetric off-diagonal
        Lfijv.emplace_back(J(i),J(j),K(f,c));
        Lfijv.emplace_back(J(j),J(i),K(f,c));
        // diagonal
        Lfijv.emplace_back(J(i),J(i),-K(f,c));
        Lfijv.emplace_back(J(j),J(j),-K(f,c));
      }
    }
  }
  P.resize(n+m,n);
  P.setFromTriplets(Pijv.begin(),Pijv.end());
  Eigen::SparseMatrix<Scalar> Lf(n+m,n+m);
  Lf.setFromTriplets(Lfijv.begin(),Lfijv.end());
  Eigen::SparseMatrix<Scalar> Mf(n+m,n+m);
  Mf.setFromTriplets(Mfijv.begin(),Mfijv.end());
  L = P.transpose() * Lf * P;
  // "unlumped" M
  const Eigen::SparseMatrix<Scalar> PTMP = P.transpose() * Mf * P;
  // Lump M
  const VectorXS Mdiag = PTMP * VectorXS::Ones(n,1);
  M = Eigen::SparseMatrix<Scalar>(Mdiag.asDiagonal());

  MatrixXS Vf = P*V;
  Eigen::MatrixXi Ff(I.size(),3);
  {
    Index f = 0;
    for(Index p = 0;p<C.size()-1;p++)
    {
      const Index np = C(p+1)-C(p);
      for(Index c = 0;c<np;c++)
      {
        Ff(f,0) = I(C(p)+c);
        Ff(f,1) = I(C(p)+(c+1)%np);
        Ff(f,2) = V.rows()+p;
        f++;
      }
    }
    assert(f == Ff.rows());
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::cotmatrix<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::SparseMatrix<double, 0, int>&, Eigen::SparseMatrix<double, 0, int>&, Eigen::SparseMatrix<double, 0, int>&);
// generated by autoexplicit.sh
template void igl::cotmatrix<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
template void igl::cotmatrix<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 4, 0, -1, 4>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 4, 0, -1, 4> > const&, Eigen::SparseMatrix<double, 0, int>&);
template void igl::cotmatrix<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::SparseMatrix<double, 0, int>&);
template void igl::cotmatrix<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
