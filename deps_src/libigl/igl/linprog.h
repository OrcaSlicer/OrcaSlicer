// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LINPROG_H
#define IGL_LINPROG_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Solve a linear program given in "standard form"
  ///
  ///      min  c'x
  ///      s.t. A(    1:k,:) x <= b(1:k)
  ///           A(k+1:end,:) x = b(k+1:end)
  ///        ** x >= 0 **
  ///
  /// In contrast to other APIs the entries in b may be negative.
  ///
  /// @param[in] c  #x list of linear coefficients
  /// @param[in] A  #A by #x matrix of linear constraint coefficients
  /// @param[in] b  #A list of linear constraint right-hand sides
  /// @param[in] k  number of inequality constraints as first rows of A,b
  /// @param[out] x  #x solution vector
  /// @return false on failure or detected infeasibility, returns true on termination
  ///
  /// \note It appears that this implementation does not detect all infeasibile
  /// problems (e.g., https://github.com/libigl/libigl/issues/2051). Therefor,
  /// it's worth double-checking that the output actually satisfies the
  /// constraints even if the return value is `true`.
  IGL_INLINE bool linprog(
    const Eigen::VectorXd & c,
    const Eigen::MatrixXd & A,
    const Eigen::VectorXd & b,
    const int k,
    Eigen::VectorXd & x);
  /// \overload
  /// \brief Wrapper in friendlier general form (no implicit bounds on x)
  ///
  ///     min  f'x
  ///     s.t. A x <= b
  ///          B x = c
  ///
  /// @param[in] f  #x list of linear coefficients
  /// @param[in] A  #A by #x matrix of linear inequality constraint coefficients
  /// @param[in] b  #A list of linear constraint right-hand sides
  /// @param[in] B  #B by #x matrix of linear equality constraint coefficients
  /// @param[in] c  #B list of linear constraint right-hand sides
  /// @param[out] x  #x solution vector
  /// @return false on failure or detected infeasibility, returns true on termination
  ///
  IGL_INLINE bool linprog(
    const Eigen::VectorXd & f,
    const Eigen::MatrixXd & A,
    const Eigen::VectorXd & b,
    const Eigen::MatrixXd & B,
    const Eigen::VectorXd & c,
    Eigen::VectorXd & x);
}

#ifndef IGL_STATIC_LIBRARY
#  include "linprog.cpp"
#endif
#endif
