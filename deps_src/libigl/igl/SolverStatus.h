// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SOLVER_STATUS_H
#define IGL_SOLVER_STATUS_H
namespace igl
{
  enum SolverStatus
  {
    // Good
    SOLVER_STATUS_CONVERGED = 0,
    // OK
    SOLVER_STATUS_MAX_ITER = 1,
    // Bad
    SOLVER_STATUS_ERROR = 2,
    NUM_SOLVER_STATUSES = 3,
  };
};
#endif
