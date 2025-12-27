// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "active_set.h"
#include "min_quad_with_fixed.h"
#include "slice.h"
#include "slice_into.h"
#include "cat.h"
//#include "matlab_format.h"

#include <iostream>
#include <limits>
#include <algorithm>

template <
  typename AT,
  typename DerivedB,
  typename Derivedknown,
  typename DerivedY,
  typename AeqT,
  typename DerivedBeq,
  typename AieqT,
  typename DerivedBieq,
  typename Derivedlx,
  typename Derivedux,
  typename DerivedZ
  >
IGL_INLINE igl::SolverStatus igl::active_set(
  const Eigen::SparseMatrix<AT>& A,
  const Eigen::PlainObjectBase<DerivedB> & B,
  const Eigen::PlainObjectBase<Derivedknown> & known,
  const Eigen::PlainObjectBase<DerivedY> & Y,
  const Eigen::SparseMatrix<AeqT>& Aeq,
  const Eigen::PlainObjectBase<DerivedBeq> & Beq,
  const Eigen::SparseMatrix<AieqT>& Aieq,
  const Eigen::PlainObjectBase<DerivedBieq> & Bieq,
  const Eigen::PlainObjectBase<Derivedlx> & p_lx,
  const Eigen::PlainObjectBase<Derivedux> & p_ux,
  const igl::active_set_params & params,
  Eigen::PlainObjectBase<DerivedZ> & Z
  )
{
//#define ACTIVE_SET_CPP_DEBUG
#if defined(ACTIVE_SET_CPP_DEBUG) && !defined(_MSC_VER)
#  warning "ACTIVE_SET_CPP_DEBUG"
#endif
  using namespace Eigen;
  using namespace std;
  SolverStatus ret = SOLVER_STATUS_ERROR;
  const int n = A.rows();
  assert(n == A.cols() && "A must be square");
  // Discard const qualifiers
  //if(B.size() == 0)
  //{
  //  B = DerivedB::Zero(n,1);
  //}
  assert(n == B.rows() && "B.rows() must match A.rows()");
  assert(B.cols() == 1 && "B must be a column vector");
  assert(Y.cols() == 1 && "Y must be a column vector");
  assert((Aeq.size() == 0 && Beq.size() == 0) || Aeq.cols() == n);
  assert((Aeq.size() == 0 && Beq.size() == 0) || Aeq.rows() == Beq.rows());
  assert((Aeq.size() == 0 && Beq.size() == 0) || Beq.cols() == 1);
  assert((Aieq.size() == 0 && Bieq.size() == 0) || Aieq.cols() == n);
  assert((Aieq.size() == 0 && Bieq.size() == 0) || Aieq.rows() == Bieq.rows());
  assert((Aieq.size() == 0 && Bieq.size() == 0) || Bieq.cols() == 1);
  Eigen::Matrix<typename Derivedlx::Scalar,Eigen::Dynamic,1> lx;
  Eigen::Matrix<typename Derivedux::Scalar,Eigen::Dynamic,1> ux;
  if(p_lx.size() == 0)
  {
    lx = Derivedlx::Constant(
      n,1,-numeric_limits<typename Derivedlx::Scalar>::max());
  }else
  {
    lx = p_lx;
  }
  if(p_ux.size() == 0)
  {
    ux = Derivedux::Constant(
      n,1,numeric_limits<typename Derivedux::Scalar>::max());
  }else
  {
    ux = p_ux;
  }
  assert(lx.rows() == n && "lx must have n rows");
  assert(ux.rows() == n && "ux must have n rows");
  assert(ux.cols() == 1 && "lx must be a column vector");
  assert(lx.cols() == 1 && "ux must be a column vector");
  assert((ux.array()-lx.array()).minCoeff() > 0 && "ux(i) must be > lx(i)");
  if(Z.size() != 0)
  {
    // Initial guess should have correct size
    assert(Z.rows() == n && "Z must have n rows");
    assert(Z.cols() == 1 && "Z must be a column vector");
  }
  assert(known.cols() == 1 && "known must be a column vector");
  // Number of knowns
  const int nk = known.size();

  // Initialize active sets
  typedef int BOOL;
#define TRUE 1
#define FALSE 0
  Matrix<BOOL,Dynamic,1> as_lx = Matrix<BOOL,Dynamic,1>::Constant(n,1,FALSE);
  Matrix<BOOL,Dynamic,1> as_ux = Matrix<BOOL,Dynamic,1>::Constant(n,1,FALSE);
  Matrix<BOOL,Dynamic,1> as_ieq = Matrix<BOOL,Dynamic,1>::Constant(Aieq.rows(),1,FALSE);

  // Keep track of previous Z for comparison
  DerivedZ old_Z;
  old_Z = DerivedZ::Constant(
      n,1,numeric_limits<typename DerivedZ::Scalar>::max());

  int iter = 0;
  while(true)
  {
#ifdef ACTIVE_SET_CPP_DEBUG
    cout<<"Iteration: "<<iter<<":"<<endl;
    cout<<"  pre"<<endl;
#endif
    // FIND BREACHES OF CONSTRAINTS
    int new_as_lx = 0;
    int new_as_ux = 0;
    int new_as_ieq = 0;
    if(Z.size() > 0)
    {
      for(int z = 0;z < n;z++)
      {
        if(Z(z) < lx(z))
        {
          new_as_lx += (as_lx(z)?0:1);
          //new_as_lx++;
          as_lx(z) = TRUE;
        }
        if(Z(z) > ux(z))
        {
          new_as_ux += (as_ux(z)?0:1);
          //new_as_ux++;
          as_ux(z) = TRUE;
        }
      }
      if(Aieq.rows() > 0)
      {
        DerivedZ AieqZ;
        AieqZ = Aieq*Z;
        for(int a = 0;a<Aieq.rows();a++)
        {
          if(AieqZ(a) > Bieq(a))
          {
            new_as_ieq += (as_ieq(a)?0:1);
            as_ieq(a) = TRUE;
          }
        }
      }
#ifdef ACTIVE_SET_CPP_DEBUG
      cout<<"  new_as_lx: "<<new_as_lx<<endl;
      cout<<"  new_as_ux: "<<new_as_ux<<endl;
#endif
      const double diff = (Z-old_Z).squaredNorm();
#ifdef ACTIVE_SET_CPP_DEBUG
      cout<<"diff: "<<diff<<endl;
#endif
      if(diff < params.solution_diff_threshold)
      {
        ret = SOLVER_STATUS_CONVERGED;
        break;
      }
      old_Z = Z;
    }

    const int as_lx_count = std::count(as_lx.data(),as_lx.data()+n,TRUE);
    const int as_ux_count = std::count(as_ux.data(),as_ux.data()+n,TRUE);
    const int as_ieq_count =
      std::count(as_ieq.data(),as_ieq.data()+as_ieq.size(),TRUE);
#ifndef NDEBUG
    {
      int count = 0;
      for(int a = 0;a<as_ieq.size();a++)
      {
        if(as_ieq(a))
        {
          assert(as_ieq(a) == TRUE);
          count++;
        }
      }
      assert(as_ieq_count == count);
    }
#endif

    // PREPARE FIXED VALUES
    Derivedknown known_i;
    known_i.resize(nk + as_lx_count + as_ux_count,1);
    DerivedY Y_i;
    Y_i.resize(nk + as_lx_count + as_ux_count,1);
    {
      known_i.block(0,0,known.rows(),known.cols()) = known;
      Y_i.block(0,0,Y.rows(),Y.cols()) = Y;
      int k = nk;
      // Then all lx
      for(int z = 0;z < n;z++)
      {
        if(as_lx(z))
        {
          known_i(k) = z;
          Y_i(k) = lx(z);
          k++;
        }
      }
      // Finally all ux
      for(int z = 0;z < n;z++)
      {
        if(as_ux(z))
        {
          known_i(k) = z;
          Y_i(k) = ux(z);
          k++;
        }
      }
      assert(k==Y_i.size());
      assert(k==known_i.size());
    }
    //cout<<matlab_format((known_i.array()+1).eval(),"known_i")<<endl;
    // PREPARE EQUALITY CONSTRAINTS
    VectorXi as_ieq_list(as_ieq_count,1);
    // Gather active constraints and resp. rhss
    DerivedBeq Beq_i;
    Beq_i.resize(Beq.rows()+as_ieq_count,1);
    Beq_i.head(Beq.rows()) = Beq;
    {
      int k =0;
      for(int a=0;a<as_ieq.size();a++)
      {
        if(as_ieq(a))
        {
          assert(k<as_ieq_list.size());
          as_ieq_list(k)=a;
          Beq_i(Beq.rows()+k,0) = Bieq(k,0);
          k++;
        }
      }
      assert(k == as_ieq_count);
    }
    // extract active constraint rows
    SparseMatrix<AeqT> Aeq_i,Aieq_i;
    slice(Aieq,as_ieq_list,1,Aieq_i);
    // Append to equality constraints
    cat(1,Aeq,Aieq_i,Aeq_i);


    min_quad_with_fixed_data<AT> data;
#ifndef NDEBUG
    {
      // NO DUPES!
      Matrix<BOOL,Dynamic,1> fixed = Matrix<BOOL,Dynamic,1>::Constant(n,1,FALSE);
      for(int k = 0;k<known_i.size();k++)
      {
        assert(!fixed[known_i(k)]);
        fixed[known_i(k)] = TRUE;
      }
    }
#endif

    DerivedZ sol;
    if(known_i.size() == A.rows())
    {
      // Everything's fixed?
#ifdef ACTIVE_SET_CPP_DEBUG
      cout<<"  everything's fixed."<<endl;
#endif
      Z.resize(A.rows(),Y_i.cols());
      slice_into(Y_i,known_i,1,Z);
      sol.resize(0,Y_i.cols());
      assert(Aeq_i.rows() == 0 && "All fixed but linearly constrained");
    }else
    {
#ifdef ACTIVE_SET_CPP_DEBUG
      cout<<"  min_quad_with_fixed_precompute"<<endl;
#endif
      if(!min_quad_with_fixed_precompute(A,known_i,Aeq_i,params.Auu_pd,data))
      {
        cerr<<"Error: min_quad_with_fixed precomputation failed."<<endl;
        if(iter > 0 && Aeq_i.rows() > Aeq.rows())
        {
          cerr<<"  *Are you sure rows of [Aeq;Aieq] are linearly independent?*"<<
            endl;
        }
        ret = SOLVER_STATUS_ERROR;
        break;
      }
#ifdef ACTIVE_SET_CPP_DEBUG
      cout<<"  min_quad_with_fixed_solve"<<endl;
#endif
      if(!min_quad_with_fixed_solve(data,B,Y_i,Beq_i,Z,sol))
      {
        cerr<<"Error: min_quad_with_fixed solve failed."<<endl;
        ret = SOLVER_STATUS_ERROR;
        break;
      }
      //cout<<matlab_format((Aeq*Z-Beq).eval(),"cr")<<endl;
      //cout<<matlab_format(Z,"Z")<<endl;
#ifdef ACTIVE_SET_CPP_DEBUG
      cout<<"  post"<<endl;
#endif
      // Computing Lagrange multipliers needs to be adjusted slightly if A is not symmetric
      assert(data.Auu_sym);
    }

    // Compute Lagrange multiplier values for known_i
    SparseMatrix<AT> Ak;
    // Slow
    slice(A,known_i,1,Ak);
    DerivedB Bk;
    slice(B,known_i,Bk);
    MatrixXd Lambda_known_i = -(0.5*Ak*Z + 0.5*Bk);
    // reverse the lambda values for lx
    Lambda_known_i.block(nk,0,as_lx_count,1) =
      (-1*Lambda_known_i.block(nk,0,as_lx_count,1)).eval();

    // Extract Lagrange multipliers for Aieq_i (always at back of sol)
    VectorXd Lambda_Aieq_i(Aieq_i.rows(),1);
    for(int l = 0;l<Aieq_i.rows();l++)
    {
      Lambda_Aieq_i(Aieq_i.rows()-1-l) = sol(sol.rows()-1-l);
    }

    // Remove from active set
    for(int l = 0;l<as_lx_count;l++)
    {
      if(Lambda_known_i(nk + l) < params.inactive_threshold)
      {
        as_lx(known_i(nk + l)) = FALSE;
      }
    }
    for(int u = 0;u<as_ux_count;u++)
    {
      if(Lambda_known_i(nk + as_lx_count + u) <
        params.inactive_threshold)
      {
        as_ux(known_i(nk + as_lx_count + u)) = FALSE;
      }
    }
    for(int a = 0;a<as_ieq_count;a++)
    {
      if(Lambda_Aieq_i(a) < params.inactive_threshold)
      {
        as_ieq(as_ieq_list(a)) = FALSE;
      }
    }

    iter++;
    //cout<<iter<<endl;
    if(params.max_iter>0 && iter>=params.max_iter)
    {
      ret = SOLVER_STATUS_MAX_ITER;
      break;
    }

  }

  return ret;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template igl::SolverStatus igl::active_set<double, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, double, Eigen::Matrix<double, -1, 1, 0, -1, 1>, double, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::SparseMatrix<double, 0, int> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::SparseMatrix<double, 0, int> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, igl::active_set_params const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template igl::SolverStatus igl::active_set<double, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, double, Eigen::Matrix<double, -1, 1, 0, -1, 1>, double, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::SparseMatrix<double, 0, int> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::SparseMatrix<double, 0, int> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, igl::active_set_params const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
