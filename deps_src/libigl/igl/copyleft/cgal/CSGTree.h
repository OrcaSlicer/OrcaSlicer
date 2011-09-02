// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_CSG_TREE_H
#define IGL_COPYLEFT_CGAL_CSG_TREE_H

#include "../../MeshBooleanType.h"
#include "string_to_mesh_boolean_type.h"
#include "mesh_boolean.h"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/number_utils.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Class for defining and computing a constructive solid geometry result
      // out of a tree of boolean operations on "solid" triangle meshes.
      //
      //template <typename DerivedF>
      class CSGTree
      {
        public:
          typedef CGAL::Epeck::FT ExactScalar;
          //typedef Eigen::PlainObjectBase<DerivedF> POBF;
          typedef Eigen::MatrixXi POBF;
          typedef POBF::Index FIndex;
          typedef Eigen::Matrix<ExactScalar,Eigen::Dynamic,3> MatrixX3E;
          typedef Eigen::Matrix<FIndex,Eigen::Dynamic,1> VectorJ;
        private:
          // Resulting mesh
          MatrixX3E m_V;
          POBF m_F;
          VectorJ m_J;
          // Number of birth faces in A + those in B. I.e. sum of original "leaf"
          // faces involved in result.
          size_t m_number_of_birth_faces;
        public:
          CSGTree()
          {
          }
          //typedef Eigen::MatrixXd MatrixX3E;
          //typedef Eigen::MatrixXi POBF;
          // http://stackoverflow.com/a/3279550/148668
          CSGTree(const CSGTree & other)
            :
            // copy things
            m_V(other.m_V),
            // This is an issue if m_F is templated
            // https://forum.kde.org/viewtopic.php?f=74&t=128414
            m_F(other.m_F),
            m_J(other.m_J),
            m_number_of_birth_faces(other.m_number_of_birth_faces)
          {
          }
          // copy-swap idiom
          friend void swap(CSGTree& first, CSGTree& second)
          {
            using std::swap;
            // swap things
            swap(first.m_V,second.m_V);
            // This is an issue if m_F is templated, similar to
            // https://forum.kde.org/viewtopic.php?f=74&t=128414
            swap(first.m_F,second.m_F);
            swap(first.m_J,second.m_J);
            swap(first.m_number_of_birth_faces,second.m_number_of_birth_faces);
          }
          // Pass-by-value (aka copy)
          CSGTree& operator=(CSGTree other)
          {
            swap(*this,other);
            return *this;
          }
          CSGTree(CSGTree&& other):
            // initialize via default constructor
            CSGTree() 
          {
            swap(*this,other);
          }
          // Construct and compute a boolean operation on existing CSGTree nodes.
          //
          // Inputs:
          //   A  Solid result of previous CSG operation (or identity, see below)
          //   B  Solid result of previous CSG operation (or identity, see below)
          //   type  type of mesh boolean to compute 
          CSGTree(
            const CSGTree & A,
            const CSGTree & B,
            const MeshBooleanType & type)
          {
            // conduct boolean operation
            mesh_boolean(A.V(),A.F(),B.V(),B.F(),type,m_V,m_F,m_J);
            // reindex m_J
            std::for_each(m_J.data(),m_J.data()+m_J.size(),
              [&](typename VectorJ::Scalar & j) -> void
              {
                if(j < A.F().rows())
                {
                  j = A.J()(j);
                }else
                {
                  assert(j<(A.F().rows()+B.F().rows()));
                  j = A.number_of_birth_faces()+(B.J()(j-A.F().rows()));
                }
              });
            m_number_of_birth_faces = 
              A.number_of_birth_faces() + B.number_of_birth_faces();
          }
          // Overload using string for type
          CSGTree(
            const CSGTree & A,
            const CSGTree & B,
            const std::string & s):
            CSGTree(A,B,string_to_mesh_boolean_type(s))
          {
            // do nothing (all done in constructor).
          }
          // "Leaf" node with identity operation on assumed "solid" mesh (V,F)
          //
          // Inputs:
          //   V  #V by 3 list of mesh vertices (in any precision, will be
          //     converted to exact)
          //   F  #F by 3 list of mesh face indices into V
          template <typename DerivedV>
          CSGTree(const Eigen::PlainObjectBase<DerivedV> & V, const POBF & F)//:
          // Possible Eigen bug:
          // https://forum.kde.org/viewtopic.php?f=74&t=128414
            //m_V(V.template cast<ExactScalar>()),m_F(F)
          {
            m_V = V.template cast<ExactScalar>();
            m_F = F;
            // number of faces
            m_number_of_birth_faces = m_F.rows();
            // identity birth index
            m_J = VectorJ::LinSpaced(
              m_number_of_birth_faces,0,m_number_of_birth_faces-1);
          }
          // Returns reference to resulting mesh vertices m_V in exact scalar
          // representation
          const MatrixX3E & V() const
          {
            return m_V;
          }
          // Returns mesh vertices in the desired output type, casting when
          // appropriate to floating precision.
          template <typename DerivedV>
          DerivedV cast_V() const
          {
            DerivedV dV;
            dV.resize(m_V.rows(),m_V.cols());
            for(int i = 0;i<m_V.rows();i++)
            {
              for(int j = 0;j<m_V.cols();j++)
              {
                dV(i,j) = CGAL::to_double(m_V(i,j));
              }
            }
            return dV;
          }
          // Returns reference to resulting mesh faces m_F
          const POBF & F() const
          {
            return m_F;
          }
          // Returns reference to "birth parents" indices into [F1;F2;...;Fn]
          // where F1, ... , Fn are the face lists of the leaf ("original") input
          // meshes.
          const VectorJ & J() const
          {
            return m_J;
          }
          // The number of leaf faces = #F1 + #F2 + ... + #Fn
          const size_t & number_of_birth_faces() const
          {
            return m_number_of_birth_faces;
          }
      };
    }
  }
}


#endif
