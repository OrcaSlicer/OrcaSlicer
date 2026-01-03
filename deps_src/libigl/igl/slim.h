// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef SLIM_H
#define SLIM_H

#include "igl_inline.h"
#include "MappingEnergyType.h"
#include <Eigen/Dense>
#include <Eigen/Sparse>

// This option makes the iterations faster (all except the first) by caching the
// sparsity pattern of the matrix involved in the assembly. It should be on if
// you plan to do many iterations, off if you have to change the matrix
// structure at every iteration.
#define SLIM_CACHED 

#ifdef SLIM_CACHED
#include "AtA_cached.h"
#endif

namespace igl
{

/// Parameters and precomputed data for computing a SLIM map as derived in
/// "Scalable Locally Injective Maps" [Rabinovich et al. 2016].
///
/// \fileinfo
struct SLIMData
{
  /// #V by 3 list of mesh vertex positions
  Eigen::MatrixXd V; 
  /// #F by 3/4 list of mesh faces (triangles/tets)
  Eigen::MatrixXi F; 
  /// Mapping energy type
  MappingEnergyType slim_energy;

  // Optional Input
  /// Fixed indices
  Eigen::VectorXi b;
  /// Fixed values
  Eigen::MatrixXd bc;
  /// Weight for enforcing fixed values as soft constraint
  double soft_const_p;

  /// used for exponential energies, ignored otherwise
  double exp_factor; 
  /// only supported for 3d
  bool mesh_improvement_3d; 

  // Output
  /// #V by dim list of mesh vertex positions (dim = 2 for parametrization, 3 otherwise)
  Eigen::MatrixXd V_o; 
  /// objective value
  double energy; 

  // INTERNAL
  Eigen::VectorXd M;
  double mesh_area;
  double avg_edge_length;
  int v_num;
  int f_num;
  double proximal_p;

  Eigen::VectorXd WGL_M;
  Eigen::VectorXd rhs;
  Eigen::MatrixXd Ri,Ji;
  Eigen::MatrixXd W;
  Eigen::SparseMatrix<double> Dx,Dy,Dz;
  int f_n,v_n;
  bool first_solve;
  bool has_pre_calc = false;
  int dim;

  #ifdef SLIM_CACHED
  Eigen::SparseMatrix<double> A;
  Eigen::VectorXi A_data;
  Eigen::SparseMatrix<double> AtA;
  igl::AtA_cached_data AtA_data;
  #endif
};

/// Compute necessary information to start using SLIM
/// 
/// @param[in] V           #V by 3 list of mesh vertex positions
/// @param[in] F           #F by (3|4) list of mesh faces (triangles/tets)
/// @param[in] V_init      #V by 3 list of initial mesh vertex positions
/// @param[in,out] data        Precomputation data structure
/// @param[in] slim_energy Energy to minimize
/// @param[in] b           list of boundary indices into V
/// @param[in] bc          #b by dim list of boundary conditions
/// @param[in] soft_p      Soft penalty factor (can be zero)
///
/// \fileinfo
IGL_INLINE void slim_precompute(
  const Eigen::MatrixXd& V,
  const Eigen::MatrixXi& F,
  const Eigen::MatrixXd& V_init,
  SLIMData& data,
  MappingEnergyType slim_energy,
  const Eigen::VectorXi& b,
  const Eigen::MatrixXd& bc,
  double soft_p);

/// Run iter_num iterations of SLIM
///
/// @param[in,out] data   Precomputation data structure
/// @param[in] iter_num   Number of iterations to run
/// @return #V by dim list of mesh vertex positions
///
/// \fileinfo
IGL_INLINE Eigen::MatrixXd slim_solve(
  SLIMData& data, 
  int iter_num);

/// Internal Routine. Exposed for Integration with SCAF
///
/// @param[in] Ji  ?? by ?? list of Jacobians??
/// @param[in] slim_energy Energy to minimize
/// @param[in] exp_factor   ??? used for exponential energies, ignored otherwise
/// @param[out] W  ?? by ?? list of weights??
/// @param[out] Ri ?? by ?? list of rotations??
///
/// \fileinfo
IGL_INLINE void slim_update_weights_and_closest_rotations_with_jacobians(
  const Eigen::MatrixXd &Ji,
  igl::MappingEnergyType slim_energy,
  double exp_factor,
  Eigen::MatrixXd &W,
  Eigen::MatrixXd &Ri);

/// Undocumented function related to SLIM optimization
///
/// @param[in] Dx  ?? by ?? matrix to compute of x derivatives?
/// @param[in] Dy  ?? by ?? matrix to compute of y derivatives?
/// @param[in] Dz  ?? bz ?? matrix to compute of z derivatives?
/// @param[in] W  ?? by ?? list of weights??
/// @param[out] IJV  ?? by ?? list of triplets to some A matrix??
IGL_INLINE void slim_buildA(
  const Eigen::SparseMatrix<double> &Dx,
  const Eigen::SparseMatrix<double> &Dy,
  const Eigen::SparseMatrix<double> &Dz,
  const Eigen::MatrixXd &W,
  std::vector<Eigen::Triplet<double> > & IJV);
}

#ifndef IGL_STATIC_LIBRARY
#  include "slim.cpp"
#endif

#endif
