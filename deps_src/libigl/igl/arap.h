// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ARAP_H
#define IGL_ARAP_H
#include "igl_inline.h"
#include "min_quad_with_fixed.h"
#include "ARAPEnergyType.h"
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  struct ARAPData
  {
    // n  #V
    // G  #V list of group indices (1 to k) for each vertex, such that vertex i
    //    is assigned to group G(i)
    // energy  type of energy to use
    // with_dynamics  whether using dynamics (need to call arap_precomputation
    //   after changing)
    // f_ext  #V by dim list of external forces
    // vel  #V by dim list of velocities
    // h  dynamics time step
    // ym  ~Young's modulus smaller is softer, larger is more rigid/stiff
    // max_iter  maximum inner iterations
    // K  rhs pre-multiplier
    // M  mass matrix
    // solver_data  quadratic solver data
    // b  list of boundary indices into V
    // dim  dimension being used for solving
    int n;
    Eigen::VectorXi G;
    ARAPEnergyType energy;
    bool with_dynamics;
    Eigen::MatrixXd f_ext,vel;
    double h;
    double ym;
    int max_iter;
    Eigen::SparseMatrix<double> K,M;
    Eigen::SparseMatrix<double> CSM;
    min_quad_with_fixed_data<double> solver_data;
    Eigen::VectorXi b;
    int dim;
      ARAPData():
        n(0),
        G(),
        energy(ARAP_ENERGY_TYPE_DEFAULT),
        with_dynamics(false),
        f_ext(),
        h(1),
        ym(1),
        max_iter(10),
        K(),
        CSM(),
        solver_data(),
        b(),
        dim(-1) // force this to be set by _precomputation
    {
    };
  };
  
  // Compute necessary information to start using an ARAP deformation
  //
  // Inputs:
  //   V  #V by dim list of mesh positions
  //   F  #F by simplex-size list of triangle|tet indices into V
  //   dim  dimension being used at solve time. For deformation usually dim =
  //     V.cols(), for surface parameterization V.cols() = 3 and dim = 2
  //   b  #b list of "boundary" fixed vertex indices into V
  // Outputs:
  //   data  struct containing necessary precomputation
  template <
    typename DerivedV,
    typename DerivedF,
    typename Derivedb>
  IGL_INLINE bool arap_precomputation(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const int dim,
    const Eigen::PlainObjectBase<Derivedb> & b,
    ARAPData & data);
  // Inputs:
  //   bc  #b by dim list of boundary conditions
  //   data  struct containing necessary precomputation and parameters
  //   U  #V by dim initial guess
  template <
    typename Derivedbc,
    typename DerivedU>
  IGL_INLINE bool arap_solve(
    const Eigen::PlainObjectBase<Derivedbc> & bc,
    ARAPData & data,
    Eigen::PlainObjectBase<DerivedU> & U);
};

#ifndef IGL_STATIC_LIBRARY
#include "arap.cpp"
#endif

#endif
