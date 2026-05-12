// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_KKT_INVERSE_H
#define IGL_KKT_INVERSE_H
#include "igl_inline.h"

#include <Eigen/Dense>

//// debug
//#include <matlabinterface.h>
//Engine *g_pEngine;


namespace igl
{
  /// Constructs the inverse of the KKT matrix of a convex, linear equality
  /// constrained quadratic minimization problem.
  ///
  /// Systems of the form:
  ///
  ///      ╱ A   Aeqᵀ ╲  ╱ x ╲ = ╱ b   ╲
  ///      ╲ Aeq    0 ╱  ╲ λ ╱   ╲ beq ╱
  ///     ╲_____.______╱╲__.__╱ ╲___.___╱
  ///           M          z        c
  ///
  /// Arise, for example, when solve convex, linear equality constrained
  /// quadratic minimization problems:
  ///
  ///     min ½ xᵀ A x - xᵀb  subject to Aeq x = beq
  ///
  /// This function constructs a matrix S such that x = S c solves the system
  /// above. That is:
  ///
  ///     S = [In 0] M⁻¹
  ///
  ///     so that
  ///
  ///     x = S c
  ///
  /// @tparam  T  should be a eigen matrix primitive type like float or double
  /// @param[in] A  n by n matrix of quadratic coefficients
  /// @param[in] B  n by 1 column of linear coefficients
  /// @param[in] Aeq  m by n list of linear equality constraint coefficients
  /// @param[in] Beq  m by 1 list of linear equality constraint constant values
  /// @param[in] use_lu_decomposition  use lu rather than SVD
  /// @param[out] S  n by (n + m) "solve" matrix, such that S*[B', Beq'] is a solution
  /// @return true on success, false on error
  template <typename T>
  IGL_INLINE void kkt_inverse(
    const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& A,
    const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& Aeq,
    const bool use_lu_decomposition,
    Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>& S);
}

#ifndef IGL_STATIC_LIBRARY
#  include "kkt_inverse.cpp"
#endif

#endif
