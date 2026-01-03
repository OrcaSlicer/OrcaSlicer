// high level interface for MshSaver 
//
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/. 
#ifndef IGL_WRITE_MSH_H
#define IGL_WRITE_MSH_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <string>
#include <vector>

namespace igl
{
  /// write triangle surface mesh and tetrahedral volume mesh to .msh file
  ///
  /// @param[in] msh - file name
  /// @param[in] X  eigen double matrix of vertex positions  #X by 3
  /// @param[in] Tri  #Tri eigen integer matrix of triangular faces indices into vertex positions
  /// @param[in] Tet  #Tet eigen integer matrix of tetrahedral indices into vertex positions
  /// @param[in] TriTag #Tri eigen integer vector of tags associated with surface faces {0s}
  /// @param[in] TetTag #Tet eigen integer vector of tags associated with volume elements {0s}
  /// @param[in] XFields #XFields list of strings with field names associated with nodes
  /// @param[in] XF      #XFields list of eigen double matrices, fields associated with nodes 
  /// @param[in] EFields #EFields list of strings with field names associated with elements
  /// @param[in] TriF    #EFields list of eigen double matrices, fields associated with surface elements
  /// @param[in] TetF    #EFields list of eigen double matrices, fields associated with volume elements
  ///
  /// \bug files are always stored in binary format
  /// \bug file format is 2.2
  /// \bug only triangle surface elements and tetrahedral volumetric elements are supported
  /// \bug only 3D information is supported
  /// \bug the tag id is duplicated for physical (0) and elementary (1)
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
  IGL_INLINE bool writeMSH(
    const std::string   &msh,
    const Eigen::MatrixBase<DerivedX> &X,
    const Eigen::MatrixBase<DerivedTri> &Tri,
    const Eigen::MatrixBase<DerivedTet> &Tet,
    const Eigen::MatrixBase<DerivedTriTag> &TriTag,
    const Eigen::MatrixBase<DerivedTetTag> &TetTag,
    const std::vector<std::string> &XFields,
    const std::vector<MatrixXF> &XF,
    const std::vector<std::string>  &EFields,
    const std::vector<MatrixTriF> &TriF,
    const std::vector<MatrixTetF> &TetF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "writeMSH.cpp"
#endif

#endif //IGL_WRITE_MSH_H
