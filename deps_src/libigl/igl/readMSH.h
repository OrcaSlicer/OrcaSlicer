// high level interface for MshLoader.h/.cpp

// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distribute
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READ_MSH_H
#define IGL_READ_MSH_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <string>
#include <vector>


namespace igl 
{
    /// read triangle surface mesh and tetrahedral volume mesh from .msh file
    ///
    /// @tparam EigenMatrixOptions  matrix options of output matrices (e.g.,
    /// Eigen::ColMajor, Eigen::RowMajor)
    /// @param[in] msh - file name
    /// @param[out] X  eigen double matrix of vertex positions  #X by 3
    /// @param[out] Tri  #Tri eigen integer matrix of triangular faces indices into vertex positions
    /// @param[out] Tet  #Tet eigen integer matrix of tetrahedral indices into vertex positions
    /// @param[out] TriTag #Tri eigen integer vector of tags associated with surface faces
    /// @param[out] TetTag #Tet eigen integer vector of tags associated with volume elements
    /// @param[out] XFields #XFields list of strings with field names associated with nodes
    /// @param[out] XF      #XFields list of eigen double matrices, fields associated with nodes 
    /// @param[out] EFields #EFields list of strings with field names associated with elements
    /// @param[out] TriF    #EFields list of eigen double matrices, fields associated with surface elements
    /// @param[out] TetF    #EFields list of eigen double matrices, fields associated with volume elements
    /// @return true on success
    /// \bug only version 2.2 of .msh file is supported (gmsh 3.X)
    /// \bug only triangle surface elements and tetrahedral volumetric elements are supported
    /// \bug only 3D information is supported
    /// \bug only the 1st tag per element is returned (physical) 
    /// \bug same element fields are expected to be associated with surface elements and volumetric elements
    template <
      typename DerivedX,
      typename DerivedTri,
      typename DerivedTet,
      typename DerivedTriTag,
      typename DerivedTetTag,
      typename MatrixXF,
      typename MatrixTriF,
      typename MatrixTetF
      >
    IGL_INLINE bool readMSH(
      const std::string &msh,
      Eigen::PlainObjectBase<DerivedX> &X,
      Eigen::PlainObjectBase<DerivedTri> &Tri,
      Eigen::PlainObjectBase<DerivedTet> &Tet,
      Eigen::PlainObjectBase<DerivedTriTag> &TriTag,
      Eigen::PlainObjectBase<DerivedTetTag> &TetTag,
      std::vector<std::string>     &XFields,
      std::vector<MatrixXF> &XF,
      std::vector<std::string>     &EFields,
      std::vector<MatrixTriF> &TriF,
      std::vector<MatrixTetF> &TetF);
    /// \overload
    template <int EigenMatrixOptions>
    IGL_INLINE bool readMSH(
      const std::string &msh,
      Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &X,
      Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tri,
      Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tet,
      Eigen::VectorXi &TriTag,
      Eigen::VectorXi &TetTag);
    /// \overload
    template <int EigenMatrixOptions>
    IGL_INLINE bool readMSH(
      const std::string &msh,
      Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &X,
      Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tri,
      Eigen::VectorXi &TriTag);
    /// \overload
    template <int EigenMatrixOptions>
    IGL_INLINE bool readMSH(
      const std::string &msh,
      Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &X,
      Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tri);

}


#ifndef IGL_STATIC_LIBRARY
#  include "readMSH.cpp"
#endif

#endif
