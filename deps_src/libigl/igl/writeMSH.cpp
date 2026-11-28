// high level interface for MshSaver
//
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "writeMSH.h"
#include "MshSaver.h"
#include "MshLoader.h"
#include <iostream>


namespace igl {
namespace internal {

    // helper function, appends contents of Eigen matrix to an std::vector, in RowMajor fashion
    template <typename T, typename Derived>
    void append_mat_to_vec(std::vector<T> &vec, const Eigen::MatrixBase<Derived> & mat)
    {
      size_t st = vec.size();
      vec.resize(st + mat.size());

      // Iterate over the rows and columns in row-major order
      for (int i = 0; i < mat.rows(); ++i) 
      {
        for (int j = 0; j < mat.cols(); ++j) 
        {
          vec[st++] = mat(i, j);
        }
      }
    }

}
}

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
IGL_INLINE bool igl::writeMSH(
  const std::string &msh,
  const Eigen::MatrixBase<DerivedX> &X,
  const Eigen::MatrixBase<DerivedTri> &Tri,
  const Eigen::MatrixBase<DerivedTet> &Tet,
  const Eigen::MatrixBase<DerivedTriTag> &TriTag,
  const Eigen::MatrixBase<DerivedTetTag> &TetTag,
  const std::vector<std::string> &XFields,
  const std::vector<MatrixXF> &XF,
  const std::vector<std::string>  &EFields,
  const std::vector<MatrixTriF> &TriF,
  const std::vector<MatrixTetF> &TetF)
{
  
    using namespace internal;

    try
    {
        // error checks
        if(!XFields.empty())
        {
            if(XFields.size()!=XF.size())
                throw std::invalid_argument("Vertex field count mismatch");
            for(int i=0;i<XFields.size();++i)
                if(XF[i].rows()!=X.rows())
                    throw std::invalid_argument("Vertex field size mismatch");
        }

        if(!EFields.empty())
        {
            if(EFields.size()!=TriF.size())
                throw std::invalid_argument("Triangle field count mismatch");
            if(EFields.size()!=TetF.size())
                throw std::invalid_argument("Tetrahedra field count mismatch");

            for(int i=0;i<EFields.size();++i)
            {
                if(TriF[i].rows()!=Tri.rows())
                    throw std::invalid_argument("Triangle field size mismatch");
                if(TetF[i].rows()!=Tet.rows())
                    throw std::invalid_argument("Tetrahedra field size mismatch");
            }
        }

        // this is not the most optimal , it would be faster to modify RRMshSaver to work with Eiged data types
        std::vector<double> _X;
        append_mat_to_vec(_X, X);

        std::vector<int> _Tri_Tet;
        append_mat_to_vec( _Tri_Tet, Tri);
        append_mat_to_vec( _Tri_Tet, Tet);

        std::vector<int> _Tri_Tet_len(Tri.rows(), 3); //each is 3 elements long
        _Tri_Tet_len.insert(_Tri_Tet_len.end(), Tet.rows(), 4);

        std::vector<int> _Tri_Tet_type(Tri.rows(), MshLoader::ELEMENT_TRI);
        _Tri_Tet_type.insert(_Tri_Tet_type.end(), Tet.rows(), MshLoader::ELEMENT_TET);

        std::vector<int> _Tri_Tet_tag;
        // apparently TriTag and TetTag need to be present. Use zero arrays if their
        // empty
        if(TriTag.size() == 0)
        {
          append_mat_to_vec(_Tri_Tet_tag, Eigen::Matrix<int, Eigen::Dynamic, 1>::Zero(Tri.rows()));
        }else
        {
          append_mat_to_vec(_Tri_Tet_tag, TriTag);
        }
        if(TetTag.size() == 0)
        {
          append_mat_to_vec(_Tri_Tet_tag, Eigen::Matrix<int, Eigen::Dynamic, 1>::Zero(Tet.rows()));
        }else
        {
          append_mat_to_vec(_Tri_Tet_tag, TetTag);
        }


        igl::MshSaver msh_saver(msh, true);
        msh_saver.save_mesh( _X,
            _Tri_Tet,
            _Tri_Tet_len,
            _Tri_Tet_type,
            _Tri_Tet_tag);

        // append vertex data
        for(size_t i=0;i<XFields.size();++i)
        {
            assert(X.rows()==XF[i].rows());

            std::vector<double> _XF;
            append_mat_to_vec(_XF, XF[i]);

            if(XF[i].cols() == 1)
                msh_saver.save_scalar_field(XFields[i], _XF );
            else if(XF[i].cols() == 3)
                msh_saver.save_vector_field(XFields[i], _XF );
            else
            {
                throw std::invalid_argument("unsupported vertex field dimensionality");
            }
        }

        // append node data
        for(size_t i=0; i<EFields.size(); ++i)
        {
            assert(TriF[i].cols() == TetF[i].cols());
            assert(TriF[i].rows() == Tri.rows());
            assert(TetF[i].rows() == Tet.rows());

            std::vector<double> _EF;
            append_mat_to_vec(_EF, TriF[i]);
            append_mat_to_vec(_EF, TetF[i]);

            assert(_EF.size() == (TriF[i].size()+TetF[i].size()));

            if( TriF[i].cols() == 1 )
                msh_saver.save_elem_scalar_field(EFields[i], _EF );
            else if( TriF[i].cols() == 3 )
                msh_saver.save_elem_vector_field(EFields[i], _EF );
            else
            {
                throw std::invalid_argument("unsupported node field dimensionality");
            }
        }
    } catch(const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return false;
    }
    return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::writeMSH<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(std::basic_string<char, std::char_traits<char>, std::allocator<char>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char>>, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char>>>> const&, std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1>>> const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char>>, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char>>>> const&, std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1>>> const&, std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1>>> const&);
#endif
