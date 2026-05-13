// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#pragma once

#include "min_quad_with_fixed.h"

#include "slice.h"
#include "is_symmetric.h"
#include "find.h"
#include "sparse.h"
#include "repmat.h"
#include "EPS.h"
#include "cat.h"
#include "placeholders.h"

//#include <Eigen/SparseExtra>
// Bug in unsupported/Eigen/SparseExtra needs iostream first
#include <iostream>
#include <unsupported/Eigen/SparseExtra>
#include <cassert>
#include <cstdio>
#include "matlab_format.h"
#include <type_traits>

template <typename T, typename Derivedknown>
IGL_INLINE bool igl::min_quad_with_fixed_precompute(
  const Eigen::SparseMatrix<T>& A2,
  const Eigen::MatrixBase<Derivedknown> & known,
  const Eigen::SparseMatrix<T>& Aeq,
  const bool pd,
  min_quad_with_fixed_data<T> & data
  )
{
//#define MIN_QUAD_WITH_FIXED_CPP_DEBUG
  using namespace Eigen;
  using namespace std;
  const Eigen::SparseMatrix<T> A = 0.5*A2;
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
  cout<<"    pre"<<endl;
#endif
  // number of rows
  int n = A.rows();
  // cache problem size
  data.n = n;

  int neq = Aeq.rows();
  // default is to have 0 linear equality constraints
  if(Aeq.size() != 0)
  {
    assert(n == Aeq.cols() && "#Aeq.cols() should match A.rows()");
  }

  assert(known.cols() == 1 && "known should be a vector");
  assert(A.rows() == n && "A should be square");
  assert(A.cols() == n && "A should be square");

  // number of known rows
  int kr = known.size();

  assert((kr == 0 || known.minCoeff() >= 0)&& "known indices should be in [0,n)");
  assert((kr == 0 || known.maxCoeff() < n) && "known indices should be in [0,n)");
  assert(neq <= n && "Number of equality constraints should be less than DOFs");


  // cache known
  // FIXME: This is *NOT* generic and introduces a copy.
  data.known = known.template cast<int>();

  // get list of unknown indices
  data.unknown.resize(n-kr);
  std::vector<bool> unknown_mask;
  unknown_mask.resize(n,true);
  for(int i = 0;i<kr;i++)
  {
    unknown_mask[known(i, 0)] = false;
  }
  int u = 0;
  for(int i = 0;i<n;i++)
  {
    if(unknown_mask[i])
    {
      data.unknown(u) = i;
      u++;
    }
  }
  // get list of lagrange multiplier indices
  data.lagrange.resize(neq);
  for(int i = 0;i<neq;i++)
  {
    data.lagrange(i) = n + i;
  }
  // cache unknown followed by lagrange indices
  data.unknown_lagrange.resize(data.unknown.size()+data.lagrange.size());
  // Would like to do:
  //data.unknown_lagrange << data.unknown, data.lagrange;
  // but Eigen can't handle empty vectors in comma initialization
  // https://forum.kde.org/viewtopic.php?f=74&t=107974&p=364947#p364947
  if(data.unknown.size() > 0)
  {
    data.unknown_lagrange.head(data.unknown.size()) = data.unknown;
  }
  if(data.lagrange.size() > 0)
  {
    data.unknown_lagrange.tail(data.lagrange.size()) = data.lagrange;
  }

  SparseMatrix<T> Auu;
  slice(A,data.unknown,data.unknown,Auu);
  assert(Auu.size() != 0 && Auu.rows() > 0 && "There should be at least one unknown.");

  // Positive definiteness is *not* determined, rather it is given as a
  // parameter
  data.Auu_pd = pd;
  if(data.Auu_pd)
  {
    // PD implies symmetric
    data.Auu_sym = true;
    // This is an annoying assertion unless EPS can be chosen in a nicer way.
    //assert(is_symmetric(Auu,EPS<T>()));
    assert(is_symmetric(Auu,1.0) &&
      "Auu should be symmetric if positive definite");
  }else
  {
    // determine if A(unknown,unknown) is symmetric and/or positive definite
    VectorXi AuuI,AuuJ;
    Matrix<T,Eigen::Dynamic,Eigen::Dynamic> AuuV;
    find(Auu,AuuI,AuuJ,AuuV);
    data.Auu_sym = is_symmetric(Auu,EPS<T>()*AuuV.maxCoeff());
  }

  // Determine number of linearly independent constraints
  int nc = 0;
  if(neq>0)
  {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    qr"<<endl;
#endif
    // QR decomposition to determine row rank in Aequ
    slice(Aeq,data.unknown,2,data.Aequ);
    assert(data.Aequ.rows() == neq &&
      "#Rows in Aequ should match #constraints");
    assert(data.Aequ.cols() == data.unknown.size() &&
      "#cols in Aequ should match #unknowns");
    data.AeqTQR.compute(data.Aequ.transpose().eval());
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    //cout<<endl<<matlab_format(SparseMatrix<T>(data.Aequ.transpose().eval()),"AeqT")<<endl<<endl;
#endif
    switch(data.AeqTQR.info())
    {
      case Eigen::Success:
        break;
      case Eigen::NumericalIssue:
#ifdef IGL_MIN_QUAD_WITH_FIXED_CPP_DEBUG
        cerr<<"Error: Numerical issue."<<endl;
#endif
        return false;
      case Eigen::InvalidInput:
#ifdef IGL_MIN_QUAD_WITH_FIXED_CPP_DEBUG
        cerr<<"Error: Invalid input."<<endl;
#endif
        return false;
      default:
#ifdef IGL_MIN_QUAD_WITH_FIXED_CPP_DEBUG
        cerr<<"Error: Other."<<endl;
#endif
        return false;
    }
    nc = data.AeqTQR.rank();
    assert(nc<=neq &&
      "Rank of reduced constraints should be <= #original constraints");
    data.Aeq_li = nc == neq;
    //cout<<"data.Aeq_li: "<<data.Aeq_li<<endl;
  }else
  {
    data.Aeq_li = true;
  }

  if(data.Aeq_li)
  {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    Aeq_li=true"<<endl;
#endif
    // Append lagrange multiplier quadratic terms
    SparseMatrix<T> new_A;
    SparseMatrix<T> AeqT = Aeq.transpose();
    SparseMatrix<T> Z(neq,neq);
    // This is a bit slower. But why isn't cat fast?
    new_A = cat(1, cat(2,   A, AeqT ),
                   cat(2, Aeq,    Z ));

    // precompute RHS builders
    if(kr > 0)
    {
      SparseMatrix<T> Aulk,Akul;
      // Slow
      slice(new_A,data.unknown_lagrange,data.known,Aulk);
      //// This doesn't work!!!
      //data.preY = Aulk + Akul.transpose();
      // Slow
      if(data.Auu_sym)
      {
        data.preY = Aulk*2;
      }else
      {
        slice(new_A,data.known,data.unknown_lagrange,Akul);
        SparseMatrix<T> AkulT = Akul.transpose();
        data.preY = Aulk + AkulT;
      }
    }else
    {
      data.preY.resize(data.unknown_lagrange.size(),0);
    }

    // Positive definite and no equality constraints (Positive definiteness
    // implies symmetric)
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    factorize"<<endl;
#endif
    if(data.Auu_pd && neq == 0)
    {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    llt"<<endl;
#endif
      data.llt.compute(Auu);
      switch(data.llt.info())
      {
        case Eigen::Success:
          break;
        case Eigen::NumericalIssue:
#ifdef IGL_MIN_QUAD_WITH_FIXED_CPP_DEBUG
          cerr<<"Error: Numerical issue."<<endl;
#endif
          return false;
        default:
#ifdef IGL_MIN_QUAD_WITH_FIXED_CPP_DEBUG
          cerr<<"Error: Other."<<endl;
#endif
          return false;
      }
      data.solver_type = min_quad_with_fixed_data<T>::LLT;
    }else
    {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
        cout<<"    ldlt/lu"<<endl;
#endif
      // Either not PD or there are equality constraints
      SparseMatrix<T> NA;
      slice(new_A,data.unknown_lagrange,data.unknown_lagrange,NA);
      data.NA = NA;
      if(data.Auu_pd)
      {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
        cout<<"    ldlt"<<endl;
#endif
        data.ldlt.compute(NA);
        switch(data.ldlt.info())
        {
          case Eigen::Success:
            break;
          case Eigen::NumericalIssue:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
            cerr<<"Error: Numerical issue."<<endl;
#endif
            return false;
          default:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
            cerr<<"Error: Other."<<endl;
#endif
            return false;
        }
        data.solver_type = min_quad_with_fixed_data<T>::LDLT;
      }else
      {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    lu"<<endl;
#endif
        // Resort to LU
        // Bottleneck >1/2
        data.lu.compute(NA);
        //std::cout<<"NA=["<<std::endl<<NA<<std::endl<<"];"<<std::endl;
        switch(data.lu.info())
        {
          case Eigen::Success:
            break;
          case Eigen::NumericalIssue:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
            cerr<<"Error: Numerical issue."<<endl;
            return false;
#endif
          case Eigen::InvalidInput:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
            cerr<<"Error: Invalid Input."<<endl;
#endif
            return false;
          default:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
            cerr<<"Error: Other."<<endl;
#endif
            return false;
        }
        data.solver_type = min_quad_with_fixed_data<T>::LU;
      }
    }
  }else
  {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    Aeq_li=false"<<endl;
#endif
    data.neq = neq;
    const int nu = data.unknown.size();
    //cout<<"nu: "<<nu<<endl;
    //cout<<"neq: "<<neq<<endl;
    //cout<<"nc: "<<nc<<endl;
    //cout<<"    matrixR"<<endl;
    SparseMatrix<T> AeqTR,AeqTQ;
    AeqTR = data.AeqTQR.matrixR();
    // This shouldn't be necessary
    AeqTR.prune(static_cast<T>(0.0));
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    matrixQ"<<endl;
#endif
    // THIS IS ESSENTIALLY DENSE AND THIS IS BY FAR THE BOTTLENECK
    // http://forum.kde.org/viewtopic.php?f=74&t=117500
    AeqTQ = data.AeqTQR.matrixQ();
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    prune"<<endl;
    cout<<"      nnz: "<<AeqTQ.nonZeros()<<endl;
#endif
    // This shouldn't be necessary
    AeqTQ.prune(static_cast<T>(0.0));
    //cout<<"AeqTQ: "<<AeqTQ.rows()<<" "<<AeqTQ.cols()<<endl;
    //cout<<matlab_format(AeqTQ,"AeqTQ")<<endl;
    //cout<<"    perms"<<endl;
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"      nnz: "<<AeqTQ.nonZeros()<<endl;
    cout<<"    perm"<<endl;
#endif
    SparseMatrix<T> I(neq,neq);
    I.setIdentity();
    data.AeqTE = data.AeqTQR.colsPermutation() * I;
    data.AeqTET = data.AeqTQR.colsPermutation().transpose() * I;
    assert(AeqTR.rows() == nu   && "#rows in AeqTR should match #unknowns");
    assert(AeqTR.cols() == neq  && "#cols in AeqTR should match #constraints");
    assert(AeqTQ.rows() == nu && "#rows in AeqTQ should match #unknowns");
    assert(AeqTQ.cols() == nu && "#cols in AeqTQ should match #unknowns");
    //cout<<"    slice"<<endl;
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    slice"<<endl;
#endif
    data.AeqTQ1 = AeqTQ.topLeftCorner(nu,nc);
    data.AeqTQ1T = data.AeqTQ1.transpose().eval();
    // ALREADY TRIM (Not 100% sure about this)
    data.AeqTR1 = AeqTR.topLeftCorner(nc,nc);
    data.AeqTR1T = data.AeqTR1.transpose().eval();
    //cout<<"AeqTR1T.size() "<<data.AeqTR1T.rows()<<" "<<data.AeqTR1T.cols()<<endl;
    // Null space
    data.AeqTQ2 = AeqTQ.bottomRightCorner(nu,nu-nc);
    data.AeqTQ2T = data.AeqTQ2.transpose().eval();
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    proj"<<endl;
#endif
    // Projected hessian
    SparseMatrix<T> QRAuu = data.AeqTQ2T * Auu * data.AeqTQ2;
    {
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
      cout<<"    factorize"<<endl;
#endif
      // QRAuu should always be PD
      data.llt.compute(QRAuu);
      switch(data.llt.info())
      {
        case Eigen::Success:
          break;
        case Eigen::NumericalIssue:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
          cerr<<"Error: Numerical issue."<<endl;
#endif
          return false;
        default:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
          cerr<<"Error: Other."<<endl;
#endif
          return false;
      }
      data.solver_type = min_quad_with_fixed_data<T>::QR_LLT;
    }
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
    cout<<"    smash"<<endl;
#endif
    // Known value multiplier
    SparseMatrix<T> Auk;
    slice(A,data.unknown,data.known,Auk);
    SparseMatrix<T> Aku;
    slice(A,data.known,data.unknown,Aku);
    SparseMatrix<T> AkuT = Aku.transpose();
    data.preY = Auk + AkuT;
    // Needed during solve
    data.Auu = Auu;
    slice(Aeq,data.known,2,data.Aeqk);
    assert(data.Aeqk.rows() == neq);
    assert(data.Aeqk.cols() == data.known.size());
  }
  return true;
}


template <
  typename T,
  typename DerivedB,
  typename DerivedY,
  typename DerivedBeq,
  typename DerivedZ,
  typename Derivedsol>
IGL_INLINE bool igl::min_quad_with_fixed_solve(
  const min_quad_with_fixed_data<T> & data,
  const Eigen::MatrixBase<DerivedB> & B,
  const Eigen::MatrixBase<DerivedY> & Y,
  const Eigen::MatrixBase<DerivedBeq> & Beq,
  Eigen::PlainObjectBase<DerivedZ> & Z,
  Eigen::PlainObjectBase<Derivedsol> & sol)
{
  using namespace std;
  using namespace Eigen;
  typedef Matrix<T,Dynamic,Dynamic> MatrixXT;
  // number of known rows
  int kr = data.known.size();
  if(kr!=0)
  {
    assert(kr == Y.rows());
  }
  // number of columns to solve
  int cols = Y.cols();
  assert(B.cols() == 1 || B.cols() == cols);
  assert(Beq.size() == 0 || Beq.cols() == 1 || Beq.cols() == cols);

  // resize output
  Z.resize(data.n,cols);
  // Set known values
  for(int i = 0;i < kr;i++)
  {
    for(int j = 0;j < cols;j++)
    {
      Z(data.known(i),j) = Y(i,j);
    }
  }

  if(data.Aeq_li)
  {
    // number of lagrange multipliers aka linear equality constraints
    int neq = data.lagrange.size();
    // append lagrange multiplier rhs's
    MatrixXT BBeq(B.rows() + Beq.rows(),cols);
    if(B.size() > 0)
    {
      BBeq.topLeftCorner(B.rows(),cols) = B.replicate(1,B.cols()==cols?1:cols);
    }
    if(Beq.size() > 0)
    {
      BBeq.bottomLeftCorner(Beq.rows(),cols) = -2.0*Beq.replicate(1,Beq.cols()==cols?1:cols);
    }

    // Build right hand side
    MatrixXT BBequlcols = BBeq(data.unknown_lagrange,igl::placeholders::all);
    MatrixXT NB;
    if(kr == 0)
    {
      NB = BBequlcols;
    }else
    {
      NB = data.preY * Y + BBequlcols;
    }

    //std::cout<<"NB=["<<std::endl<<NB<<std::endl<<"];"<<std::endl;
    //cout<<matlab_format(NB,"NB")<<endl;
    switch(data.solver_type)
    {
      case igl::min_quad_with_fixed_data<T>::LLT:
        sol = data.llt.solve(NB);
        break;
      case igl::min_quad_with_fixed_data<T>::LDLT:
        sol = data.ldlt.solve(NB);
        break;
      case igl::min_quad_with_fixed_data<T>::LU:
        // Not a bottleneck
        sol = data.lu.solve(NB);
        break;
      default:
#ifdef MIN_QUAD_WITH_FIXED_CPP_DEBUG
        cerr<<"Error: invalid solver type"<<endl;
#endif
        return false;
    }
    //std::cout<<"sol=["<<std::endl<<sol<<std::endl<<"];"<<std::endl;
    // Now sol contains sol/-0.5
    sol *= -0.5;
    // Now sol contains solution
    // Place solution in Z
    for(int i = 0;i<(sol.rows()-neq);i++)
    {
      for(int j = 0;j<sol.cols();j++)
      {
        Z(data.unknown_lagrange(i),j) = sol(i,j);
      }
    }
  }else
  {
    assert(data.solver_type == min_quad_with_fixed_data<T>::QR_LLT);
    MatrixXT eff_Beq;
    // Adjust Aeq rhs to include known parts
    eff_Beq =
      //data.AeqTQR.colsPermutation().transpose() * (-data.Aeqk * Y + Beq);
      data.AeqTET * (-data.Aeqk * Y + Beq.replicate(1,Beq.cols()==cols?1:cols));
    // Where did this -0.5 come from? Probably the same place as above.
    MatrixXT Bu = B(data.unknown,igl::placeholders::all);
    MatrixXT NB;
    NB = -0.5*(Bu.replicate(1,B.cols()==cols?1:cols) + data.preY * Y);
    // Trim eff_Beq
    const int nc = data.AeqTQR.rank();
    const int neq = Beq.rows();
    eff_Beq = eff_Beq.topLeftCorner(nc,cols).eval();
    data.AeqTR1T.template triangularView<Lower>().solveInPlace(eff_Beq);
    // Now eff_Beq = (data.AeqTR1T \ (data.AeqTET * (-data.Aeqk * Y + Beq)))
    MatrixXT lambda_0;
    lambda_0 = data.AeqTQ1 * eff_Beq;
    //cout<<matlab_format(lambda_0,"lambda_0")<<endl;
    MatrixXT QRB;
    QRB = -data.AeqTQ2T * (data.Auu * lambda_0) + data.AeqTQ2T * NB;
    Derivedsol lambda;
    lambda = data.llt.solve(QRB);
    // prepare output
    Derivedsol solu;
    solu = data.AeqTQ2 * lambda + lambda_0;
    //  http://www.math.uh.edu/~rohop/fall_06/Chapter3.pdf
    Derivedsol solLambda;
    {
      Derivedsol temp1,temp2;
      temp1 = (data.AeqTQ1T * NB - data.AeqTQ1T * data.Auu * solu);
      data.AeqTR1.template triangularView<Upper>().solveInPlace(temp1);
      //cout<<matlab_format(temp1,"temp1")<<endl;
      temp2 = Derivedsol::Zero(neq,cols);
      temp2.topLeftCorner(nc,cols) = temp1;
      //solLambda = data.AeqTQR.colsPermutation() * temp2;
      solLambda = data.AeqTE * temp2;
    }
    // sol is [Z(unknown);Lambda]
    assert(data.unknown.size() == solu.rows());
    assert(cols == solu.cols());
    assert(data.neq == neq);
    assert(data.neq == solLambda.rows());
    assert(cols == solLambda.cols());
    sol.resize(data.unknown.size()+data.neq,cols);
    sol.block(0,0,solu.rows(),solu.cols()) = solu;
    sol.block(solu.rows(),0,solLambda.rows(),solLambda.cols()) = solLambda;
    for(int u = 0;u<data.unknown.size();u++)
    {
      for(int j = 0;j<Z.cols();j++)
      {
        Z(data.unknown(u),j) = solu(u,j);
      }
    }
  }
  return true;
}

template <
  typename T,
  typename DerivedB,
  typename DerivedY,
  typename DerivedBeq,
  typename DerivedZ>
IGL_INLINE bool igl::min_quad_with_fixed_solve(
  const min_quad_with_fixed_data<T> & data,
  const Eigen::MatrixBase<DerivedB> & B,
  const Eigen::MatrixBase<DerivedY> & Y,
  const Eigen::MatrixBase<DerivedBeq> & Beq,
  Eigen::PlainObjectBase<DerivedZ> & Z)
{
  Eigen::Matrix<typename DerivedZ::Scalar, Eigen::Dynamic, Eigen::Dynamic> sol;
  return min_quad_with_fixed_solve(data,B,Y,Beq,Z,sol);
}

template <
  typename T,
  typename Derivedknown,
  typename DerivedB,
  typename DerivedY,
  typename DerivedBeq,
  typename DerivedZ>
IGL_INLINE bool igl::min_quad_with_fixed(
  const Eigen::SparseMatrix<T>& A,
  const Eigen::MatrixBase<DerivedB> & B,
  const Eigen::MatrixBase<Derivedknown> & known,
  const Eigen::MatrixBase<DerivedY> & Y,
  const Eigen::SparseMatrix<T>& Aeq,
  const Eigen::MatrixBase<DerivedBeq> & Beq,
  const bool pd,
  Eigen::PlainObjectBase<DerivedZ> & Z)
{
  min_quad_with_fixed_data<T> data;
  if(!min_quad_with_fixed_precompute(A,known,Aeq,pd,data))
  {
    return false;
  }
  return min_quad_with_fixed_solve(data,B,Y,Beq,Z);
}


template <typename Scalar, int n, int m, bool Hpd>
IGL_INLINE Eigen::Matrix<Scalar,n,1> igl::min_quad_with_fixed(
  const Eigen::Matrix<Scalar,n,n> & H,
  const Eigen::Matrix<Scalar,n,1> & f,
  const Eigen::Array<bool,n,1> & k,
  const Eigen::Matrix<Scalar,n,1> & bc,
  const Eigen::Matrix<Scalar,m,n> & A,
  const Eigen::Matrix<Scalar,m,1> & b)
{
  const auto dyn_n = n == Eigen::Dynamic ? H.rows() : n;
  const auto dyn_m = m == Eigen::Dynamic ? A.rows() : m;
  constexpr const int nn = n == Eigen::Dynamic ? Eigen::Dynamic : n+m;
  const auto dyn_nn = nn == Eigen::Dynamic ? dyn_n+dyn_m : nn;
  if(dyn_m == 0)
  {
    return igl::min_quad_with_fixed<Scalar,n,Hpd>(H,f,k,bc);
  }
  // min_x ½ xᵀ H x + xᵀ f   subject to A x = b and x(k) = bc(k)
  // let zᵀ = [xᵀ λᵀ]
  // min_z ½ zᵀ [H Aᵀ;A 0] z + zᵀ [f;-b]   z(k) = bc(k)
  const auto make_HH = [&]()
  {
    // Windows can't remember that nn is const.
    constexpr const int nn = n == Eigen::Dynamic ? Eigen::Dynamic : n+m;
    Eigen::Matrix<Scalar,nn,nn> HH =
      Eigen::Matrix<Scalar,nn,nn>::Zero(dyn_nn,dyn_nn);
    HH.topLeftCorner(dyn_n,dyn_n) = H;
    HH.bottomLeftCorner(dyn_m,dyn_n) = A;
    HH.topRightCorner(dyn_n,dyn_m) = A.transpose();
    return HH;
  };
  const Eigen::Matrix<Scalar,nn,nn> HH = make_HH();
  const auto make_ff  = [&]()
  {
    // Windows can't remember that nn is const.
    constexpr const int nn = n == Eigen::Dynamic ? Eigen::Dynamic : n+m;
    Eigen::Matrix<Scalar,nn,1> ff(dyn_nn);
    ff.head(dyn_n) =  f;
    ff.tail(dyn_m) = -b;
    return ff;
  };
  const Eigen::Matrix<Scalar,nn,1> ff = make_ff();
  const auto make_kk  = [&]()
  {
    // Windows can't remember that nn is const.
    constexpr const int nn = n == Eigen::Dynamic ? Eigen::Dynamic : n+m;
    Eigen::Array<bool,nn,1> kk =
      Eigen::Array<bool,nn,1>::Constant(dyn_nn,1,false);
    kk.head(dyn_n) =  k;
    return kk;
  };
  const Eigen::Array<bool,nn,1> kk = make_kk();
  const auto make_bcbc= [&]()
  {
    // Windows can't remember that nn is const.
    constexpr const int nn = n == Eigen::Dynamic ? Eigen::Dynamic : n+m;
    Eigen::Matrix<Scalar,nn,1> bcbc(dyn_nn);
    bcbc.head(dyn_n) =  bc;
    return bcbc;
  };
  const Eigen::Matrix<Scalar,nn,1> bcbc = make_bcbc();
  const Eigen::Matrix<Scalar,nn,1> xx =
    min_quad_with_fixed<Scalar,nn,false>(HH,ff,kk,bcbc);
  return xx.head(dyn_n);
}

template <typename Scalar, int n, bool Hpd>
IGL_INLINE Eigen::Matrix<Scalar,n,1> igl::min_quad_with_fixed(
  const Eigen::Matrix<Scalar,n,n> & H,
  const Eigen::Matrix<Scalar,n,1> & f,
  const Eigen::Array<bool,n,1> & k,
  const Eigen::Matrix<Scalar,n,1> & bc)
{
  assert(H.isApprox(H.transpose(),1e-7));
  assert(H.rows() == H.cols());
  assert(H.rows() == f.size());
  assert(H.rows() == k.size());
  assert(H.rows() == bc.size());
  const auto kcount = k.count();
  // Everything fixed
  if(kcount == (Eigen::Dynamic?H.rows():n))
  {
    return bc;
  }
  // Nothing fixed
  if(kcount == 0)
  {
    // avoid function call
    typedef Eigen::Matrix<Scalar,n,n> MatrixSn;
    typedef typename
      std::conditional<Hpd,Eigen::LLT<MatrixSn>,Eigen::CompleteOrthogonalDecomposition<MatrixSn>>::type
      Solver;
    return Solver(H).solve(-f);
  }
  // All-but-one fixed
  if( (Eigen::Dynamic?H.rows():n)-kcount == 1)
  {
    // which one is not fixed?
    int u = -1;
    for(int i=0;i<k.size();i++){ if(!k(i)){ u=i; break; } }
    assert(u>=0);
    // min ½ x(u) Huu x(u) + x(u)(fu + H(u,k)bc(k))
    // Huu x(u) = -(fu + H(u,k) bc(k))
    // x(u) = (-fu + ∑ -Huj bcj)/Huu
    Eigen::Matrix<Scalar,n,1> x = bc;
    x(u) = -f(u);
    for(int i=0;i<k.size();i++){ if(i!=u){ x(u)-=bc(i)*H(i,u); } }
    x(u) /= H(u,u);
    return x;
  }
  // Alec: Is there a smart template way to do this?
  // jdumas: I guess you could do a templated for-loop starting from 16, and
  // dispatching to the appropriate templated function when the argument matches
  // (with a fallback to the dynamic version). Cf this example:
  // https://gist.github.com/disconnect3d/13c2d035bb31b244df14
  switch(kcount)
  {
    case 0: assert(false && "Handled above."); return Eigen::Matrix<Scalar,n,1>();
    // % Matlibberish for generating these case statements:
    // maxi=16;for i=1:maxi;fprintf('    case %d:\n    {\n     const bool D = (n-%d<=0)||(%d>=n)||(n>%d);\n     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:%d,Hpd>(H,f,k,bc);\n    }\n',[i i i maxi i]);end
    case 1:
    {
     const bool D = (n-1<=0)||(1>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:1,Hpd>(H,f,k,bc);
    }
    case 2:
    {
     const bool D = (n-2<=0)||(2>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:2,Hpd>(H,f,k,bc);
    }
    case 3:
    {
     const bool D = (n-3<=0)||(3>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:3,Hpd>(H,f,k,bc);
    }
    case 4:
    {
     const bool D = (n-4<=0)||(4>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:4,Hpd>(H,f,k,bc);
    }
    case 5:
    {
     const bool D = (n-5<=0)||(5>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:5,Hpd>(H,f,k,bc);
    }
    case 6:
    {
     const bool D = (n-6<=0)||(6>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:6,Hpd>(H,f,k,bc);
    }
    case 7:
    {
     const bool D = (n-7<=0)||(7>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:7,Hpd>(H,f,k,bc);
    }
    case 8:
    {
     const bool D = (n-8<=0)||(8>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:8,Hpd>(H,f,k,bc);
    }
    case 9:
    {
     const bool D = (n-9<=0)||(9>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:9,Hpd>(H,f,k,bc);
    }
    case 10:
    {
     const bool D = (n-10<=0)||(10>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:10,Hpd>(H,f,k,bc);
    }
    case 11:
    {
     const bool D = (n-11<=0)||(11>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:11,Hpd>(H,f,k,bc);
    }
    case 12:
    {
     const bool D = (n-12<=0)||(12>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:12,Hpd>(H,f,k,bc);
    }
    case 13:
    {
     const bool D = (n-13<=0)||(13>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:13,Hpd>(H,f,k,bc);
    }
    case 14:
    {
     const bool D = (n-14<=0)||(14>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:14,Hpd>(H,f,k,bc);
    }
    case 15:
    {
     const bool D = (n-15<=0)||(15>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:15,Hpd>(H,f,k,bc);
    }
    case 16:
    {
     const bool D = (n-16<=0)||(16>=n)||(n>16);
     return min_quad_with_fixed<Scalar,D?Eigen::Dynamic:n,D?Eigen::Dynamic:16,Hpd>(H,f,k,bc);
    }
    default:
      return min_quad_with_fixed<Scalar,Eigen::Dynamic,Eigen::Dynamic,Hpd>(H,f,k,bc);
  }
}

template <typename Scalar, int n, int kcount, bool Hpd>
IGL_INLINE Eigen::Matrix<Scalar,n,1> igl::min_quad_with_fixed(
  const Eigen::Matrix<Scalar,n,n> & H,
  const Eigen::Matrix<Scalar,n,1> & f,
  const Eigen::Array<bool,n,1> & k,
  const Eigen::Matrix<Scalar,n,1> & bc)
{
  // 0 and n should be handle outside this function
  static_assert(kcount==Eigen::Dynamic || kcount>0                  ,"");
  static_assert(kcount==Eigen::Dynamic || kcount<n                  ,"");
  const int ucount = n==Eigen::Dynamic ? Eigen::Dynamic : n-kcount;
  static_assert(kcount==Eigen::Dynamic || ucount+kcount == n        ,"");
  static_assert((n==Eigen::Dynamic) == (ucount==Eigen::Dynamic),"");
  static_assert((kcount==Eigen::Dynamic) == (ucount==Eigen::Dynamic),"");
  assert((n==Eigen::Dynamic) || n == H.rows());
  assert((kcount==Eigen::Dynamic) || kcount == k.count());
  typedef Eigen::Matrix<Scalar,ucount,ucount> MatrixSuu;
  typedef Eigen::Matrix<Scalar,ucount,kcount> MatrixSuk;
  typedef Eigen::Matrix<Scalar,n,1>      VectorSn;
  typedef Eigen::Matrix<Scalar,ucount,1> VectorSu;
  typedef Eigen::Matrix<Scalar,kcount,1> VectorSk;
  const auto dyn_n = n==Eigen::Dynamic ? H.rows() : n;
  const auto dyn_kcount = kcount==Eigen::Dynamic ? k.count() : kcount;
  const auto dyn_ucount = ucount==Eigen::Dynamic ? dyn_n- dyn_kcount : ucount;
  // For ucount==2 or kcount==2 this calls the coefficient initiliazer rather
  // than the size initilizer, but I guess that's ok.
  MatrixSuu Huu(dyn_ucount,dyn_ucount);
  MatrixSuk Huk(dyn_ucount,dyn_kcount);
  VectorSu mrhs(dyn_ucount);
  VectorSk  bck(dyn_kcount);
  {
    int ui = 0;
    int ki = 0;
    for(int i = 0;i<dyn_n;i++)
    {
      if(k(i))
      {
        bck(ki) = bc(i);
        ki++;
      }else
      {
        mrhs(ui) = f(i);
        int uj = 0;
        int kj = 0;
        for(int j = 0;j<dyn_n;j++)
        {
          if(k(j))
          {
            Huk(ui,kj) = H(i,j);
            kj++;
          }else
          {
            Huu(ui,uj) = H(i,j);
            uj++;
          }
        }
        ui++;
      }
    }
  }
  mrhs += Huk * bck;
  typedef typename
    std::conditional<Hpd,
      Eigen::LLT<MatrixSuu>,
      // LDLT should be faster for indefinite problems but already found some
      // cases where it was too inaccurate when called via quadprog_primal.
      // Ideally this function takes LLT,LDLT, or
      // CompleteOrthogonalDecomposition as a template parameter.  "template
      // template" parameters did work because LLT,LDLT have different number of
      // template parameters from CompleteOrthogonalDecomposition.  Perhaps
      // there's a way to take advantage of LLT and LDLT's default template
      // parameters (I couldn't figure out how).
      Eigen::CompleteOrthogonalDecomposition<MatrixSuu>>::type
    Solver;
  VectorSu xu = Solver(Huu).solve(-mrhs);
  VectorSn x(dyn_n);
  {
    int ui = 0;
    int ki = 0;
    for(int i = 0;i<dyn_n;i++)
    {
      if(k(i))
      {
        x(i) = bck(ki);
        ki++;
      }else
      {
        x(i) = xu(ui);
        ui++;
      }
    }
  }
  return x;
}
