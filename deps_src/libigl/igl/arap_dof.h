// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ARAP_ENERGY_TYPE_DOF_H
#define IGL_ARAP_ENERGY_TYPE_DOF_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include "ARAPEnergyType.h"
#include <vector>

namespace igl
{
  // Caller example:
  //
  // Once:
  // arap_dof_precomputation(...)
  //
  // Each frame:
  // while(not satisfied)
  //   arap_dof_update(...)
  // end
  
  template <typename LbsMatrixType, typename SSCALAR>
  struct ArapDOFData;
  
  ///////////////////////////////////////////////////////////////////////////
  //
  // Arap DOF precomputation consists of two parts the computation. The first is
  // that which depends solely on the mesh (V,F), the linear blend skinning
  // weights (M) and the groups G. Then there's the part that depends on the
  // previous precomputation and the list of free and fixed vertices. 
  //
  ///////////////////////////////////////////////////////////////////////////
  
  
  // The code and variables differ from the description in Section 3 of "Fast
  // Automatic Skinning Transformations" by [Jacobson et al. 2012]
  // 
  // Here is a useful conversion table:
  //
  // [article]                             [code]
  // S = \tilde{K} T                       S = CSM * Lsep
  // S --> R                               S --> R --shuffled--> Rxyz
  // Gamma_solve RT = Pi_1 \tilde{K} RT    L_part1xyz = CSolveBlock1 * Rxyz 
  // Pi_1 \tilde{K}                        CSolveBlock1
  // Peq = [T_full; P_pos]                 
  // T_full                                B_eq_fix <--- L0
  // P_pos                                 B_eq
  // Pi_2 * P_eq =                         Lpart2and3 = Lpart2 + Lpart3
  //   Pi_2_left T_full +                  Lpart3 = M_fullsolve(right) * B_eq_fix
  //   Pi_2_right P_pos                    Lpart2 = M_fullsolve(left) * B_eq
  // T = [Pi_1 Pi_2] [\tilde{K}TRT P_eq]   L = Lpart1 + Lpart2and3
  //
  
  // Precomputes the system we are going to optimize. This consists of building
  // constructor matrices (to compute covariance matrices from transformations
  // and to build the poisson solve right hand side from rotation matrix entries)
  // and also prefactoring the poisson system.
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   F  #F by {3|4} list of face indices
  //   M  #V * dim by #handles * dim * (dim+1) matrix such that
  //     new_V(:) = LBS(V,W,A) = reshape(M * A,size(V)), where A is a column
  //     vectors formed by the entries in each handle's dim by dim+1 
  //     transformation matrix. Specifcally, A =
  //       reshape(permute(Astack,[3 1 2]),n*dim*(dim+1),1)
  //     or A = [Lxx;Lyx;Lxy;Lyy;tx;ty], and likewise for other dim
  //     if Astack(:,:,i) is the dim by (dim+1) transformation at handle i
  //     handles are ordered according to P then BE (point handles before bone
  //     handles)
  //   G  #V list of group indices (1 to k) for each vertex, such that vertex i 
  //     is assigned to group G(i)
  // Outputs:
  //   data  structure containing all necessary precomputation for calling
  //     arap_dof_update
  // Returns true on success, false on error
  //
  // See also: lbs_matrix_column
  template <typename LbsMatrixType, typename SSCALAR>
  IGL_INLINE bool arap_dof_precomputation(
    const Eigen::MatrixXd & V, 
    const Eigen::MatrixXi & F,
    const LbsMatrixType & M,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & G,
    ArapDOFData<LbsMatrixType, SSCALAR> & data);
  
  // Should always be called after arap_dof_precomputation, but may be called in
  // between successive calls to arap_dof_update, recomputes precomputation
  // given that there are only changes in free and fixed
  //
  // Inputs:
  //   fixed_dim  list of transformation element indices for fixed (or partailly
  //   fixed) handles: not necessarily the complement of 'free'
  //    NOTE: the constraints for fixed transformations still need to be
  //    present in A_eq
  //   A_eq  dim*#constraint_points by m*dim*(dim+1)  matrix of linear equality
  //     constraint coefficients. Each row corresponds to a linear constraint,
  //     so that A_eq * L = Beq says that the linear transformation entries in
  //     the column L should produce the user supplied positional constraints
  //     for each handle in Beq. The row A_eq(i*dim+d) corresponds to the
  //     constrain on coordinate d of position i
  // Outputs:
  //   data  structure containing all necessary precomputation for calling
  //     arap_dof_update
  // Returns true on success, false on error
  //
  // See also: lbs_matrix_column
  template <typename LbsMatrixType, typename SSCALAR>
  IGL_INLINE bool arap_dof_recomputation(
    const Eigen::Matrix<int,Eigen::Dynamic,1> & fixed_dim,
    const Eigen::SparseMatrix<double> & A_eq,
    ArapDOFData<LbsMatrixType, SSCALAR> & data);
  
  // Optimizes the transformations attached to each weight function based on
  // precomputed system.
  //
  // Inputs:
  //   data  precomputation data struct output from arap_dof_precomputation
  //   Beq  dim*#constraint_points constraint values.
  //   L0  #handles * dim * dim+1 list of initial guess transformation entries,
  //     also holds fixed transformation entries for fixed handles
  //   max_iters  maximum number of iterations
  //   tol  stopping criteria parameter. If variables (linear transformation
  //     matrix entries) change by less than 'tol' the optimization terminates,
  //       0.75 (weak tolerance)
  //       0.0 (extreme tolerance)
  // Outputs:
  //   L  #handles * dim * dim+1 list of final optimized transformation entries,
  //     allowed to be the same as L
  template <typename LbsMatrixType, typename SSCALAR>
  IGL_INLINE bool arap_dof_update(
    const ArapDOFData<LbsMatrixType,SSCALAR> & data,
    const Eigen::Matrix<double,Eigen::Dynamic,1> & B_eq,
    const Eigen::MatrixXd & L0,
    const int max_iters,
    const double tol,
    Eigen::MatrixXd & L
    );
  
  // Structure that contains fields for all precomputed data or data that needs
  // to be remembered at update
  template <typename LbsMatrixType, typename SSCALAR>
  struct ArapDOFData
  {
    typedef Eigen::Matrix<SSCALAR, Eigen::Dynamic, Eigen::Dynamic> MatrixXS;
    // Type of arap energy we're solving
    igl::ARAPEnergyType energy;
    //// LU decomposition precomptation data; note: not used by araf_dop_update
    //// any more, replaced by M_FullSolve
    //igl::min_quad_with_fixed_data<double> lu_data;
    // List of indices of fixed transformation entries
    Eigen::Matrix<int,Eigen::Dynamic,1> fixed_dim;
    // List of precomputed covariance scatter matrices multiplied by lbs
    // matrices
    //std::vector<Eigen::SparseMatrix<double> > CSM_M;
    std::vector<Eigen::MatrixXd> CSM_M;
    LbsMatrixType M_KG;
    // Number of mesh vertices
    int n;
    // Number of weight functions
    int m;
    // Number of dimensions
    int dim;
    // Effective dimensions
    int effective_dim;
    // List of indices into C of positional constraints
    Eigen::Matrix<int,Eigen::Dynamic,1> interpolated;
    std::vector<bool> free_mask;
    // Full quadratic coefficients matrix before lagrangian (should be dense)
    LbsMatrixType Q;
  
  
    //// Solve matrix for the global step
    //Eigen::MatrixXd M_Solve; // TODO: remove from here
  
    // Full solve matrix that contains also conversion from rotations to the right hand side, 
    // i.e., solves Poisson transformations just from rotations and positional constraints
    MatrixXS M_FullSolve;
  
    // Precomputed condensed matrices (3x3 commutators folded to 1x1):
    MatrixXS CSM;
    MatrixXS CSolveBlock1;
  
    // Print timings at each update
    bool print_timings;
  
    // Dynamics
    bool with_dynamics;
    // I'm hiding the extra dynamics stuff in this struct, which sort of defeats
    // the purpose of this function-based coding style...
  
    // Time step
    double h;
  
    // L0  #handles * dim * dim+1 list of transformation entries from
    // previous solve
    MatrixXS L0;
    //// Lm1  #handles * dim * dim+1 list of transformation entries from
    //// previous-previous solve
    //MatrixXS Lm1;
    // "Velocity"
    MatrixXS Lvel0;
  
    // #V by dim matrix of external forces
    // fext
    MatrixXS fext;
  
    // Mass_tilde: MT * Mass * M
    LbsMatrixType Mass_tilde;
  
    // Force due to gravity (premultiplier)
    Eigen::MatrixXd fgrav;
    // Direction of gravity
    Eigen::Vector3d grav_dir;
    // Magnitude of gravity
    double grav_mag;
    
    // Π1 from the paper
    MatrixXS Pi_1;
  
    // Default values
    ArapDOFData(): 
      energy(igl::ARAP_ENERGY_TYPE_SPOKES), 
      with_dynamics(false),
      h(1),
      grav_dir(0,-1,0),
      grav_mag(0)
    {
    }
  };
}

#ifndef IGL_STATIC_LIBRARY
#  include "arap_dof.cpp"
#endif

#endif
