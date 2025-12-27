// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "remesh_along_isoline.h"
#include "list_to_matrix.h"

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedS,
  typename DerivedU,
  typename DerivedG,
  typename DerivedJ,
  typename BCtype,
  typename DerivedSU,
  typename DerivedL>
  IGL_INLINE void igl::remesh_along_isoline(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedS> & S,
    const typename DerivedS::Scalar val,
    Eigen::PlainObjectBase<DerivedU> & U,
    Eigen::PlainObjectBase<DerivedG> & G,
    Eigen::PlainObjectBase<DerivedSU> & SU,
    Eigen::PlainObjectBase<DerivedJ> & J,
    Eigen::SparseMatrix<BCtype> & BC,
    Eigen::PlainObjectBase<DerivedL> & L)
{
  igl::remesh_along_isoline(V.rows(),F,S,val,G,SU,J,BC,L);
  U = BC * V;
}

template <
  typename DerivedF,
  typename DerivedS,
  typename DerivedG,
  typename DerivedJ,
  typename BCtype,
  typename DerivedSU,
  typename DerivedL>
  IGL_INLINE void igl::remesh_along_isoline(
    const int num_vertices,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedS> & S,
    const typename DerivedS::Scalar val,
    Eigen::PlainObjectBase<DerivedG> & G,
    Eigen::PlainObjectBase<DerivedSU> & SU,
    Eigen::PlainObjectBase<DerivedJ> & J,
    Eigen::SparseMatrix<BCtype> & BC,
    Eigen::PlainObjectBase<DerivedL> & L)
{
  // Lazy implementation using vectors

  //assert(val.size() == 1);
  const int isoval_i = 0;
  //auto isoval = val(isoval_i);
  auto isoval = val;
  std::vector<std::vector<typename DerivedG::Scalar> > vG;
  std::vector<typename DerivedJ::Scalar> vJ;
  std::vector<typename DerivedL::Scalar> vL;
  std::vector<Eigen::Triplet<BCtype> > vBC;
  int Ucount = 0;
  for(int i = 0;i<num_vertices;i++)
  {
    vBC.emplace_back(Ucount,i,1.0);
    Ucount++;
  }

  // Loop over each face
  for(int f = 0;f<F.rows();f++)
  {
    bool Psign[2];
    int P[2];
    int count = 0;
    for(int p = 0;p<3;p++)
    {
      const bool psign = S(F(f,p)) > isoval;
      // Find crossings
      const int n = (p+1)%3;
      const bool nsign = S(F(f,n)) > isoval;
      if(psign != nsign)
      {
        P[count] = p;
        Psign[count] = psign;
        // record crossing
        count++;
      }
    }

    assert(count == 0  || count == 2);
    switch(count)
    {
      case 0:
      {
        // Easy case
        std::vector<typename DerivedG::Scalar> row = {F(f,0),F(f,1),F(f,2)};
        vG.push_back(row);
        vJ.push_back(f);
        vL.push_back( S(F(f,0))>isoval ? isoval_i+1 : isoval_i );
        break;
      }
      case 2:
      {
        // Cut case
        // flip so that P[1] is the one-off vertex
        if(P[0] == 0 && P[1] == 2)
        {
          std::swap(P[0],P[1]);
          std::swap(Psign[0],Psign[1]);
        }
        assert(Psign[0] != Psign[1]);
        // Create two new vertices
        for(int i = 0;i<2;i++)
        {
          const double bci = (isoval - S(F(f,(P[i]+1)%3)))/
            (S(F(f,P[i]))-S(F(f,(P[i]+1)%3)));
          vBC.emplace_back(Ucount,F(f,P[i]),bci);
          vBC.emplace_back(Ucount,F(f,(P[i]+1)%3),1.0-bci);
          Ucount++;
        }
        const int v0 = F(f,P[0]);
        const int v01 = Ucount-2;
        assert(((P[0]+1)%3) == P[1]);
        const int v1 = F(f,P[1]);
        const int v12 = Ucount-1;
        const int v2 = F(f,(P[1]+1)%3);
        // v0
        // |  \
        // |   \
        // |    \
        // v01   \
        // |      \
        // |       \
        // |        \
        // v1--v12---v2
        typedef std::vector<typename DerivedG::Scalar> Row;
        {Row row = {v01,v1,v12}; vG.push_back(row);vJ.push_back(f);vL.push_back(Psign[0]?isoval_i:isoval_i+1);}
        {Row row = {v12,v2,v01}; vG.push_back(row);vJ.push_back(f);vL.push_back(Psign[1]?isoval_i:isoval_i+1);}
        {Row row = {v2,v0,v01}; vG.push_back(row) ;vJ.push_back(f);vL.push_back(Psign[1]?isoval_i:isoval_i+1);}
        break;
      }
      default: assert(false);
    }
  }
  igl::list_to_matrix(vG,G);
  igl::list_to_matrix(vJ,J);
  igl::list_to_matrix(vL,L);
  BC.resize(Ucount,num_vertices);
  BC.setFromTriplets(vBC.begin(),vBC.end());
  SU = BC * S;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::remesh_along_isoline<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, double, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<double, -1, 1, 0, -1, 1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::SparseMatrix<double, 0, int>&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
