// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "lim.h"
#include <LIMSolverInterface.h>


IGL_INLINE igl::lim::State igl::lim::lim(
  Eigen::Matrix<double,Eigen::Dynamic,3>& vertices,
  const Eigen::Matrix<double,Eigen::Dynamic,3>& initialVertices,
  const Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic>& elements,
  const Eigen::SparseMatrix<double>& constraintMatrix,
  const Eigen::Matrix<double,Eigen::Dynamic,1>& constraintTargets,
  Energy energyType,
  double tolerance,
  int maxIteration,
  bool findLocalMinima)
{
  return (State)ComputeLIM(
    vertices,
    initialVertices,
    elements,
    constraintMatrix,
    constraintTargets,
    (EnergyType)energyType,
    tolerance,
    maxIteration,
    findLocalMinima
    );
}

IGL_INLINE igl::lim::State igl::lim::lim(
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
  double eps)
{
  return (State)ComputeLIM(
    vertices,
    initialVertices,
    elements,
    constraintMatrix,
    constraintTargets,
    (EnergyType)energyType,
    tolerance,
    maxIteration,
    findLocalMinima,
    enableOuput,
    enableBarriers,
    enableAlphaUpdate,
    beta,
    eps
    );
}

IGL_INLINE igl::lim::State igl::lim::lim(
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
  bool findLocalMinima)
{
  return (State)ComputeLIM(
    vertices,
    initialVertices,
    elements,
    borderVertices,
    gradients,
    constraintMatrix,
    constraintTargets,
    (EnergyType)energyType,
    tolerance,
    maxIteration,
    findLocalMinima
    );
}

IGL_INLINE igl::lim::State igl::lim::lim(
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
  double eps)
{
  return (State)ComputeLIM(
    vertices,
    initialVertices,
    elements,
    borderVertices,
    gradients,
    constraintMatrix,
    constraintTargets,
    (EnergyType)energyType,
    tolerance,
    maxIteration,
    findLocalMinima,
    enableOuput,
    enableBarriers,
    enableAlphaUpdate,
    beta,
    eps);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
