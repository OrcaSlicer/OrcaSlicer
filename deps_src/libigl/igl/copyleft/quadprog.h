// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_QUADPROG_H
#define IGL_COPYLEFT_QUADPROG_H

#include "../igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  namespace copyleft
  {
    // Solve a (dense) quadratric program of the form:
    //
    //   min  0.5 x G x + g0 x
    //   s.t. CE' x + ce0  = 0
    //   and  CI' x + ci0 >= 0
    //
    // Inputs:
    //   G  #x by #x matrix of quadratic coefficients
    //   g0  #x vector of linear coefficients
    //   CE #x by #CE list of linear equality coefficients
    //   ce0 #CE list of linear equality right-hand sides
    //   CI #x by #CI list of linear equality coefficients
    //   ci0 #CI list of linear equality right-hand sides
    // Outputs:
    //   x  #x vector of solution values
    // Returns true iff success
    IGL_INLINE bool quadprog(
      const Eigen::MatrixXd & G,  
      const Eigen::VectorXd & g0,  
      const Eigen::MatrixXd & CE, 
      const Eigen::VectorXd & ce0,  
      const Eigen::MatrixXd & CI, 
      const Eigen::VectorXd & ci0, 
      Eigen::VectorXd& x);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "quadprog.cpp"
#endif

#endif
