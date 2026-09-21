// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Hanxiao Shen <hanxiao@cims.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "cut_mesh.h"
#include "triangle_triangle_adjacency.h"
#include "HalfEdgeIterator.h"
#include "is_border_vertex.h"

// wrapper for input/output style
template <typename DerivedV, typename DerivedF, typename DerivedC>
IGL_INLINE void igl::cut_mesh(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedC>& C,
  Eigen::PlainObjectBase<DerivedV>& Vn,
  Eigen::PlainObjectBase<DerivedF>& Fn
){
  Vn = V;
  Fn = F;
  typedef typename DerivedF::Scalar Index;
  Eigen::Matrix<Index,Eigen::Dynamic,1> _I;
  cut_mesh(Vn,Fn,C,_I);
}

template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedC, 
  typename DerivedVn,
  typename DerivedFn,
  typename DerivedI>
IGL_INLINE void igl::cut_mesh(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedC>& C,
  Eigen::PlainObjectBase<DerivedVn>& Vn,
  Eigen::PlainObjectBase<DerivedFn>& Fn,
  Eigen::PlainObjectBase<DerivedI>& I
){
  static_assert(std::is_same<typename DerivedV::Scalar, typename DerivedVn::Scalar>::value, "Scalar types of V and Vn must match");
  static_assert(std::is_same<typename DerivedF::Scalar, typename DerivedFn::Scalar>::value, "Scalar types of F and Fn must match");
  Vn = V;
  Fn = F;
  cut_mesh(Vn,Fn,C,I);
}

template <typename DerivedV, typename DerivedF, typename DerivedC, typename DerivedI>
IGL_INLINE void igl::cut_mesh(
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedC>& C,
  Eigen::PlainObjectBase<DerivedI>& I
){
  DerivedF FF, FFi;
  igl::triangle_triangle_adjacency(F,FF,FFi);
  igl::cut_mesh(V,F,FF,FFi,C,I);
}

template <typename DerivedV, typename DerivedF, typename DerivedFF, typename DerivedFFi, typename DerivedC, typename DerivedI>
IGL_INLINE void igl::cut_mesh(
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::MatrixBase<DerivedFF>& FF,
  Eigen::MatrixBase<DerivedFFi>& FFi,
  const Eigen::MatrixBase<DerivedC>& C,
  Eigen::PlainObjectBase<DerivedI>& I
){

  typedef typename DerivedF::Scalar Index;

  // store current number of occurance of each vertex as the alg proceed
  Eigen::Matrix<Index,Eigen::Dynamic,1> occurence(V.rows());
  occurence.setConstant(1);
  
  // set eventual number of occurance of each vertex expected
  Eigen::Matrix<Index,Eigen::Dynamic,1> eventual(V.rows());
  eventual.setZero();
  for(Index i=0;i<F.rows();i++){
    for(Index k=0;k<3;k++){
      Index u = F(i,k);
      Index v = F(i,(k+1)%3);
      if(FF(i,k) == -1){ 
        // add one extra occurance for boundary vertices
        eventual(u) += 1;
      }else if( 
          (u < v) && 
          (C(i,k) || C(FF(i,k),FFi(i,k))) )
      { 
        // only compute every (undirected) edge ones
        eventual(u) += 1;
        eventual(v) += 1;
      }
    }
  }
  
  // original number of vertices
  Index n_v = V.rows(); 
  
  // estimate number of new vertices and resize V
  Index n_new = 0;
  for(Index i=0;i<eventual.rows();i++)
    n_new += ((eventual(i) > 0) ? eventual(i)-1 : 0);
  V.conservativeResize(n_v+n_new,Eigen::NoChange);
  I = DerivedI::LinSpaced(V.rows(),0,V.rows());
  
  // pointing to the current bottom of V
  Index pos = n_v;
  for(Index f=0;f<C.rows();f++){
    for(Index k=0;k<3;k++){
      Index v0 = F(f,k);
      if(F(f,k) >= n_v) continue; // ignore new vertices
      if(C(f,k) == 1 && occurence(v0) != eventual(v0)){
        igl::HalfEdgeIterator<DerivedF,DerivedF,DerivedF> he(F,FF,FFi,f,k);

        // rotate clock-wise around v0 until hit another cut
        std::vector<Index> fan;
        Index fi = he.Fi();
        Index ei = he.Ei();
        do{
          fan.push_back(fi);
          he.flipE();
          he.flipF();
          fi = he.Fi();
          ei = he.Ei();
        }while(C(fi,ei) == 0 && !he.isBorder());
        
        // make a copy
        V.row(pos) << V.row(v0);
        I(pos) = v0;
        // add one occurance to v0
        occurence(v0) += 1;
        
        // replace old v0
        for(Index f0: fan)
          for(Index j=0;j<3;j++)
            if(F(f0,j) == v0)
              F(f0,j) = pos;
        
        // mark cuts as boundary
        FF(f,k) = -1;
        FF(fi,ei) = -1;
        
        pos++;
      }
    }
  }
  
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::cut_mesh<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::cut_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cut_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cut_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
