// high level interface for MshLoader 
//
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/. 

#include "readMSH.h"
#include "MshLoader.h"
#include <iostream>

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
IGL_INLINE bool igl::readMSH(
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
  std::vector<MatrixTetF> &TetF)
{
    try 
    {
        igl::MshLoader _loader(msh);
        const int USETAG = 1;

#ifdef IGL_READMESH_DEBUG
        std::cout<<"readMSH:Total number of nodes:" << _loader.get_nodes().size()<<std::endl;   
        std::cout<<"readMSH:Total number of elements:" << _loader.get_elements().size()<<std::endl;

        std::cout<<"readMSH:Node fields:" << std::endl;
        for(auto i=std::begin(_loader.get_node_fields_names()); i!=std::end(_loader.get_node_fields_names()); i++)
        {
            std::cout << i->c_str() << ":" << _loader.get_node_fields()[i-std::begin(_loader.get_node_fields_names())].size() << std::endl;
        }
        
        std::cout << "readMSH:Element fields:" << std::endl;
        for(auto i=std::begin(_loader.get_element_fields_names()); i!=std::end(_loader.get_element_fields_names()); i++)
        {
            std::cout << i->c_str() << ":" << _loader.get_element_fields()[i-std::begin(_loader.get_element_fields_names())].size() << std::endl;
        }

        if(_loader.is_element_map_identity())
            std::cout<<"readMSH:Element ids map is identity"<<std::endl;
        else
            std::cout<<"readMSH:Element ids map is NOT identity"<<std::endl;
        
#endif
        
        // convert nodes
        // hadrcoded for 3D 
        Eigen::Map< const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > 
            node_map( _loader.get_nodes().data(), _loader.get_nodes().size()/3, 3 );

        X = node_map;
        XFields = _loader.get_element_fields_names();
        XF.resize(_loader.get_node_fields().size());
        XFields = _loader.get_node_fields_names();
        for(size_t i=0;i<_loader.get_node_fields().size();++i)
        {
            Eigen::Map< const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > 
                field_map( _loader.get_node_fields()[i].data(), 
                        _loader.get_node_fields()[i].size()/_loader.get_node_fields_components()[i], 
                        _loader.get_node_fields_components()[i] );
            XF[i] = field_map;
        }

        // calculate number of elements 
        std::map<int,int> element_counts;
       
        for(auto i:_loader.get_elements_types())
        {
            auto j=element_counts.insert({i,1});
            if(!j.second) (*j.first).second+=1;
        }
#ifdef IGL_READMESH_DEBUG
        std::cout<<"ReadMSH: elements found"<<std::endl;
        for(auto i:element_counts)
            std::cout<<"\t"<<i.first<<":"<<i.second<<std::endl;
#endif
        int n_tri_el=0;
        int n_tet_el=0;
        
        auto n_tri_el_=element_counts.find(igl::MshLoader::ELEMENT_TRI);
        auto n_tet_el_=element_counts.find(igl::MshLoader::ELEMENT_TET);
        if(n_tri_el_!=std::end(element_counts)) 
            n_tri_el=n_tri_el_->second;
        if(n_tet_el_!=std::end(element_counts)) 
            n_tet_el=n_tet_el_->second;

        Tri.resize(n_tri_el,3);
        Tet.resize(n_tet_el,4);
        TriTag.resize(n_tri_el);
        TetTag.resize(n_tet_el);
        size_t el_start = 0;
        TriF.resize(_loader.get_element_fields().size());
        TetF.resize(_loader.get_element_fields().size());
        for(size_t i=0;i<_loader.get_element_fields().size();++i)
        {
            TriF[i].resize(n_tri_el,_loader.get_element_fields_components()[i]);
            TetF[i].resize(n_tet_el,_loader.get_element_fields_components()[i]);
        }
        EFields = _loader.get_element_fields_names();
        int i_tri = 0;
        int i_tet = 0;

        for(size_t i=0;i<_loader.get_elements_lengths().size();++i)
        {
            if(_loader.get_elements_types()[i]==MshLoader::ELEMENT_TRI )
            {
                assert(_loader.get_elements_lengths()[i]==3);

                Tri(i_tri, 0) = _loader.get_elements()[el_start  ];
                Tri(i_tri, 1) = _loader.get_elements()[el_start+1];
                Tri(i_tri, 2) = _loader.get_elements()[el_start+2];

                TriTag(i_tri) = _loader.get_elements_tags()[1][i];

                for(size_t j=0;j<_loader.get_element_fields().size();++j)
                    for(size_t k=0;k<_loader.get_element_fields_components()[j];++k)
                        TriF[j](i_tri,k) = _loader.get_element_fields()[j][_loader.get_element_fields_components()[j]*i+k];

                ++i_tri;
            } else if(_loader.get_elements_types()[i]==MshLoader::ELEMENT_TET ) {
                assert(_loader.get_elements_lengths()[i]==4);

                Tet(i_tet, 0) = _loader.get_elements()[el_start  ];
                Tet(i_tet, 1) = _loader.get_elements()[el_start+1];
                Tet(i_tet, 2) = _loader.get_elements()[el_start+2];
                Tet(i_tet, 3) = _loader.get_elements()[el_start+3];

                TetTag(i_tet) = _loader.get_elements_tags()[USETAG][i];

                for(size_t j=0;j<_loader.get_element_fields().size();++j)
                    for(size_t k=0;k<_loader.get_element_fields_components()[j];++k)
                        TetF[j](i_tet,k) = _loader.get_element_fields()[j][_loader.get_element_fields_components()[j]*i+k];
                
                ++i_tet;
            } else {
                // else: it's unsupported type of the element, ignore for now
                std::cerr<<"readMSH: unsupported element type: "<<_loader.get_elements_types()[i] << 
                           ", length: "<< _loader.get_elements_lengths()[i] <<std::endl;
            }

            el_start += _loader.get_elements_lengths()[i];
        }

        assert(i_tet == n_tet_el);
        assert(i_tri == n_tri_el);
    } catch(const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return false;
    }
    return true;
}

template <int EigenMatrixOptions>
IGL_INLINE bool igl::readMSH(
  const std::string &msh,
  Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &X,
  Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tri,
  Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tet,
  Eigen::VectorXi &TriTag,
  Eigen::VectorXi &TetTag)
{
    std::vector<std::string>     XFields;
    std::vector<std::string>     EFields;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> XF;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> TriF;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> TetF;
    return igl::readMSH(msh,X,Tri,Tet,TriTag,TetTag,XFields,XF,EFields,TriF,TetF);
}

template <int EigenMatrixOptions>
IGL_INLINE bool igl::readMSH(
  const std::string &msh,
  Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &X,
  Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tri,
  Eigen::VectorXi &TriTag)
{
    Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> Tet;
    Eigen::VectorXi TetTag;

    std::vector<std::string>     XFields;
    std::vector<std::string>     EFields;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> XF;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> TriF;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> TetF;

    return igl::readMSH(msh,X,Tri,Tet,TriTag,TetTag,XFields,XF,EFields,TriF,TetF);
}

template <int EigenMatrixOptions>
IGL_INLINE bool igl::readMSH(
  const std::string &msh,
  Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &X,
  Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> &Tri)
{
    Eigen::Matrix<int,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions> Tet;
    Eigen::VectorXi TetTag;
    Eigen::VectorXi TriTag;

    std::vector<std::string>     XFields;
    std::vector<std::string>     EFields;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> XF;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> TriF;
    std::vector<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic,EigenMatrixOptions>> TetF;

    return igl::readMSH(msh,X,Tri,Tet,TriTag,TetTag,XFields,XF,EFields,TriF,TetF);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::readMSH<0>(std::basic_string<char, std::char_traits<char>, std::allocator<char>> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<int, -1, -1, 0, -1, -1>&, Eigen::Matrix<int, -1, -1, 0, -1, -1>&, Eigen::Matrix<int, -1, 1, 0, -1, 1>&, Eigen::Matrix<int, -1, 1, 0, -1, 1>&);
template bool igl::readMSH<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>>(std::basic_string<char, std::char_traits<char>, std::allocator<char>> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 1, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 1, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char>>, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char>>>>&, std::vector<Eigen::Matrix<double, -1, -1, 1, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 1, -1, -1>>>&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char>>, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char>>>>&, std::vector<Eigen::Matrix<double, -1, -1, 1, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 1, -1, -1>>>&, std::vector<Eigen::Matrix<double, -1, -1, 1, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 1, -1, -1>>>&);
#endif

