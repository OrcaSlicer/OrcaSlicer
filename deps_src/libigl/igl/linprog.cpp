// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "linprog.h"
#include "slice.h"
#include "slice_into.h"
#include "find.h"
#include "colon.h"
#include <iostream>

//#define IGL_LINPROG_VERBOSE
IGL_INLINE bool igl::linprog(
  const Eigen::VectorXd & c,
  const Eigen::MatrixXd & _A,
  const Eigen::VectorXd & b,
  const int k,
  Eigen::VectorXd & x)
{
  // This is a very literal translation of
  // http://www.mathworks.com/matlabcentral/fileexchange/2166-introduction-to-linear-algebra/content/strang/linprog.m
  using namespace Eigen;
  using namespace std;
  bool success = true;
  // number of constraints
  const int m = _A.rows();
  // number of original variables
  const int n = _A.cols();
  // number of iterations
  int it = 0;
  // maximum number of iterations
  //const int MAXIT = 10*m;
  const int MAXIT = 100*m;
  // residual tolerance
  const double tol = 1e-10;
  const auto & sign = [](const Eigen::VectorXd & B) -> Eigen::VectorXd
  {
    Eigen::VectorXd Bsign(B.size());
    for(int i = 0;i<B.size();i++)
    {
      Bsign(i) = B(i)>0?1:(B(i)<0?-1:0);
    }
    return Bsign;
  };
  // initial (inverse) basis matrix
  VectorXd Dv = sign(sign(b).array()+0.5);
  Dv.head(k).setConstant(1.);
  MatrixXd D = Dv.asDiagonal();
  // Incorporate slack variables
  MatrixXd A(_A.rows(),_A.cols()+D.cols());
  A<<_A,D;
  // Initial basis
  VectorXi B = igl::colon<int>(n,n+m-1);
  // non-basis, may turn out that vector<> would be better here
  VectorXi N = igl::colon<int>(0,n-1);
  int j;
  double bmin = b.minCoeff(&j);
  int phase;
  VectorXd xb;
  VectorXd s;
  VectorXi J;
  if(k>0 && bmin<0)
  {
    phase = 1;
    xb = VectorXd::Ones(m);
    // super cost
    s.resize(n+m+1);
    s<<VectorXd::Zero(n+k),VectorXd::Ones(m-k+1);
    N.resize(n+1);
    N<<igl::colon<int>(0,n-1),B(j);
    J.resize(B.size()-1);
    // [0 1 2 3 4]
    //      ^
    // [0 1]
    //      [3 4]
    J.head(j) = B.head(j);
    J.tail(B.size()-j-1) = B.tail(B.size()-j-1);
    B(j) = n+m;
    MatrixXd AJ;
    igl::slice(A,J,2,AJ);
    const VectorXd a = b - AJ.rowwise().sum();
    {
      MatrixXd old_A = A;
      A.resize(A.rows(),A.cols()+a.cols());
      A<<old_A,a;
    }
    D.col(j) = -a/a(j);
    D(j,j) = 1./a(j);
  }else if(k==m)
  {
    phase = 2;
    xb = b;
    s.resize(c.size()+m);
    // cost function
    s<<c,VectorXd::Zero(m);
  }else //k = 0 or bmin >=0
  {
    phase = 1;
    xb = b.array().abs();
    s.resize(n+m);
    // super cost
    s<<VectorXd::Zero(n+k),VectorXd::Ones(m-k);
  }
  while(phase<3)
  {
    double df = -1;
    int t = std::numeric_limits<int>::max();
    // Lagrange mutipliers fro Ax=b
    VectorXd yb = D.transpose() * igl::slice(s,B);
    while(true)
    {
      if(MAXIT>0 && it>=MAXIT)
      {
#ifdef IGL_LINPROG_VERBOSE
        cerr<<"linprog: warning! maximum iterations without convergence."<<endl;
#endif
        success = false;
        break;
      }
      // no freedom for minimization
      if(N.size() == 0)
      {
        break;
      }
      // reduced costs
      VectorXd sN = igl::slice(s,N);
      MatrixXd AN = igl::slice(A,N,2);
      VectorXd r = sN - AN.transpose() * yb;
      int q;
      // determine new basic variable
      double rmin = r.minCoeff(&q);
      // optimal! infinity norm
      if(rmin>=-tol*(sN.array().abs().maxCoeff()+1))
      {
        break;
      }
      // increment iteration count
      it++;
      // apply Bland's rule to avoid cycling
      if(df>=0)
      {
        if(MAXIT == -1)
        {
#ifdef IGL_LINPROG_VERBOSE
          cerr<<"linprog: warning! degenerate vertex"<<endl;
#endif
          success = false;
        }
        igl::find((r.array()<0).eval(),J);
        double Nq = igl::slice(N,J).minCoeff();
        // again seems like q is assumed to be a scalar though matlab code
        // could produce a vector for multiple matches
        (N.array()==Nq).cast<int>().maxCoeff(&q);
      }
      VectorXd d = D*A.col(N(q));
      VectorXi I;
      igl::find((d.array()>tol).eval(),I);
      if(I.size() == 0)
      {
#ifdef IGL_LINPROG_VERBOSE
        cerr<<"linprog: warning! solution is unbounded"<<endl;
#endif
        // This seems dubious:
        it=-it;
        success = false;
        break;
      }
      VectorXd xbd = igl::slice(xb,I).array()/igl::slice(d,I).array();
      // new use of r
      int p;
      {
        double r;
        r = xbd.minCoeff(&p);
        p = I(p);
        // apply Bland's rule to avoid cycling
        if(df>=0)
        {
          igl::find((xbd.array()==r).eval(),J);
          double Bp = igl::slice(B,igl::slice(I,J)).minCoeff();
          // idiotic way of finding index in B of Bp
          // code down the line seems to assume p is a scalar though the matlab
          // code could find a vector of matches)
          (B.array()==Bp).cast<int>().maxCoeff(&p);
        }
        // update x
        xb -= r*d;
        xb(p) = r;
        // change in f
        df = r*rmin;
      }
      // row vector
      RowVectorXd v = D.row(p)/d(p);
      yb += v.transpose() * (s(N(q)) - d.transpose()*igl::slice(s,B));
      d(p)-=1;
      // update inverse basis matrix
      D = D - d*v;
      t = B(p);
      B(p) = N(q);
      if(t>(n+k-1))
      {
        // remove qth entry from N
        VectorXi old_N = N;
        N.resize(N.size()-1);
        N.head(q) = old_N.head(q);
        N.head(q) = old_N.head(q);
        N.tail(old_N.size()-q-1) = old_N.tail(old_N.size()-q-1);
      }else
      {
        N(q) = t;
      }
    }
    // iterative refinement
    xb = (xb+D*(b-igl::slice(A,B,2)*xb)).eval();
    // must be due to rounding
    VectorXi I;
    igl::find((xb.array()<0).eval(),I);
    if(I.size()>0)
    {
      // so correct
      VectorXd Z = VectorXd::Zero(I.size(),1);
      igl::slice_into(Z,I,xb);
    }
    // B, xb,n,m,res=A(:,B)*xb-b
    if(phase == 2 || it<0)
    {
      break;
    }
    if(xb.transpose()*igl::slice(s,B) > tol)
    {
      it = -it;
#ifdef IGL_LINPROG_VERBOSE
      cerr<<"linprog: warning, no feasible solution"<<endl;
#endif
      success = false;
      break;
    }
    // re-initialize for Phase 2
    phase = phase+1;
    s*=1e6*c.array().abs().maxCoeff();
    s.head(n) = c;
  }
  x.resize(std::max(B.maxCoeff()+1,n));
  igl::slice_into(xb,B,x);
  x = x.head(n).eval();
  return success;
}

IGL_INLINE bool igl::linprog(
  const Eigen::VectorXd & f,
  const Eigen::MatrixXd & A,
  const Eigen::VectorXd & b,
  const Eigen::MatrixXd & B,
  const Eigen::VectorXd & c,
  Eigen::VectorXd & x)
{
  using namespace Eigen;
  using namespace std;
  const int m = A.rows();
  const int n = A.cols();
  const int p = B.rows();
  MatrixXd Im = MatrixXd::Identity(m,m);
  MatrixXd AS(m,n+m);
  AS<<A,Im;
  MatrixXd bS = b.array().abs();
  for(int i = 0;i<m;i++)
  {
    const auto & sign = [](double x)->double
    {
      return (x<0?-1:(x>0?1:0));
    };
    AS.row(i) *= sign(b(i));
  }
  MatrixXd In = MatrixXd::Identity(n,n);
  MatrixXd P(n+m,2*n+m);
  P<<              In, -In, MatrixXd::Zero(n,m),
     MatrixXd::Zero(m,2*n), Im;
  MatrixXd ASP = AS*P;
  MatrixXd BSP(0,2*n+m);
  if(p>0)
  {
    MatrixXd BS(p,2*n);
    BS<<B,MatrixXd::Zero(p,n);
    BSP = BS*P;
  }

  VectorXd fSP = VectorXd::Ones(2*n+m);
  fSP.head(2*n) = P.block(0,0,n,2*n).transpose()*f;
  const VectorXd & cc = fSP;

  MatrixXd AA(m+p,2*n+m);
  AA<<ASP,BSP;
  VectorXd bb(m+p);
  bb<<bS,c;

  VectorXd xxs;
  bool ret = linprog(cc,AA,bb,0,xxs);
  x = P.block(0,0,n,2*n+m)*xxs;
  return ret;
}
