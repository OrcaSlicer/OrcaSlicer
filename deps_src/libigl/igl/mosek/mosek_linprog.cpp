// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mosek_linprog.h"
#include "../mosek/mosek_guarded.h"
#include "../harwell_boeing.h"
#include <limits>
#include <cmath>
#include <vector>

IGL_INLINE bool igl::mosek::mosek_linprog(
  const Eigen::VectorXd & c,
  const Eigen::SparseMatrix<double> & A,
  const Eigen::VectorXd & lc,
  const Eigen::VectorXd & uc,
  const Eigen::VectorXd & lx,
  const Eigen::VectorXd & ux,
  Eigen::VectorXd & x)
{
  // variables for mosek task, env and result code
  MSKenv_t env;
  // Create the MOSEK environment
  mosek_guarded(MSK_makeenv(&env,NULL));
  // initialize mosek environment
#if MSK_VERSION_MAJOR <= 7
  mosek_guarded(MSK_initenv(env));
#endif
  const bool ret = mosek_linprog(c,A,lc,uc,lx,ux,env,x);
  MSK_deleteenv(&env);
  return ret;
}

IGL_INLINE bool igl::mosek::mosek_linprog(
  const Eigen::VectorXd & c,
  const Eigen::SparseMatrix<double> & A,
  const Eigen::VectorXd & lc,
  const Eigen::VectorXd & uc,
  const Eigen::VectorXd & lx,
  const Eigen::VectorXd & ux,
  const MSKenv_t & env,
  Eigen::VectorXd & x)
{
  // following http://docs.mosek.com/7.1/capi/Linear_optimization.html
  using namespace std;
  // number of constraints
  const int m = A.rows();
  // number of variables
  const int n = A.cols();


  vector<double> vAv;
  vector<int> vAri,vAcp;
  int nr;
  harwell_boeing(A,nr,vAv,vAri,vAcp);

  MSKtask_t task;
  // Create the optimization task
  mosek_guarded(MSK_maketask(env,m,n,&task));
  // no threads
  mosek_guarded(MSK_putintparam(task,MSK_IPAR_NUM_THREADS,1));
  if(m>0)
  {
    // Append 'm' empty constraints, the constrainst will initially have no
    // bounds
    mosek_guarded(MSK_appendcons(task,m));
  }
  mosek_guarded(MSK_appendvars(task,n));

  
  const auto & key = [](const double lxj, const double uxj) ->
    MSKboundkeye
  {
    MSKboundkeye k = MSK_BK_FR;
    if(isfinite(lxj) && isfinite(uxj))
    {
      if(lxj == uxj)
      {
        k = MSK_BK_FX;
      }else{
        k = MSK_BK_RA;
      }
    }else if(isfinite(lxj))
    {
      k = MSK_BK_LO;
    }else if(isfinite(uxj))
    {
      k = MSK_BK_UP;
    }
    return k;
  };

  // loop over variables
  for(int j = 0;j<n;j++)
  {
    if(c.size() > 0)
    {
      // Set linear term c_j in the objective
      mosek_guarded(MSK_putcj(task,j,c(j)));
    }

    // Set constant bounds on variable j
    const double lxj = lx.size()>0?lx[j]:-numeric_limits<double>::infinity();
    const double uxj = ux.size()>0?ux[j]: numeric_limits<double>::infinity();
    mosek_guarded(MSK_putvarbound(task,j,key(lxj,uxj),lxj,uxj));

    if(m>0)
    {
      // Input column j of A
      mosek_guarded(
        MSK_putacol(
          task,
          j,
          vAcp[j+1]-vAcp[j],
          &vAri[vAcp[j]],
          &vAv[vAcp[j]])
        );
    }
  }
  // loop over constraints
  for(int i = 0;i<m;i++)
  {
    // Set constraint bounds for row i
    const double lci = lc.size()>0?lc[i]:-numeric_limits<double>::infinity();
    const double uci = uc.size()>0?uc[i]: numeric_limits<double>::infinity();
    mosek_guarded(MSK_putconbound(task,i,key(lci,uci),lci,uci));
  }

  // Now the optimizer has been prepared
  MSKrescodee trmcode;
  // run the optimizer
  mosek_guarded(MSK_optimizetrm(task,&trmcode));
  // Get status
  MSKsolstae solsta;
  MSK_getsolsta (task,MSK_SOL_ITR,&solsta);
  bool success = false;
  switch(solsta)
  {
    case MSK_SOL_STA_OPTIMAL:   
#if MSK_VERSION_MAJOR <= 8
    case MSK_SOL_STA_NEAR_OPTIMAL:
#endif
      x.resize(n);
      /* Request the basic solution. */ 
      MSK_getxx(task,MSK_SOL_BAS,x.data()); 
      success = true;
      break;
    case MSK_SOL_STA_DUAL_INFEAS_CER:
    case MSK_SOL_STA_PRIM_INFEAS_CER:
#if MSK_VERSION_MAJOR <= 8
    case MSK_SOL_STA_NEAR_DUAL_INFEAS_CER:
    case MSK_SOL_STA_NEAR_PRIM_INFEAS_CER:  
#endif
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
  return success;
}
