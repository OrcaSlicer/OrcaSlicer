// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MOSEK_MOSEK_LINPROG_H
#define IGL_MOSEK_MOSEK_LINPROG_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
#include <mosek.h>
namespace igl
{
  namespace mosek
  {
    /// Solve a linear program using mosek. Given in the form:
    /// 
    ///     min c'x
    ///     s.t. lc <= A x <= uc
    ///          lx <= x <= ux
    ///
    /// @param[in] c  #x list of linear objective coefficients
    /// @param[in] A  #A by #x matrix of linear inequality constraint coefficients
    /// @param[in] lc  #A list of lower constraint bounds
    /// @param[in] uc  #A list of upper constraint bounds
    /// @param[in] lx  #x list of lower variable bounds
    /// @param[in] ux  #x list of upper variable bounds
    /// @param[out] x  #x list of solution values
    /// @return true iff success.
    IGL_INLINE bool mosek_linprog(
        const Eigen::VectorXd & c,
        const Eigen::SparseMatrix<double> & A,
        const Eigen::VectorXd & lc,
        const Eigen::VectorXd & uc,
        const Eigen::VectorXd & lx,
        const Eigen::VectorXd & ux,
        Eigen::VectorXd & x);
    /// \overload
    ///
    /// \brief Wrapper that keeps mosek environment alive (if licence checking is
    /// becoming a bottleneck)
    ///
    /// @param[in] env  mosek environment
    IGL_INLINE bool mosek_linprog(
        const Eigen::VectorXd & c,
        const Eigen::SparseMatrix<double> & A,
        const Eigen::VectorXd & lc,
        const Eigen::VectorXd & uc,
        const Eigen::VectorXd & lx,
        const Eigen::VectorXd & ux,
        const MSKenv_t & env,
        Eigen::VectorXd & x);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "mosek_linprog.cpp"
#endif
#endif
