// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MATLAB_MATLAB_INTERFACE_H
#define IGL_MATLAB_MATLAB_INTERFACE_H
#include "../igl_inline.h"
// WARNING: These functions require matlab installed
// Additional header folder required:
//   /Applications/MATLAB_R2011a.app/extern/include
// Additional binary lib to be linked with:
// /Applications/MATLAB_R2011a.app/bin/maci64/libeng.dylib
// /Applications/MATLAB_R2011a.app/bin/maci64/libmx.dylib

// MAC ONLY:
// Add to the environment variables:
// DYLD_LIBRARY_PATH = /Applications/MATLAB_R2011a.app/bin/maci64/
// PATH = /opt/local/bin:/opt/local/sbin:/Applications/MATLAB_R2011a.app/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/usr/texbin:/usr/X11/bin

#include <Eigen/Core>
#include <Eigen/Sparse>
#include <string>

#include <complex>
#include <cassert>
#include <map>
#include <string>
#include <vector>

#include <engine.h>  // Matlab engine header

namespace igl
{
  namespace matlab
  {
    // Init the MATLAB engine
    // (no need to call it directly since it is automatically invoked by any other command)
    IGL_INLINE void mlinit(Engine** engine);
  
    // Closes the MATLAB engine
    IGL_INLINE void mlclose(Engine** engine);
  
    // Send a matrix to MATLAB
    IGL_INLINE void mlsetmatrix(Engine** engine, std::string name, const Eigen::MatrixXd& M);
  
    // Send a matrix to MATLAB
    IGL_INLINE void mlsetmatrix(Engine** engine, std::string name, const Eigen::MatrixXf& M);
  
    // Send a matrix to MATLAB
    IGL_INLINE void mlsetmatrix(Engine** engine, std::string name, const Eigen::MatrixXi& M);
  
    // Send a matrix to MATLAB
    IGL_INLINE void mlsetmatrix(Engine** mlengine, std::string name, const Eigen::Matrix<unsigned int, Eigen::Dynamic, Eigen::Dynamic >& M);
  
    // Receive a matrix from MATLAB
    IGL_INLINE void mlgetmatrix(Engine** engine, std::string name, Eigen::MatrixXd& M);
  
    // Receive a matrix from MATLAB
    IGL_INLINE void mlgetmatrix(Engine** engine, std::string name, Eigen::MatrixXf& M);
  
    // Receive a matrix from MATLAB
    IGL_INLINE void mlgetmatrix(Engine** engine, std::string name, Eigen::MatrixXi& M);
  
    // Receive a matrix from MATLAB
    IGL_INLINE void mlgetmatrix(Engine** mlengine, std::string name, Eigen::Matrix<unsigned int, Eigen::Dynamic, Eigen::Dynamic >& M);
  
    // Send a single scalar to MATLAB
    IGL_INLINE void mlsetscalar(Engine** engine, std::string name, double s);
  
    // Receive a single scalar from MATLAB
    IGL_INLINE double mlgetscalar(Engine** engine, std::string name);
  
    // Execute arbitrary MATLAB code and return the MATLAB output
    IGL_INLINE std::string mleval(Engine** engine, std::string code);
  
    // Send a sparse matrix to MATLAB
    IGL_INLINE void mlsetmatrix(Engine** mlengine, std::string name, const Eigen::SparseMatrix<double>& M);
  
  }
}

// Be sure that this is not compiled into libigl.a
#ifndef IGL_STATIC_LIBRARY
#  include "matlabinterface.cpp"
#endif

#endif
