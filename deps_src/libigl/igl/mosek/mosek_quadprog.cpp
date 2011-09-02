// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mosek_quadprog.h"
#include "mosek_guarded.h"
#include <cstdio>
#include "../find.h"
#include "../verbose.h"
#include "../speye.h"
#include "../matrix_to_list.h"
#include "../list_to_matrix.h"
#include "../harwell_boeing.h"
#include "../EPS.h"


igl::mosek::MosekData::MosekData()
{
  // These are the default settings that worked well for BBW. Your miles may
  // very well be kilometers.

  // >1e0 NONSOLUTION
  // 1e-1 artifacts in deformation
  // 1e-3 artifacts in isolines
  // 1e-4 seems safe
  // 1e-8 MOSEK DEFAULT SOLUTION
  douparam[MSK_DPAR_INTPNT_TOL_REL_GAP]=1e-8;
#if MSK_VERSION_MAJOR >= 8
  douparam[MSK_DPAR_INTPNT_QO_TOL_REL_GAP]=1e-12;
#endif
  // Force using multiple threads, not sure if MOSEK is properly destroying
  //extra threads...
#if MSK_VERSION_MAJOR >= 7
  intparam[MSK_IPAR_NUM_THREADS] = 6;
#elif MSK_VERSION_MAJOR == 6
  intparam[MSK_IPAR_INTPNT_NUM_THREADS] = 6;
#endif
#if MSK_VERSION_MAJOR == 6
  // Force turn off data check
  intparam[MSK_IPAR_DATA_CHECK]=MSK_OFF;
#endif
  // Turn off presolving
  // intparam[MSK_IPAR_PRESOLVE_USE] = MSK_PRESOLVE_MODE_OFF;
  // Force particular matrix reordering method
  // MSK_ORDER_METHOD_NONE cuts time in half roughly, since half the time is
  //   usually spent reordering the matrix
  // !! WARNING Setting this parameter to anything but MSK_ORDER_METHOD_FREE
  //   seems to have the effect of setting it to MSK_ORDER_METHOD_NONE
  //   *Or maybe Mosek is spending a bunch of time analyzing the matrix to
  //   choose the right ordering method when really any of them are
  //   instantaneous
  intparam[MSK_IPAR_INTPNT_ORDER_METHOD] = MSK_ORDER_METHOD_NONE;
  // 1.0 means optimizer is very lenient about declaring model infeasible
  douparam[MSK_DPAR_INTPNT_TOL_INFEAS] = 1e-8;
  // Hard to say if this is doing anything, probably nothing dramatic
  douparam[MSK_DPAR_INTPNT_TOL_PSAFE]= 1e2;
  // Turn off convexity check
  intparam[MSK_IPAR_CHECK_CONVEXITY] = MSK_CHECK_CONVEXITY_NONE;
}

template <typename Index, typename Scalar>
IGL_INLINE bool igl::mosek::mosek_quadprog(
  const Index n,
  std::vector<Index> & Qi,
  std::vector<Index> & Qj,
  std::vector<Scalar> & Qv,
  const std::vector<Scalar> & c,
  const Scalar cf,
  const Index m,
  std::vector<Scalar> & Av,
  std::vector<Index> & Ari,
  const std::vector<Index> & Acp,
  const std::vector<Scalar> & lc,
  const std::vector<Scalar> & uc,
  const std::vector<Scalar> & lx,
  const std::vector<Scalar> & ux,
  MosekData & mosek_data,
  std::vector<Scalar> & x)
{
  // I J V vectors of Q should all be same length
  assert(Qv.size() == Qi.size());
  assert(Qv.size() == Qj.size());
  // number of columns in linear constraint matrix must be ≤ number of
  // variables
  assert( (int)Acp.size() == (n+1));
  // linear bound vectors must be size of number of constraints or empty
  assert( ((int)lc.size() == m) || ((int)lc.size() == 0));
  assert( ((int)uc.size() == m) || ((int)uc.size() == 0));
  // constant bound vectors must be size of number of variables or empty
  assert( ((int)lx.size() == n) || ((int)lx.size() == 0));
  assert( ((int)ux.size() == n) || ((int)ux.size() == 0));

  // allocate space for solution in x
  x.resize(n);

  // variables for mosek task, env and result code
  MSKenv_t env;
  MSKtask_t task;

  // Create the MOSEK environment
#if MSK_VERSION_MAJOR >= 7
  mosek_guarded(MSK_makeenv(&env,NULL));
#elif MSK_VERSION_MAJOR == 6
  mosek_guarded(MSK_makeenv(&env,NULL,NULL,NULL,NULL));
#endif
  ///* Directs the log stream to the 'printstr' function. */
  //// Little function mosek needs in order to know how to print to std out
  //const auto & printstr = [](void *handle, char str[])
  //{
  //  printf("%s",str);
  //}
  //mosek_guarded(MSK_linkfunctoenvstream(env,MSK_STREAM_LOG,NULL,printstr));
  // initialize mosek environment
#if MSK_VERSION_MAJOR <= 7
  mosek_guarded(MSK_initenv(env));
#endif
  // Create the optimization task
  mosek_guarded(MSK_maketask(env,m,n,&task));
  verbose("Creating task with %ld linear constraints and %ld variables...\n",m,n);
  //// Tell mosek how to print to std out
  //mosek_guarded(MSK_linkfunctotaskstream(task,MSK_STREAM_LOG,NULL,printstr));
  // Give estimate of number of variables
  mosek_guarded(MSK_putmaxnumvar(task,n));
  if(m>0)
  {
    // Give estimate of number of constraints
    mosek_guarded(MSK_putmaxnumcon(task,m));
    // Give estimate of number of non zeros in A
    mosek_guarded(MSK_putmaxnumanz(task,Av.size()));
  }
  // Give estimate of number of non zeros in Q
  mosek_guarded(MSK_putmaxnumqnz(task,Qv.size()));
  if(m>0)
  {
    // Append 'm' empty constraints, the constrainst will initially have no
    // bounds
#if MSK_VERSION_MAJOR >= 7
    mosek_guarded(MSK_appendcons(task,m));
#elif MSK_VERSION_MAJOR == 6
    mosek_guarded(MSK_append(task,MSK_ACC_CON,m));
#endif
  }
  // Append 'n' variables
#if MSK_VERSION_MAJOR >= 7
  mosek_guarded(MSK_appendvars(task,n));
#elif MSK_VERSION_MAJOR == 6
  mosek_guarded(MSK_append(task,MSK_ACC_VAR,n));
#endif
  // add a contant term to the objective
  mosek_guarded(MSK_putcfix(task,cf));

  // loop over variables
  for(int j = 0;j<n;j++)
  {
    if(c.size() > 0)
    {
      // Set linear term c_j in the objective
      mosek_guarded(MSK_putcj(task,j,c[j]));
    }

    // Set constant bounds on variable j
    if(lx[j] == ux[j])
    {
      mosek_guarded(MSK_putbound(task,MSK_ACC_VAR,j,MSK_BK_FX,lx[j],ux[j]));
    }else
    {
      mosek_guarded(MSK_putbound(task,MSK_ACC_VAR,j,MSK_BK_RA,lx[j],ux[j]));
    }

    if(m>0)
    {
      // Input column j of A
#if MSK_VERSION_MAJOR >= 7
      mosek_guarded(
        MSK_putacol(
          task,
          j,
          Acp[j+1]-Acp[j],
          &Ari[Acp[j]],
          &Av[Acp[j]])
        );
#elif MSK_VERSION_MAJOR == 6
      mosek_guarded(
        MSK_putavec(
          task,
          MSK_ACC_VAR,
          j,
          Acp[j+1]-Acp[j],
          &Ari[Acp[j]],
          &Av[Acp[j]])
        );
#endif
    }
  }

  // loop over constraints
  for(int i = 0;i<m;i++)
  {
    // put bounds  on constraints
    mosek_guarded(MSK_putbound(task,MSK_ACC_CON,i,MSK_BK_RA,lc[i],uc[i]));
  }

  // Input Q for the objective (REMEMBER Q SHOULD ONLY BE LOWER TRIANGLE)
  mosek_guarded(MSK_putqobj(task,Qv.size(),&Qi[0],&Qj[0],&Qv[0]));

  // Set up task parameters
  for(
    std::map<MSKiparame,int>::iterator pit = mosek_data.intparam.begin();
    pit != mosek_data.intparam.end(); 
    pit++)
  {
    mosek_guarded(MSK_putintparam(task,pit->first,pit->second));
  }
  for(
    std::map<MSKdparame,double>::iterator pit = mosek_data.douparam.begin();
    pit != mosek_data.douparam.end(); 
    pit++)
  {
    mosek_guarded(MSK_putdouparam(task,pit->first,pit->second));
  }

  // Now the optimizer has been prepared
  MSKrescodee trmcode;
  // run the optimizer
  mosek_guarded(MSK_optimizetrm(task,&trmcode));

  //// Print a summary containing information about the solution for debugging
  //// purposes
  //MSK_solutionsummary(task,MSK_STREAM_LOG);

  // Get status of solution
  MSKsolstae solsta;
#if MSK_VERSION_MAJOR >= 7
  MSK_getsolsta (task,MSK_SOL_ITR,&solsta);
#elif MSK_VERSION_MAJOR == 6
  MSK_getsolutionstatus(task,MSK_SOL_ITR,NULL,&solsta);
#endif

  bool success = false;
  switch(solsta)
  {
    case MSK_SOL_STA_OPTIMAL:   
    case MSK_SOL_STA_NEAR_OPTIMAL:
      MSK_getsolutionslice(task,MSK_SOL_ITR,MSK_SOL_ITEM_XX,0,n,&x[0]);
      //printf("Optimal primal solution\n");
      //for(size_t j=0; j<n; ++j)
      //{
      //  printf("x[%ld]: %g\n",j,x[j]);
      //}
      success = true;
      break;
    case MSK_SOL_STA_DUAL_INFEAS_CER:
    case MSK_SOL_STA_PRIM_INFEAS_CER:
    case MSK_SOL_STA_NEAR_DUAL_INFEAS_CER:
    case MSK_SOL_STA_NEAR_PRIM_INFEAS_CER:  
      //printf("Primal or dual infeasibility certificate found.\n");
      break;
    case MSK_SOL_STA_UNKNOWN:
      //printf("The status of the solution could not be determined.\n");
      break;
    default:
      //printf("Other solution status.");
      break;
  }

  MSK_deletetask(&task);
  MSK_deleteenv(&env);

  return success;
}

IGL_INLINE bool igl::mosek::mosek_quadprog(
  const Eigen::SparseMatrix<double> & Q,
  const Eigen::VectorXd & c,
  const double cf,
  const Eigen::SparseMatrix<double> & A,
  const Eigen::VectorXd & lc,
  const Eigen::VectorXd & uc,
  const Eigen::VectorXd & lx,
  const Eigen::VectorXd & ux,
  MosekData & mosek_data,
  Eigen::VectorXd & x)
{
  using namespace Eigen;
  using namespace std;

  typedef int Index;
  typedef double Scalar;
  // Q should be square
  assert(Q.rows() == Q.cols());
  // Q should be symmetric
#ifdef EIGEN_HAS_A_BUG_AND_FAILS_TO_LET_ME_COMPUTE_Q_MINUS_Q_TRANSPOSE
  assert( (Q-Q.transpose()).sum() < FLOAT_EPS);
#endif
  // Only keep lower triangular part of Q
  SparseMatrix<Scalar> QL;
  //QL = Q.template triangularView<Lower>();
  QL = Q.triangularView<Lower>();
  VectorXi Qi,Qj;
  VectorXd Qv;
  find(QL,Qi,Qj,Qv);
  vector<Index> vQi = matrix_to_list(Qi);
  vector<Index> vQj = matrix_to_list(Qj);
  vector<Scalar> vQv = matrix_to_list(Qv);

  // Convert linear term
  vector<Scalar> vc = matrix_to_list(c);

  assert(lc.size() == A.rows());
  assert(uc.size() == A.rows());
  // Convert A to harwell boeing format
  vector<Scalar> vAv;
  vector<Index> vAr,vAc;
  Index nr;
  harwell_boeing(A,nr,vAv,vAr,vAc);

  assert(lx.size() == Q.rows());
  assert(ux.size() == Q.rows());
  vector<Scalar> vlc = matrix_to_list(lc);
  vector<Scalar> vuc = matrix_to_list(uc);
  vector<Scalar> vlx = matrix_to_list(lx);
  vector<Scalar> vux = matrix_to_list(ux);

  vector<Scalar> vx;
  bool ret = mosek_quadprog<Index,Scalar>(
    Q.rows(),vQi,vQj,vQv,
    vc,
    cf,
    nr,
    vAv, vAr, vAc,
    vlc,vuc,
    vlx,vux,
    mosek_data,
    vx);
  list_to_matrix(vx,x);
  return ret;
}
#ifdef IGL_STATIC_LIBRARY
// Explicit template declarations
#endif
