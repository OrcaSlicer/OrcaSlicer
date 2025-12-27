// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LIM_LIM_H
#define IGL_LIM_LIM_H
#include <igl/igl_inline.h>
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  namespace lim
  {
    // Computes a locally injective mapping of a triangle or tet-mesh based on
    // a deformation energy subject to some provided linear positional
    // constraints Cv-d.
    //
    // Inputs:
    //   vertices          vx3 matrix containing vertex position of the mesh
    //   initialVertices   vx3 matrix containing vertex position of initial
    //                     rest pose mesh
    //   elements          exd matrix containing vertex indices of all elements
    //   borderVertices    (only needed for 2D LSCM) vector containing indices
    //                     of border vertices
    //   gradients         (only needed for 2D Poisson) vector containing
    //                     partial derivatives of target element gradients
    //                     (structure is: [xx_0, xy_0, xx_1, xy_1, ..., xx_v,
    //                     xy_v, yx_0, yy_0, yx_1, yy_1, ..., yx_v, yy_v]')
    //   constraintMatrix  C: (c)x(v*(d-1)) sparse linear positional constraint
    //                     matrix. X an Y-coordinates are alternatingly stacked
    //                     per row (structure for triangles: [x_1, y_1, x_2,
    //                     y_2, ..., x_v,y_v])
    //   constraintTargets d: c vector target positions
    //   energyType        type of used energy:
    //                     Dirichlet, Laplacian, Green, ARAP, LSCM, Poisson (only 2D), UniformLaplacian, Identity
    //   tolerance         max squared positional constraints error
    //   maxIteration      max number of iterations
    //   findLocalMinima   iterating until a local minima is found. If not
    //                     enabled only tolerance must be fulfilled.
    //   enableOutput      (optional) enables the output (#iteration / hessian correction / step size / positional constraints / barrier constraints / deformation energy) (default : true)
    //   enableBarriers    (optional) enables the non-flip constraints (default = true)
    //   enableAlphaUpdate (optional) enables dynamic alpha weight adjustment (default = true)
    //   beta              (optional) steepness factor of barrier slopes (default: ARAP/LSCM = 0.01, Green = 1)
    //   eps               (optional) smallest valid triangle area (default: 1e-5 * smallest triangle)
    //
    // where:
    //   v : # vertices
    //   c : # linear constraints
    //   e : # elements of mesh
    //   d : # vertices per element (triangle = 3, tet = 4)
    //--------------------------------------------------------------------------
    // Output:
    // vertices          vx3 matrix containing resulting vertex position of the
    //                   mesh
    //--------------------------------------------------------------------------
    // Return values:
    //  Succeeded : Successful optimization with fulfilled tolerance
    //  LocalMinima : Convergenged to a local minima / tolerance not fulfilled
    //  IterationLimit : Max iteration reached before tolerance was fulfilled
    //  Infeasible : not feasible -> has inverted elements (decrease eps?)
  
    enum Energy { Dirichlet = 0, Laplacian=1, Green=2, ARAP=3, LSCM=4, Poisson=5, UniformLaplacian=6, Identity=7 };
    enum State { Uninitialized = -4, Infeasible = -3, IterationLimit = -2, LocalMinima = -1, Running = 0, Succeeded = 1 };

    State lim(
      Eigen::Matrix<double,Eigen::Dynamic,3>& vertices,
      const Eigen::Matrix<double,Eigen::Dynamic,3>& initialVertices,
      const Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic>& elements,
      const Eigen::SparseMatrix<double>& constraintMatrix,
      const Eigen::Matrix<double,Eigen::Dynamic,1>& constraintTargets,
      Energy energyType,
      double tolerance,
      int maxIteration,
      bool findLocalMinima);
  
    State lim(
      Eigen::Matrix<double,Eigen::Dynamic,3>& vertices,
      const Eigen::Matrix<double,Eigen::Dynamic,3>& initialVertices,
      const Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic>& elements,
      const Eigen::SparseMatrix<double>& constraintMatrix,
      const Eigen::Matrix<double,Eigen::Dynamic,1>& constraintTargets,
      Energy energyType,
      double tolerance,
      int maxIteration,
      bool findLocalMinima,
      bool enableOuput,
      bool enableBarriers,
      bool enableAlphaUpdate,
      double beta,
      double eps);
  
    State lim(
      Eigen::Matrix<double,Eigen::Dynamic,3>& vertices,
      const Eigen::Matrix<double,Eigen::Dynamic,3>& initialVertices,
      const Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic>& elements,
      const std::vector<int>& borderVertices,
      const Eigen::Matrix<double,Eigen::Dynamic,1>& gradients,
      const Eigen::SparseMatrix<double>& constraintMatrix,
      const Eigen::Matrix<double,Eigen::Dynamic,1>& constraintTargets,
      Energy energyType,
      double tolerance,
      int maxIteration,
      bool findLocalMinima);
  
    State lim(
      Eigen::Matrix<double,Eigen::Dynamic,3>& vertices,
      const Eigen::Matrix<double,Eigen::Dynamic,3>& initialVertices,
      const Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic>& elements,
      const std::vector<int>& borderVertices,
      const Eigen::Matrix<double,Eigen::Dynamic,1>& gradients,
      const Eigen::SparseMatrix<double>& constraintMatrix,
      const Eigen::Matrix<double,Eigen::Dynamic,1>& constraintTargets,
      Energy energyType,
      double tolerance,
      int maxIteration,
      bool findLocalMinima,
      bool enableOuput,
      bool enableBarriers,
      bool enableAlphaUpdate,
      double beta,
      double eps);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "lim.cpp"
#endif

#endif
