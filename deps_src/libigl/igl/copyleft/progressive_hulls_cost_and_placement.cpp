// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "progressive_hulls_cost_and_placement.h"
#include "quadprog.h"
#include "../unique.h"
#include "../circulation.h"
#include <cassert>
#include <vector>
#include <limits>

IGL_INLINE void igl::copyleft::progressive_hulls_cost_and_placement(
  const int e,
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXi & E,
  const Eigen::VectorXi & EMAP,
  const Eigen::MatrixXi & EF,
  const Eigen::MatrixXi & EI,
  double & cost,
  Eigen::RowVectorXd & p)
{
  using namespace Eigen;
  using namespace std;
  // Controls the amount of quadratic energy to add (too small will introduce
  // instabilities and flaps)
  const double w = 0.1;

  assert(V.cols() == 3 && "V.cols() should be 3");
  // Gather list of unique face neighbors
  vector<int> Nall =  circulation(e, true,EMAP,EF,EI);
  vector<int> Nother= circulation(e,false,EMAP,EF,EI);
  Nall.insert(Nall.end(),Nother.begin(),Nother.end());
  vector<int> N;
  igl::unique(Nall,N);
  // Gather:
  //   A  #N by 3 normals scaled by area,
  //   D  #N determinants of matrix formed by points as columns
  //   B  #N point on plane dot normal
  MatrixXd A(N.size(),3);
  VectorXd D(N.size());
  VectorXd B(N.size());
  //cout<<"N=[";
  for(int i = 0;i<N.size();i++)
  {
    const int f = N[i];
    //cout<<(f+1)<<" ";
    const RowVector3d & v01 = V.row(F(f,1))-V.row(F(f,0));
    const RowVector3d & v20 = V.row(F(f,2))-V.row(F(f,0));
    A.row(i) = v01.cross(v20);
    B(i) = V.row(F(f,0)).dot(A.row(i));
    D(i) = 
      (Matrix3d()<< V.row(F(f,0)), V.row(F(f,1)), V.row(F(f,2)))
      .finished().determinant();
  }
  //cout<<"];"<<endl;
  // linear objective
  Vector3d f = A.colwise().sum().transpose();
  VectorXd x;
  //bool success = linprog(f,-A,-B,MatrixXd(0,A.cols()),VectorXd(0,1),x);
  //VectorXd _;
  //bool success = mosek_linprog(f,A.sparseView(),B,_,_,_,env,x);
  //if(success)
  //{
  //  cost  = (1./6.)*(x.dot(f) - D.sum());
  //}
  bool success = false;
  {
    RowVectorXd mid = 0.5*(V.row(E(e,0))+V.row(E(e,1)));
    MatrixXd G =  w*Matrix3d::Identity(3,3);
    VectorXd g0 = (1.-w)*f - w*mid.transpose();
    const int n = A.cols();
    success = quadprog(
        G,g0,
        MatrixXd(n,0),VectorXd(0,1),
        A.transpose(),-B,x);
    cost  = (1.-w)*(1./6.)*(x.dot(f) - D.sum()) + 
      w*(x.transpose()-mid).squaredNorm() +
      w*(V.row(E(e,0))-V.row(E(e,1))).norm();
  }

  // A x >= B
  // A x - B >=0
  // This is annoyingly necessary. Seems the solver is letting some garbage
  // slip by.
  success = success && ((A*x-B).minCoeff()>-1e-10);
  if(success)
  {
    p = x.transpose();
    //assert(cost>=0 && "Cost should be positive");
  }else
  {
    cost = std::numeric_limits<double>::infinity();
    //VectorXi NM;
    //igl::list_to_matrix(N,NM);
    //cout<<matlab_format((NM.array()+1).eval(),"N")<<endl;
    //cout<<matlab_format(f,"f")<<endl;
    //cout<<matlab_format(A,"A")<<endl;
    //cout<<matlab_format(B,"B")<<endl;
    //exit(-1);
    p = RowVectorXd::Constant(1,3,std::nan("inf-cost"));
  }
}
