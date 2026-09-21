// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//                    Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLEFT_CGAL_MESH_BOOLEAN_H
#define IGL_COPYLEFT_CGAL_MESH_BOOLEAN_H

#include "../../igl_inline.h"
#include "../../MeshBooleanType.h"
#include <Eigen/Core>
#include <functional>
#include <vector>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      ///  Compute Boolean csg operations on "solid", consistently oriented
      ///  meshes.
      ///
      ///  @param[in] VA  #VA by 3 list of vertex positions of first mesh
      ///  @param[in] FA  #FA by 3 list of triangle indices into VA
      ///  @param[in] VB  #VB by 3 list of vertex positions of second mesh
      ///  @param[in] FB  #FB by 3 list of triangle indices into VB
      ///  @param[in] type  type of boolean operation
      ///  @param[out] VC  #VC by 3 list of vertex positions of boolean result mesh
      ///  @param[out] FC  #FC by 3 list of triangle indices into VC
      ///  @param[out] J  #FC list of indices into [FA;FA.rows()+FB] revealing "birth" facet
      ///  @return true if inputs induce a piecewise constant winding number
      ///  field and type is valid
      ///
      ///  \see mesh_boolean_cork, intersect_other, remesh_self_intersections
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool mesh_boolean(
        const Eigen::MatrixBase<DerivedVA > & VA,
        const Eigen::MatrixBase<DerivedFA > & FA,
        const Eigen::MatrixBase<DerivedVB > & VB,
        const Eigen::MatrixBase<DerivedFB > & FB,
        const MeshBooleanType & type,
        Eigen::PlainObjectBase<DerivedVC > & VC,
        Eigen::PlainObjectBase<DerivedFC > & FC,
        Eigen::PlainObjectBase<DerivedJ > & J);
      /// \overload
      /// @param[in] type_str  string describing type of boolean operation see mesh_boolean_type_to_funcs
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool mesh_boolean(
        const Eigen::MatrixBase<DerivedVA > & VA,
        const Eigen::MatrixBase<DerivedFA > & FA,
        const Eigen::MatrixBase<DerivedVB > & VB,
        const Eigen::MatrixBase<DerivedFB > & FB,
        const std::string & type_str,
        Eigen::PlainObjectBase<DerivedVC > & VC,
        Eigen::PlainObjectBase<DerivedFC > & FC,
        Eigen::PlainObjectBase<DerivedJ > & J);
      ///  \overload
      ///
      ///  @param[in] wind_num_op  function handle for filtering winding numbers from
      ///               tuples of integer values to [0,1] outside/inside values
      ///  @param[in] keep  function handle for determining if a patch should be "kept"
      ///      in the output based on the winding number on either side
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool mesh_boolean(
          const Eigen::MatrixBase<DerivedVA> & VA,
          const Eigen::MatrixBase<DerivedFA> & FA,
          const Eigen::MatrixBase<DerivedVB> & VB,
          const Eigen::MatrixBase<DerivedFB> & FB,
          const std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
          const std::function<int(const int, const int)> & keep,
          Eigen::PlainObjectBase<DerivedVC > & VC,
          Eigen::PlainObjectBase<DerivedFC > & FC,
          Eigen::PlainObjectBase<DerivedJ > & J);
      /// Variadic mesh Boolean operations
      ///
      /// @param[in] Vlist  k-long list of lists of mesh vertex positions
      /// @param[in] Flist  k-long list of lists of mesh face indices, so that Flist[i] indexes
      ///      vertices in Vlist[i]
      /// @param[in] wind_num_op  function handle for filtering winding numbers from
      ///      n-tuples of integer values to [0,1] outside/inside values
      /// @param[in] keep  function handle for determining if a patch should be "kept"
      ///      in the output based on the winding number on either side
      /// @param[out] VC  #VC by 3 list of vertex positions of boolean result mesh
      /// @param[out] FC  #FC by 3 list of triangle indices into VC
      /// @param[out] J  #FC list of indices into [Flist[0];Flist[1];...;Flist[k]]
      ///      revealing "birth" facet
      /// @return true iff inputs induce a piecewise constant winding number
      ///   field
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool mesh_boolean(
          const std::vector<DerivedV > & Vlist,
          const std::vector<DerivedF > & Flist,
          const std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
          const std::function<int(const int, const int)> & keep,
          Eigen::PlainObjectBase<DerivedVC > & VC,
          Eigen::PlainObjectBase<DerivedFC > & FC,
          Eigen::PlainObjectBase<DerivedJ > & J);
      /// \overload
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool mesh_boolean(
          const std::vector<DerivedV > & Vlist,
          const std::vector<DerivedF > & Flist,
          const MeshBooleanType & type,
          Eigen::PlainObjectBase<DerivedVC > & VC,
          Eigen::PlainObjectBase<DerivedFC > & FC,
          Eigen::PlainObjectBase<DerivedJ > & J);
      /// \overload
      /// \brief Given a merged mesh (V,F) and list of sizes of inputs
      ///
      /// @param[in] V  #V by 3 list of merged mesh vertex positions
      /// @param[in] F  #F by 3 list of merged mesh face indices so that first sizes(0)
      ///     faces come from the first input, and the next sizes(1) faces come
      ///     from the second input, and so on.
      /// @param[in] sizes  #inputs list of sizes so that sizes(i) is the #faces in the
      ///     ith input
      template <
        typename DerivedVV,
        typename DerivedFF,
        typename Derivedsizes,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool mesh_boolean(
          const Eigen::MatrixBase<DerivedVV > & VV,
          const Eigen::MatrixBase<DerivedFF > & FF,
          const Eigen::MatrixBase<Derivedsizes> & sizes,
          const std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
          const std::function<int(const int, const int)> & keep,
          Eigen::PlainObjectBase<DerivedVC > & VC,
          Eigen::PlainObjectBase<DerivedFC > & FC,
          Eigen::PlainObjectBase<DerivedJ > & J);
      /// \overload
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedVC,
        typename DerivedFC>
      IGL_INLINE bool mesh_boolean(
          const Eigen::MatrixBase<DerivedVA > & VA,
          const Eigen::MatrixBase<DerivedFA > & FA,
          const Eigen::MatrixBase<DerivedVB > & VB,
          const Eigen::MatrixBase<DerivedFB > & FB,
          const MeshBooleanType & type,
          Eigen::PlainObjectBase<DerivedVC > & VC,
          Eigen::PlainObjectBase<DerivedFC > & FC);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "mesh_boolean.cpp"
#endif

#endif
