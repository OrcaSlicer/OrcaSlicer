// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "eigs.h"

#include "cotmatrix.h"
#include "sort.h"
#include "slice.h"
#include "massmatrix.h"
#include <iostream>

template <
  typename Atype,
  typename Btype,
  typename DerivedU,
  typename DerivedS>
IGL_INLINE bool igl::eigs(
  const Eigen::SparseMatrix<Atype> & A,
  const Eigen::SparseMatrix<Btype> & iB,
  const size_t k,
  const EigsType type,
  Eigen::PlainObjectBase<DerivedU> & sU,
  Eigen::PlainObjectBase<DerivedS> & sS)
{
  using namespace Eigen;
  using namespace std;
  const size_t n = A.rows();
  assert(A.cols() == n && "A should be square.");
  assert(iB.rows() == n && "B should be match A's dims.");
  assert(iB.cols() == n && "B should be square.");
  assert(type == EIGS_TYPE_SM && "Only low frequencies are supported");
  DerivedU U(n,k);
  DerivedS S(k,1);
  typedef Atype Scalar;
  typedef Eigen::Matrix<typename DerivedU::Scalar,DerivedU::RowsAtCompileTime,1> VectorXS;
  // Rescale B for better numerics
  const Scalar rescale = std::abs(iB.diagonal().maxCoeff());
  const Eigen::SparseMatrix<Btype> B = iB/rescale;

  Scalar tol = 1e-4;
  Scalar conv = 1e-14;
  int max_iter = 100;
  int i = 0;
  //std::cout<<"start"<<std::endl;
  while(true)
  {
    //std::cout<<i<<std::endl;
    // Random initial guess
    VectorXS y = VectorXS::Random(n,1);
    Scalar eff_sigma = 0;
    if(i>0)
    {
      eff_sigma = 1e-8+std::abs(S(i-1));
    }
    // whether to use rayleigh quotient method
    bool ray = false;
    Scalar err = std::numeric_limits<Scalar>::infinity();
    int iter;
    Scalar sigma = std::numeric_limits<Scalar>::infinity();
    VectorXS x;
    for(iter = 0;iter<max_iter;iter++)
    {
      if(i>0 && !ray)
      {
        // project-out existing modes
        for(int j = 0;j<i;j++)
        {
          const VectorXS u = U.col(j);
          y = (y - u*u.dot(B*y)/u.dot(B * u)).eval();
        }
      }
      // normalize
      x = y/sqrt(y.dot(B*y));

      // current guess at eigen value
      sigma = x.dot(A*x)/x.dot(B*x);
      //x *= sigma>0?1.:-1.;

      Scalar err_prev = err;
      err = (A*x-sigma*B*x).array().abs().maxCoeff();
      if(err<conv)
      {
        break;
      }
      if(ray || err<tol)
      {
        eff_sigma = sigma;
        ray = true;
      }

      Scalar tikhonov = std::abs(eff_sigma)<1e-12?1e-10:0;
      switch(type)
      {
        default:
          assert(false && "Not supported");
          break;
        case EIGS_TYPE_SM:
        {
          SimplicialLDLT<SparseMatrix<Scalar> > solver;
          const SparseMatrix<Scalar> C = A-eff_sigma*B+tikhonov*B;
          //mw.save(C,"C");
          //mw.save(eff_sigma,"eff_sigma");
          //mw.save(tikhonov,"tikhonov");
          solver.compute(C);
          switch(solver.info())
          {
            case Eigen::Success:
              break;
            case Eigen::NumericalIssue:
              cerr<<"Error: Numerical issue."<<endl;
              return false;
            default:
              cerr<<"Error: Other."<<endl;
              return false;
          }
          const VectorXS rhs = B*x;
          y = solver.solve(rhs);
          //mw.save(rhs,"rhs");
          //mw.save(y,"y");
          //mw.save(x,"x");
          //mw.write("eigs.mat");
          //if(i == 1)
          //return false;
          break;
        }
      }
    }
    if(iter == max_iter)
    {
      cerr<<"Failed to converge."<<endl;
      return false;
    }
    if(
      i==0 || 
      (S.head(i).array()-sigma).abs().maxCoeff()>1e-14 ||
      ((U.leftCols(i).transpose()*B*x).array().abs()<=1e-7).all()
      )
    {
      //cout<<"Found "<<i<<"th mode"<<endl;
      U.col(i) = x;
      S(i) = sigma;
      i++;
      if(i == k)
      {
        break;
      }
    }else
    {
      //std::cout<<"i: "<<i<<std::endl;
      //std::cout<<"  "<<S.head(i).transpose()<<" << "<<sigma<<std::endl;
      //std::cout<<"  "<<(S.head(i).array()-sigma).abs().maxCoeff()<<std::endl;
      //std::cout<<"  "<<(U.leftCols(i).transpose()*B*x).array().abs().transpose()<<std::endl;
      // restart with new random guess.
      cout<<"igl::eigs RESTART"<<endl;
    }
  }
  // finally sort
  VectorXi I;
  igl::sort(S,1,false,sS,I);
  igl::slice(U,I,2,sU);
  sS /= rescale;
  sU /= sqrt(rescale);
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::eigs<double, double, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::SparseMatrix<double, 0, int> const&, const size_t, igl::EigsType, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#ifdef WIN32
template bool igl::eigs<double, double, Eigen::Matrix<double,-1,-1,0,-1,-1>, Eigen::Matrix<double,-1,1,0,-1,1> >(Eigen::SparseMatrix<double,0,int> const &,Eigen::SparseMatrix<double,0,int> const &, const size_t, igl::EigsType, Eigen::PlainObjectBase< Eigen::Matrix<double,-1,-1,0,-1,-1> > &, Eigen::PlainObjectBase<Eigen::Matrix<double,-1,1,0,-1,1> > &);
#endif
#endif
