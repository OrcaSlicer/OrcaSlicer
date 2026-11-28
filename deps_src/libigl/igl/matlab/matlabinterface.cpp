// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "matlabinterface.h"

// Implementation

// Init the MATLAB engine
// (no need to call it directly since it is automatically invoked by any other command)
IGL_INLINE void igl::matlab::mlinit(Engine** mlengine)
{
  *mlengine = engOpen("\0");
}

// Closes the MATLAB engine
IGL_INLINE void igl::matlab::mlclose(Engine** mlengine)
{
  engClose(*mlengine);
  *mlengine = 0;
}

// Send a matrix to MATLAB
IGL_INLINE void igl::matlab::mlsetmatrix(Engine** mlengine, std::string name, const Eigen::MatrixXd& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  mxArray *A = mxCreateDoubleMatrix(M.rows(), M.cols(), mxREAL);
  double *pM = mxGetPr(A);

  int c = 0;
  for(int j=0; j<M.cols();++j)
    for(int i=0; i<M.rows();++i)
      pM[c++] = double(M(i,j));

  engPutVariable(*mlengine, name.c_str(), A);
  mxDestroyArray(A);
}

// Send a matrix to MATLAB
IGL_INLINE void igl::matlab::mlsetmatrix(Engine** mlengine, std::string name, const Eigen::MatrixXf& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  mxArray *A = mxCreateDoubleMatrix(M.rows(), M.cols(), mxREAL);
  double *pM = mxGetPr(A);

  int c = 0;
  for(int j=0; j<M.cols();++j)
    for(int i=0; i<M.rows();++i)
      pM[c++] = double(M(i,j));

  engPutVariable(*mlengine, name.c_str(), A);
  mxDestroyArray(A);
}

// Send a matrix to MATLAB
IGL_INLINE void igl::matlab::mlsetmatrix(Engine** mlengine, std::string name, const Eigen::MatrixXi& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  mxArray *A = mxCreateDoubleMatrix(M.rows(), M.cols(), mxREAL);
  double *pM = mxGetPr(A);

  int c = 0;
  for(int j=0; j<M.cols();++j)
    for(int i=0; i<M.rows();++i)
      pM[c++] = double(M(i,j))+1;

  engPutVariable(*mlengine, name.c_str(), A);
  mxDestroyArray(A);
}

// Send a matrix to MATLAB
IGL_INLINE void igl::matlab::mlsetmatrix(Engine** mlengine, std::string name, const Eigen::Matrix<unsigned int, Eigen::Dynamic, Eigen::Dynamic >& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  mxArray *A = mxCreateDoubleMatrix(M.rows(), M.cols(), mxREAL);
  double *pM = mxGetPr(A);

  int c = 0;
  for(int j=0; j<M.cols();++j)
    for(int i=0; i<M.rows();++i)
      pM[c++] = double(M(i,j))+1;

  engPutVariable(*mlengine, name.c_str(), A);
  mxDestroyArray(A);
}

// Receive a matrix from MATLAB
IGL_INLINE void igl::matlab::mlgetmatrix(Engine** mlengine, std::string name, Eigen::MatrixXd& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  unsigned long m = 0;
  unsigned long n = 0;
  std::vector<double> t;

  mxArray *ary = engGetVariable(*mlengine, name.c_str());
  if (ary == NULL)
  {
    m = 0;
    n = 0;
    M = Eigen::MatrixXd(0,0);
  }
  else
  {
    m = mxGetM(ary);
    n = mxGetN(ary);
    M = Eigen::MatrixXd(m,n);

    double *pM = mxGetPr(ary);

    int c = 0;
    for(int j=0; j<M.cols();++j)
      for(int i=0; i<M.rows();++i)
        M(i,j) = pM[c++];
  }

  mxDestroyArray(ary);
}

IGL_INLINE void igl::matlab::mlgetmatrix(Engine** mlengine, std::string name, Eigen::MatrixXf& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  unsigned long m = 0;
  unsigned long n = 0;
  std::vector<double> t;

  mxArray *ary = engGetVariable(*mlengine, name.c_str());
  if (ary == NULL)
  {
    m = 0;
    n = 0;
    M = Eigen::MatrixXf(0,0);
  }
  else
  {
    m = mxGetM(ary);
    n = mxGetN(ary);
    M = Eigen::MatrixXf(m,n);

    double *pM = mxGetPr(ary);

    int c = 0;
    for(int j=0; j<M.cols();++j)
      for(int i=0; i<M.rows();++i)
        M(i,j) = pM[c++];
  }

  mxDestroyArray(ary);
}

// Receive a matrix from MATLAB
IGL_INLINE void igl::matlab::mlgetmatrix(Engine** mlengine, std::string name, Eigen::MatrixXi& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  unsigned long m = 0;
  unsigned long n = 0;
  std::vector<double> t;

  mxArray *ary = engGetVariable(*mlengine, name.c_str());
  if (ary == NULL)
  {
    m = 0;
    n = 0;
    M = Eigen::MatrixXi(0,0);
  }
  else
  {
    m = mxGetM(ary);
    n = mxGetN(ary);
    M = Eigen::MatrixXi(m,n);

    double *pM = mxGetPr(ary);

    int c = 0;
    for(int j=0; j<M.cols();++j)
      for(int i=0; i<M.rows();++i)
        M(i,j) = int(pM[c++])-1;
  }

  mxDestroyArray(ary);
}

// Receive a matrix from MATLAB
IGL_INLINE void igl::matlab::mlgetmatrix(Engine** mlengine, std::string name, Eigen::Matrix<unsigned int, Eigen::Dynamic, Eigen::Dynamic >& M)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  unsigned long m = 0;
  unsigned long n = 0;
  std::vector<double> t;

  mxArray *ary = engGetVariable(*mlengine, name.c_str());
  if (ary == NULL)
  {
    m = 0;
    n = 0;
    M = Eigen::Matrix<unsigned int, Eigen::Dynamic, Eigen::Dynamic >(0,0);
  }
  else
  {
    m = mxGetM(ary);
    n = mxGetN(ary);
    M = Eigen::Matrix<unsigned int, Eigen::Dynamic, Eigen::Dynamic >(m,n);

    double *pM = mxGetPr(ary);

    int c = 0;
    for(int j=0; j<M.cols();++j)
      for(int i=0; i<M.rows();++i)
        M(i,j) = (unsigned int)(pM[c++])-1;
  }

  mxDestroyArray(ary);
}


// Send a single scalar to MATLAB
IGL_INLINE void igl::matlab::mlsetscalar(Engine** mlengine, std::string name, double s)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  Eigen::MatrixXd M(1,1);
  M(0,0) = s;
  mlsetmatrix(mlengine, name, M);
}

// Receive a single scalar from MATLAB
IGL_INLINE double igl::matlab::mlgetscalar(Engine** mlengine, std::string name)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  Eigen::MatrixXd M;
  mlgetmatrix(mlengine, name,M);
  return M(0,0);
}

// Execute arbitrary MATLAB code and return the MATLAB output
IGL_INLINE std::string igl::matlab::mleval(Engine** mlengine, std::string code)
{
  if (*mlengine == 0)
    mlinit(mlengine);

  const char *matlab_code = code.c_str();
  const int BUF_SIZE = 4096*4096;
  // allocate on the heap to avoid running out of stack
  std::string bufauto(BUF_SIZE+1, '\0');
  char *buf = &bufauto[0];

  assert(matlab_code != NULL);

  // Use RAII ensure that on leaving this scope, the output buffer is
  // always nullified (to prevent Matlab from accessing memory that might
  // have already been deallocated).
  struct cleanup {
    Engine *m_ep;
    cleanup(Engine *ep) : m_ep(ep) { }
    ~cleanup() { engOutputBuffer(m_ep, NULL, 0); }
  } cleanup_obj(*mlengine);

  if (buf != NULL)
    engOutputBuffer(*mlengine, buf, BUF_SIZE);

  int res = engEvalString(*mlengine, matlab_code);

  if (res != 0) {
    std::ostringstream oss;
    oss << "ERROR: Matlab command failed with error code " << res << ".\n";
    return oss.str();
  }

  if (buf[0] == '>' && buf[1] == '>' && buf[2] == ' ')
    buf += 3;
  if (buf[0] == '\n') ++buf;

  return std::string(buf);
}

// Send a sparse matrix
IGL_INLINE void igl::matlab::mlsetmatrix(Engine** mlengine, std::string name, const Eigen::SparseMatrix<double>& M)
{
  int count = 0;
//  // Count non-zero
//  for (unsigned k=0; k<M.outerSize(); ++k)
//    for (Eigen::SparseMatrix<double>::InnerIterator it(M,k); it; ++it)
//      if (it.value() != 0)
//        ++count;
  
  Eigen::MatrixXd T(M.nonZeros(),3);
  for (unsigned k=0; k<(unsigned)M.outerSize(); ++k)
  {
    for (Eigen::SparseMatrix<double>::InnerIterator it(M,k); it; ++it)
    {
      T(count,0) = it.row();
      T(count,1) = it.col();
      T(count,2) = it.value();
      ++count;
    }
  }

  T.col(0) = T.col(0).array()+1;
  T.col(1) = T.col(1).array()+1;

  mlsetmatrix(mlengine,"temp93765",T);
  
  std::string temp = name + " = sparse(temp93765(:,1),temp93765(:,2),temp93765(:,3),"
                          + std::to_string(M.rows()) + ","
                          + std::to_string(M.cols()) + ");";
  
  mleval(mlengine,temp);
  mleval(mlengine,"clear temp93765");
}
