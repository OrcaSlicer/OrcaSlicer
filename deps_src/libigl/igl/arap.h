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
  /// Parameters and precomputed values for arap solver.
  ///
  /// \fileinfo
  struct ARAPData
  {
    /// #V size of mesh
    int n;
    /// #V list of group indices (1 to k) for each vertex, such that vertex i
    ///    is assigned to group G(i)
    Eigen::VectorXi G;
    /// type of energy to use
    ARAPEnergyType energy;
    /// whether using dynamics (need to call arap_precomputation after changing)
    bool with_dynamics;
    /// #V by dim list of external forces
    Eigen::MatrixXd f_ext;
    /// #V by dim list of velocities
    Eigen::MatrixXd vel;
    /// dynamics time step
    double h;
    /// "Young's modulus" smaller is softer, larger is more rigid/stiff
    double ym;
    /// maximum inner iterations
    int max_iter;
    /// @private rhs pre-multiplier
    Eigen::SparseMatrix<double> K;
    /// @private mass matrix
    Eigen::SparseMatrix<double> M;
    /// @private covariance scatter matrix
    Eigen::SparseMatrix<double> CSM;
    /// @private quadratic solver data
    min_quad_with_fixed_data<double> solver_data;
    /// @private list of boundary indices into V
    Eigen::VectorXi b;
    /// @private dimension being used for solving
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
  
  /// Compute necessary information to start using an ARAP deformation using
  /// local-global solver as described in "As-rigid-as-possible surface
  /// modeling" [Sorkine and Alexa 2007].
  ///
  /// @param[in] V  #V by dim list of mesh positions
  /// @param[in] F  #F by simplex-size list of triangle|tet indices into V
  /// @param[in] dim  dimension being used at solve time. For deformation usually dim =
  ///    V.cols(), for surface parameterization V.cols() = 3 and dim = 2
  /// @param[in] b  #b list of "boundary" fixed vertex indices into V
  /// @param[out] data  struct containing necessary precomputation
  /// @return whether initialization succeeded
  ///
  /// \fileinfo
  template <
    typename DerivedV,
    typename DerivedF,
    typename Derivedb>
  IGL_INLINE bool arap_precomputation(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const int dim,
    const Eigen::MatrixBase<Derivedb> & b,
    ARAPData & data);
  /// Conduct arap solve.
  ///
  /// @param[in] bc  #b by dim list of boundary conditions
  /// @param[in] data  struct containing necessary precomputation and parameters
  /// @param[in,out] U  #V by dim initial guess
  ///
  /// \fileinfo
  ///
  /// \note While the libigl guidelines require outputs to be of type 
  /// PlainObjectBase so that the user does not need to worry about allocating
  /// memory for the output, in this case, the user is required to give an initial
  /// guess and hence fix the size of the problem domain.
  /// Taking a reference to MatrixBase in this case thus allows the user to provide e.g.
  /// a map to the position data, allowing seamless interoperability with user-defined
  /// datastructures without requiring a copy.
  template <
    typename Derivedbc,
    typename DerivedU>
  IGL_INLINE bool arap_solve(
    const Eigen::MatrixBase<Derivedbc> & bc,
    ARAPData & data,
    Eigen::MatrixBase<DerivedU> & U);
};

#ifndef IGL_STATIC_LIBRARY
#include "arap.cpp"
#endif

#endif
