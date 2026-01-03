// based on MSH writer from PyMesh 

// Copyright (c) 2015 Qingnan Zhou <qzhou@adobe.com>           
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/. 

#include "MshSaver.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <exception>


IGL_INLINE igl::MshSaver::MshSaver(const std::string& filename, bool binary) :
    m_binary(binary), m_num_nodes(0), m_num_elements(0) {
        if (!m_binary) {
            fout.open(filename.c_str(), std::fstream::out);
        } else {
            fout.open(filename.c_str(), std::fstream::binary);
        }
        if (!fout) {
            std::stringstream err_msg;
            err_msg << "Error opening " << filename << " to write msh file." << std::endl;
            throw std::ios_base::failure(err_msg.str());
        }
}

IGL_INLINE igl::MshSaver::~MshSaver() {
    fout.close();
}

IGL_INLINE void igl::MshSaver::save_mesh(
    const FloatVector& nodes, 
    const IndexVector& elements, 
    const IntVector& element_lengths,
    const IntVector& element_types,
    const IntVector& element_tags
     ) {

    save_header();

    save_nodes(nodes);

    save_elements(elements, element_lengths, element_types, element_tags );
}

IGL_INLINE void igl::MshSaver::save_header() {
    if (!m_binary) {
        fout << "$MeshFormat" << std::endl;
        fout << "2.2 0 " << sizeof(double) << std::endl;
        fout << "$EndMeshFormat" << std::endl;
        fout.precision(17);
    } else {
        fout << "$MeshFormat" << std::endl;
        fout << "2.2 1 " << sizeof(double) << std::endl;
        int one = 1;
        fout.write((char*)&one, sizeof(int));
        fout << "\n$EndMeshFormat" << std::endl;
    }
    fout.flush();
}

IGL_INLINE void igl::MshSaver::save_nodes(const FloatVector& nodes) {
    // Save nodes.
    // 3D hadrcoded
    m_num_nodes = nodes.size() / 3;
    fout << "$Nodes" << std::endl;
    fout << m_num_nodes << std::endl;
    if (!m_binary) {
        for (size_t i=0; i<nodes.size(); i+=3) {
            //const VectorF& v = nodes.segment(i,m_dim);
            int node_idx = i/3 + 1;
            fout << node_idx << " " << nodes[i] << " " << nodes[i+1] << " " << nodes[i+2] << std::endl;
        }
    } else {
        for (size_t i=0; i<nodes.size(); i+=3) {
            //const VectorF& v = nodes.segment(i,m_dim);
            int node_idx = i/3 + 1;
            fout.write((const char*)&node_idx, sizeof(int));
            fout.write((const char*)&nodes[i], sizeof(Float)*3);
        }
    }
    fout << "$EndNodes" << std::endl;
    fout.flush();
}

IGL_INLINE void igl::MshSaver::save_elements(const IndexVector& elements, 
            const IntVector& element_lengths,
            const IntVector& element_types,
            const IntVector& element_tags) 
    {

    m_num_elements = element_tags.size();
    assert(element_lengths.size() == element_types.size() );
    assert(element_lengths.size() == element_tags.size() );
    // TODO: sum up all lengths
    // Save elements.
    // node inxes are 1-based
    fout << "$Elements" << std::endl;
    fout << m_num_elements << std::endl;

    if (m_num_elements > 0) {
        //int elem_type = el_type;
        //int tags = 0;
        if (!m_binary) {
            size_t el_ptr=0;
            for (size_t i=0;i<m_num_elements;++i) {
                
                int elem_num = (int) i + 1;
                ///VectorI elem = elements.segment(i, nodes_per_element) + VectorI::Ones(nodes_per_element);
                // hardcoded: duplicate tags (I don't know why)
                fout << elem_num << " " << element_types[i] << " " << 2 << " "<< element_tags[i] << " "<< element_tags[i] << " ";
                for (size_t j=0; j<element_lengths[i]; j++) {
                    fout << elements[el_ptr + j] + 1 << " ";
                }
                fout << std::endl;
                el_ptr+=element_lengths[i];
            }
        } else {
            size_t el_ptr=0,i=0;
            while(i<m_num_elements) {

                // write elements in consistent chunks
                // TODO: refactor this code to be able to specify different elements
                // more effeciently 

                int elem_type=-1;
                int elem_len=-1;
                size_t j=i;
                for(;j<m_num_elements;++j)
                {
                    if( elem_type==-1 ) 
                    {
                        elem_type=element_types[j];
                        elem_len=element_lengths[j];
                    } else if( elem_type!=element_types[j] || 
                               elem_len!=element_lengths[j]) {
                        break; // found the edge of the segment
                    }
                }

                //hardcoded: 2 tags
                int num_elems=j-i, num_tags=2;

                fout.write((const char*)& elem_type, sizeof(int));
                fout.write((const char*)& num_elems, sizeof(int));
                fout.write((const char*)& num_tags,  sizeof(int));

                for(int k=0;k<num_elems; ++k,++i){
                    int elem_num = (int )i + 1;
                    fout.write((const char*)&elem_num, sizeof(int));

                    // HACK: hardcoded 2 tags
                    fout.write((const char*)& element_tags[i], sizeof(int));
                    fout.write((const char*)& element_tags[i], sizeof(int));

                    for (size_t e=0; e<elem_len; e++) {
                        int _elem = static_cast<int>( elements[el_ptr + e] )+1;
                        fout.write((const char*)&_elem, sizeof(int));
                    }
                    el_ptr+=elem_len;
                }
            }
        }
    }
    fout << "$EndElements" << std::endl;
    fout.flush();
}

IGL_INLINE void igl::MshSaver::save_scalar_field(const std::string& fieldname, const FloatVector& field) {
    assert(field.size() == m_num_nodes);
    fout << "$NodeData" << std::endl;
    fout << "1" << std::endl; // num string tags.
    fout << "\"" << fieldname << "\"" << std::endl;
    fout << "1" << std::endl; // num real tags.
    fout << "0.0" << std::endl; // time value.
    fout << "3" << std::endl; // num int tags.
    fout << "0" << std::endl; // the time step
    fout << "1" << std::endl; // 1-component scalar field.
    fout << m_num_nodes << std::endl; // number of nodes

    if (m_binary) {
        for (size_t i=0; i<m_num_nodes; i++) {
            int node_idx = i+1;
            fout.write((char*)&node_idx, sizeof(int));
            fout.write((char*)&field[i], sizeof(Float));
        }
    } else {
        for (size_t i=0; i<m_num_nodes; i++) {
            int node_idx = i+1;
            fout << node_idx << " " << field[i] << std::endl;
        }
    }
    fout << "$EndNodeData" << std::endl;
    fout.flush();
}

IGL_INLINE void igl::MshSaver::save_vector_field(const std::string& fieldname, const FloatVector& field) {
    assert(field.size() == 3 * m_num_nodes);

    fout << "$NodeData" << std::endl;
    fout << "1" << std::endl; // num string tags.
    fout << "\"" << fieldname << "\"" << std::endl;
    fout << "1" << std::endl; // num real tags.
    fout << "0.0" << std::endl; // time value.
    fout << "3" << std::endl; // num int tags.
    fout << "0" << std::endl; // the time step
    fout << "3" << std::endl; // 3-component vector field.
    fout << m_num_nodes << std::endl; // number of nodes

    if (m_binary) {
        for (size_t i=0; i<m_num_nodes; i++) {
            int node_idx = i+1;
            fout.write((const char*)&node_idx, sizeof(int));
            fout.write((const char*)&field[i*3], sizeof(Float)*3);
        }
    } else {
        for (size_t i=0; i<m_num_nodes; i++) {
            int node_idx = i+1;
                fout << node_idx
                    << " " << field[i*3]
                    << " " << field[i*3+1]
                    << " " << field[i*3+2]
                    << std::endl;
        }
    }
    fout << "$EndNodeData" << std::endl;
    fout.flush();
}

IGL_INLINE void igl::MshSaver::save_elem_scalar_field(const std::string& fieldname, const FloatVector& field) {
    assert(field.size() == m_num_elements);
    fout << "$ElementData" << std::endl;
    fout << 1 << std::endl; // num string tags.
    fout << "\"" << fieldname << "\"" << std::endl;
    fout << "1" << std::endl; // num real tags.
    fout << "0.0" << std::endl; // time value.
    fout << "3" << std::endl; // num int tags.
    fout << "0" << std::endl; // the time step
    fout << "1" << std::endl; // 1-component scalar field.
    fout << m_num_elements << std::endl; // number of elements

    if (m_binary) {
        for (size_t i=0; i<m_num_elements; i++) {
            int elem_idx = i+1;
            fout.write((const char*)&elem_idx, sizeof(int));
            fout.write((const char*)&field[i], sizeof(Float));
        }
    } else {
        for (size_t i=0; i<m_num_elements; i++) {
            int elem_idx = i+1;
            fout << elem_idx << " " << field[i] << std::endl;
        }
    }

    fout << "$EndElementData" << std::endl;
    fout.flush();
}

IGL_INLINE void igl::MshSaver::save_elem_vector_field(const std::string& fieldname, const FloatVector& field) {
    assert(field.size() == m_num_elements * 3);
    fout << "$ElementData" << std::endl;
    fout << 1 << std::endl; // num string tags.
    fout << "\"" << fieldname << "\"" << std::endl;
    fout << "1" << std::endl; // num real tags.
    fout << "0.0" << std::endl; // time value.
    fout << "3" << std::endl; // num int tags.
    fout << "0" << std::endl; // the time step
    fout << "3" << std::endl; // 3-component vector field.
    fout << m_num_elements << std::endl; // number of elements

    if (m_binary) {
        for (size_t i=0; i<m_num_elements; ++i) {
            int elem_idx = i+1;
            fout.write((const char*)&elem_idx, sizeof(int));
            fout.write((const char*)&field[i*3], sizeof(Float) * 3);
        }
    } else {
        for (size_t i=0; i<m_num_elements; ++i) {
            int elem_idx = i+1;
            fout << elem_idx
                << " " << field[i*3]
                << " " << field[i*3+1]
                << " " << field[i*3+2]
                << std::endl;
        }
    }

    fout << "$EndElementData" << std::endl;
    fout.flush();
}


IGL_INLINE void igl::MshSaver::save_elem_tensor_field(const std::string& fieldname, const FloatVector& field) {
    assert(field.size() == m_num_elements * 3 * (3 + 1) / 2);
    fout << "$ElementData" << std::endl;
    fout << 1 << std::endl; // num string tags.
    fout << "\"" << fieldname << "\"" << std::endl;
    fout << "1" << std::endl; // num real tags.
    fout << "0.0" << std::endl; // time value.
    fout << "3" << std::endl; // num int tags.
    fout << "0" << std::endl; // the time step
    fout << "9" << std::endl; // 9-component tensor field.
    fout << m_num_elements << std::endl; // number of elements

    
    if (m_binary) {
        for (size_t i=0; i<m_num_elements; i++) {
            int elem_idx = i+1;
            fout.write((char*)&elem_idx, sizeof(int));
            //const VectorF& val = field.segment(i*6, 6);
            const Float* val = &field[i*6];
            Float tensor[9] = {
                val[0], val[5], val[4],
                val[5], val[1], val[3],
                val[4], val[3], val[2] };
            fout.write((char*)tensor, sizeof(Float) * 9);
        }
    } else {
        for (size_t i=0; i<m_num_elements; i++) {
            int elem_idx = i+1;
            const Float* val = &field[i*6];
            fout << elem_idx
                << " " << val[0]
                << " " << val[5]
                << " " << val[4]
                << " " << val[5]
                << " " << val[1]
                << " " << val[3]
                << " " << val[4]
                << " " << val[3]
                << " " << val[2]
                << std::endl;
        }
    }

    fout << "$EndElementData" << std::endl;
    fout.flush();
}
