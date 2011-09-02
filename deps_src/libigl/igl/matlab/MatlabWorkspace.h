// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MATLAB_MATLAB_WORKSPACE_H
#define IGL_MATLAB_MATLAB_WORKSPACE_H

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <mat.h>

#include <string>
#include <vector>

namespace igl
{
  namespace matlab
  {
    // It would be really great to replicate this for a simple XML-based
    // workspace.
    //
    // Class which contains data of a matlab workspace which can be written to a
    // .mat file and loaded from matlab
    // 
    // This depends on matlab at compile time (though it shouldn't necessarily
    // have to) but it does not depend on running the matlab engine at run-time.
    //
    // Known bugs: Treats all matrices as doubles (this may actually be desired
    // for some "index" matrices since matlab's sparse command takes doubles
    // rather than int class matrices). It is of course not desired when dealing
    // with logicals or uint's for images.
    class MatlabWorkspace
    {
      private:
        // KNOWN BUG: Why not use a map? Any reason to allow duplicate names?
        //
        // List of names
        std::vector<std::string> names;
        // List of data pointers
        std::vector<mxArray*> data;
      public:
        MatlabWorkspace();
        ~MatlabWorkspace();
        // Clear names and data of variables in workspace
        inline void clear();
        // Save current list of variables
        //
        // Inputs:
        //   path  path to .mat file
        // Returns true on success, false on failure
        inline bool write(const std::string & path) const;
        // Load list of variables from .mat file
        //
        // Inputs:
        //   path  path to .mat file
        // Returns true on success, false on failure
        inline bool read(const std::string & path);
        // Assign data to a variable name in the workspace
        //
        // Template: 
        //   DerivedM  eigen matrix (e.g. MatrixXd)
        // Inputs:
        //   M  data (usually a matrix)
        //   name  variable name to save into work space
        // Returns true on success, false on failure
        //
        // Known Bugs: Assumes Eigen is using column major ordering
        template <typename DerivedM>
        inline MatlabWorkspace& save(
          const Eigen::PlainObjectBase<DerivedM>& M,
          const std::string & name);
        // Template:
        //   MT  sparse matrix type (e.g. double)
        template <typename MT>
        inline MatlabWorkspace& save(
          const Eigen::SparseMatrix<MT>& M,
          const std::string & name);
        // Templates:
        //   ScalarM  scalar type, e.g. double
        template <typename ScalarM>
        inline MatlabWorkspace& save(
          const std::vector<std::vector<ScalarM> > & vM,
          const std::string & name);
        // Templates:
        //   ScalarV  scalar type, e.g. double
        template <typename ScalarV>
        inline MatlabWorkspace& save(
          const std::vector<ScalarV> & vV,
          const std::string & name);
        // NOTE: Eigen stores quaternions coefficients as (i,j,k,1), but most of
        // our matlab code stores them as (1,i,j,k) This takes a quaternion and
        // saves it as a (1,i,j,k) row vector
        //
        // Templates:
        //   Q  quaternion type
        template <typename Q>
        inline MatlabWorkspace& save(
          const Eigen::Quaternion<Q> & q,
          const std::string & name);
        inline MatlabWorkspace& save(
          const double d,
          const std::string & name);
        // Same as save() but adds 1 to each element, useful for saving "index"
        // matrices like lists of faces or elements
        template <typename DerivedM>
        inline MatlabWorkspace& save_index(
          const Eigen::DenseBase<DerivedM>& M,
          const std::string & name);
        template <typename ScalarM>
        inline MatlabWorkspace& save_index(
          const std::vector<std::vector<ScalarM> > & vM,
          const std::string & name);
        template <typename ScalarV>
        inline MatlabWorkspace& save_index(
          const std::vector<ScalarV> & vV,
          const std::string & name);
        // Find a certain matrix by name.
        //
        // KNOWN BUG: Outputs the first found (not necessarily unique lists).
        //
        // Template: 
        //   DerivedM  eigen matrix (e.g. MatrixXd)
        // Inputs:
        //   name  exact name of matrix as string
        // Outputs:
        //   M  matrix
        // Returns true only if found.
        template <typename DerivedM>
        inline bool find( 
          const std::string & name,
          Eigen::PlainObjectBase<DerivedM>& M);
        template <typename MT>
        inline bool find( 
          const std::string & name,
          Eigen::SparseMatrix<MT>& M);
        inline bool find( 
          const std::string & name,
          double & d);
        inline bool find( 
          const std::string & name,
          int & v);
        // Subtracts 1 from all entries
        template <typename DerivedM>
        inline bool find_index( 
          const std::string & name,
          Eigen::PlainObjectBase<DerivedM>& M);
    };
  }
}

// Implementation

// Be sure that this is not compiled into libigl.a
// http://stackoverflow.com/a/3318993/148668

// IGL
#include "igl/list_to_matrix.h"

// MATLAB
#include "mat.h"

// STL
#include <iostream>
#include <algorithm>
#include <vector>

inline igl::matlab::MatlabWorkspace::MatlabWorkspace():
  names(),
  data()
{
}

inline igl::matlab::MatlabWorkspace::~MatlabWorkspace()
{
  // clean up data
  clear();
}

inline void igl::matlab::MatlabWorkspace::clear()
{
  for_each(data.begin(),data.end(),&mxDestroyArray);
  data.clear();
  names.clear();
}

inline bool igl::matlab::MatlabWorkspace::write(const std::string & path) const
{
  using namespace std;
  MATFile * mat_file = matOpen(path.c_str(), "w");
  if(mat_file == NULL)
  {
    fprintf(stderr,"Error opening file %s\n",path.c_str());
    return false;
  }
  assert(names.size() == data.size());
  // loop over names and data
  for(int i = 0;i < (int)names.size(); i++)
  {
    // Put variable as LOCAL variable
    int status = matPutVariable(mat_file,names[i].c_str(), data[i]);
    if(status != 0) 
    {
      cerr<<"^MatlabWorkspace::save Error: matPutVariable ("<<names[i]<<
        ") failed"<<endl;
      return false;
    } 
  }
  if(matClose(mat_file) != 0)
  {
    fprintf(stderr,"Error closing file %s\n",path.c_str());
    return false;
  }
  return true;
}

inline bool igl::matlab::MatlabWorkspace::read(const std::string & path)
{
  using namespace std;

  MATFile * mat_file;

  mat_file = matOpen(path.c_str(), "r");
  if (mat_file == NULL) 
  {
    cerr<<"Error: failed to open "<<path<<endl;
    return false;
  }

  int ndir;
  const char ** dir = (const char **)matGetDir(mat_file, &ndir);
  if (dir == NULL) {
    cerr<<"Error reading directory of file "<< path<<endl;
    return false;
  }
  mxFree(dir);

  // Must close and reopen
  if(matClose(mat_file) != 0)
  {
    cerr<<"Error: failed to close file "<<path<<endl;
    return false;
  }
  mat_file = matOpen(path.c_str(), "r");
  if (mat_file == NULL) 
  {
    cerr<<"Error: failed to open "<<path<<endl;
    return false;
  }
  

  /* Read in each array. */
  for (int i=0; i<ndir; i++) 
  {
    const char * name;
    mxArray * mx_data = matGetNextVariable(mat_file, &name);
    if (mx_data == NULL) 
    {
      cerr<<"Error: matGetNextVariable failed in "<<path<<endl;
      return false;
    } 
    const int dims = mxGetNumberOfDimensions(mx_data);
    assert(dims == 2);
    if(dims != 2)
    {
      fprintf(stderr,"Variable '%s' has %d ≠ 2 dimensions. Skipping\n",
          name,dims);
      mxDestroyArray(mx_data);
      continue;
    }
    // don't destroy
    names.push_back(name);
    data.push_back(mx_data);
  }

  if(matClose(mat_file) != 0)
  {
    cerr<<"Error: failed to close file "<<path<<endl;
    return false;
  }

  return true;
}

// Treat everything as a double
template <typename DerivedM>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save(
  const Eigen::PlainObjectBase<DerivedM>& M,
  const std::string & name)
{
  using namespace std;
  const int m = M.rows();
  const int n = M.cols();
  mxArray * mx_data = mxCreateDoubleMatrix(m,n,mxREAL);
  data.push_back(mx_data);
  names.push_back(name);
  // Copy data immediately
  // Use Eigen's map and cast to copy
  Eigen::Map< Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> > 
    map(mxGetPr(mx_data),m,n);
  map = M.template cast<double>();
  return *this;
}

// Treat everything as a double
template <typename MT>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save(
  const Eigen::SparseMatrix<MT>& M,
  const std::string & name)
{
  using namespace std;
  const int m = M.rows();
  const int n = M.cols();
  // THIS WILL NOT WORK FOR ROW-MAJOR
  assert(n==M.outerSize());
  const int nzmax = M.nonZeros();
  mxArray * mx_data = mxCreateSparse(m, n, nzmax, mxREAL);
  data.push_back(mx_data);
  names.push_back(name);
  // Copy data immediately
  double * pr = mxGetPr(mx_data);
  mwIndex * ir = mxGetIr(mx_data);
  mwIndex * jc = mxGetJc(mx_data);

  // Iterate over outside
  int k = 0;
  for(int j=0; j<M.outerSize();j++)
  {
    jc[j] = k;
    // Iterate over inside
    for(typename Eigen::SparseMatrix<MT>::InnerIterator it (M,j); it; ++it)
    {
      pr[k] = it.value();
      ir[k] = it.row();
      k++;
    }
  }
  jc[M.outerSize()] = k;

  return *this;
}

template <typename ScalarM>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save(
  const std::vector<std::vector<ScalarM> > & vM,
  const std::string & name)
{
  Eigen::MatrixXd M;
  list_to_matrix(vM,M);
  return this->save(M,name);
}

template <typename ScalarV>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save(
  const std::vector<ScalarV> & vV,
  const std::string & name)
{
  Eigen::MatrixXd V;
  list_to_matrix(vV,V);
  return this->save(V,name);
}

template <typename Q>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save(
  const Eigen::Quaternion<Q> & q,
  const std::string & name)
{
  Eigen::Matrix<Q,1,4> qm;
  qm(0,0) = q.w();
  qm(0,1) = q.x();
  qm(0,2) = q.y();
  qm(0,3) = q.z();
  return save(qm,name);
}

inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save(
  const double d,
  const std::string & name)
{
  Eigen::VectorXd v(1);
  v(0) = d;
  return save(v,name);
}

template <typename DerivedM>
inline igl::matlab::MatlabWorkspace& 
  igl::matlab::MatlabWorkspace::save_index(
    const Eigen::DenseBase<DerivedM>& M,
    const std::string & name)
{
  DerivedM Mp1 = M;
  Mp1.array() += 1;
  return this->save(Mp1,name);
}

template <typename ScalarM>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save_index(
  const std::vector<std::vector<ScalarM> > & vM,
  const std::string & name)
{
  Eigen::MatrixXd M;
  list_to_matrix(vM,M);
  return this->save_index(M,name);
}

template <typename ScalarV>
inline igl::matlab::MatlabWorkspace& igl::matlab::MatlabWorkspace::save_index(
  const std::vector<ScalarV> & vV,
  const std::string & name)
{
  Eigen::MatrixXd V;
  list_to_matrix(vV,V);
  return this->save_index(V,name);
}

template <typename DerivedM>
inline bool igl::matlab::MatlabWorkspace::find( 
  const std::string & name,
  Eigen::PlainObjectBase<DerivedM>& M)
{
  using namespace std;
  const int i = std::find(names.begin(), names.end(), name)-names.begin();
  if(i>=(int)names.size())
  {
    return false;
  }
  assert(i<=(int)data.size());
  mxArray * mx_data = data[i];
  assert(!mxIsSparse(mx_data));
  assert(mxGetNumberOfDimensions(mx_data) == 2);
  //cout<<name<<": "<<mxGetM(mx_data)<<" "<<mxGetN(mx_data)<<endl;
  const int m = mxGetM(mx_data);
  const int n = mxGetN(mx_data);
  // Handle vectors: in the sense that anything found becomes a column vector,
  // whether it was column vector, row vector or matrix
  if(DerivedM::IsVectorAtCompileTime)
  {
    assert(m==1 || n==1 || (m==0 && n==0));
    M.resize(m*n,1);
  }else
  {
    M.resize(m,n);
  }
  assert(mxGetNumberOfElements(mx_data) == M.size());
  // Use Eigen's map and cast to copy
  M = Eigen::Map< Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> > 
    (mxGetPr(mx_data),M.rows(),M.cols()).cast<typename DerivedM::Scalar>();
  return true;
}

template <typename MT>
inline bool igl::matlab::MatlabWorkspace::find( 
  const std::string & name,
  Eigen::SparseMatrix<MT>& M)
{
  using namespace std;
  using namespace Eigen;
  const int i = std::find(names.begin(), names.end(), name)-names.begin();
  if(i>=(int)names.size())
  {
    return false;
  }
  assert(i<=(int)data.size());
  mxArray * mx_data = data[i];
  // Handle boring case where matrix is actually an empty dense matrix
  if(mxGetNumberOfElements(mx_data) == 0)
  {
    M.resize(0,0);
    return true;
  }
  assert(mxIsSparse(mx_data));
  assert(mxGetNumberOfDimensions(mx_data) == 2);
  //cout<<name<<": "<<mxGetM(mx_data)<<" "<<mxGetN(mx_data)<<endl;
  const int m = mxGetM(mx_data);
  const int n = mxGetN(mx_data);
  // TODO: It should be possible to directly load the data into the sparse
  // matrix without going through the triplets
  // Copy data immediately
  double * pr = mxGetPr(mx_data);
  mwIndex * ir = mxGetIr(mx_data);
  mwIndex * jc = mxGetJc(mx_data);
  vector<Triplet<MT> > MIJV;
  const int nnz = mxGetNzmax(mx_data);
  MIJV.reserve(nnz);
  // Iterate over outside
  int k = 0;
  for(int j=0; j<n;j++)
  {
    // Iterate over inside
    while(k<(int)jc[j+1])
    {
      //cout<<ir[k]<<" "<<j<<" "<<pr[k]<<endl;
      assert((int)ir[k]<m);
      assert((int)j<n);
      MIJV.push_back(Triplet<MT >(ir[k],j,pr[k]));
      k++;
    }
  }
  M.resize(m,n);
  M.setFromTriplets(MIJV.begin(),MIJV.end());

  return true;
}

inline bool igl::matlab::MatlabWorkspace::find( 
  const std::string & name,
  int & v)
{
  using namespace std;
  const int i = std::find(names.begin(), names.end(), name)-names.begin();
  if(i>=(int)names.size())
  {
    return false;
  }
  assert(i<=(int)data.size());
  mxArray * mx_data = data[i];
  assert(!mxIsSparse(mx_data));
  assert(mxGetNumberOfDimensions(mx_data) == 2);
  //cout<<name<<": "<<mxGetM(mx_data)<<" "<<mxGetN(mx_data)<<endl;
  assert(mxGetNumberOfElements(mx_data) == 1);
  copy(
    mxGetPr(mx_data),
    mxGetPr(mx_data)+mxGetNumberOfElements(mx_data),
    &v);
  return true;
}

inline bool igl::matlab::MatlabWorkspace::find( 
  const std::string & name,
  double & d)
{
  using namespace std;
  const int i = std::find(names.begin(), names.end(), name)-names.begin();
  if(i>=(int)names.size())
  {
    return false;
  }
  assert(i<=(int)data.size());
  mxArray * mx_data = data[i];
  assert(!mxIsSparse(mx_data));
  assert(mxGetNumberOfDimensions(mx_data) == 2);
  //cout<<name<<": "<<mxGetM(mx_data)<<" "<<mxGetN(mx_data)<<endl;
  assert(mxGetNumberOfElements(mx_data) == 1);
  copy(
    mxGetPr(mx_data),
    mxGetPr(mx_data)+mxGetNumberOfElements(mx_data),
    &d);
  return true;
}

template <typename DerivedM>
inline bool igl::matlab::MatlabWorkspace::find_index( 
  const std::string & name,
  Eigen::PlainObjectBase<DerivedM>& M)
{
  if(!find(name,M))
  {
    return false;
  }
  M.array() -= 1;
  return true;
}


//template <typename Data>
//bool igl::matlab::MatlabWorkspace::save(const Data & M, const std::string & name)
//{
//  using namespace std;
//  // If I don't know the type then I can't save it
//  cerr<<"^MatlabWorkspace::save Error: Unknown data type. "<<
//    name<<" not saved."<<endl;
//  return false;
//}

#endif

