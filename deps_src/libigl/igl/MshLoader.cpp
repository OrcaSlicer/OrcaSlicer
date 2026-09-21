// based on MSH reader from PyMesh 

// Copyright (c) 2015 Qingnan Zhou <qzhou@adobe.com>           
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/. 

#include "MshLoader.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <vector>

#include <string.h>

namespace igl {
    // helper function
    void inline _msh_eat_white_space(std::ifstream& fin) {
        char next = fin.peek();
        while (next == '\n' || next == ' ' || next == '\t' || next == '\r') {
            fin.get();
            next = fin.peek();
        }
    }
}

IGL_INLINE igl::MshLoader::MshLoader(const std::string &filename) {
    std::ifstream fin(filename, std::ios::in | std::ios::binary);

    if (!fin.is_open()) {
        std::stringstream err_msg;
        err_msg << "failed to open file \"" << filename << "\"";
        throw std::ios_base::failure(err_msg.str());
    }
    // Parse header
    std::string buf;
    double version;
    int type;
    fin >> buf;
    if (buf != "$MeshFormat") { throw std::runtime_error("Unexpected .msh format"); }

    fin >> version >> type >> m_data_size;
    m_binary = (type == 1);
    if(version>2.2 || version<2.0)
    {
        // probably unsupported version
        std::stringstream err_msg;
        err_msg << "Error: Unsupported file version:" << version << std::endl;
        throw std::runtime_error(err_msg.str());

    }
    // Some sanity check.
    if (m_data_size != 8) {
        std::stringstream err_msg;
        err_msg << "Error: data size must be 8 bytes." << std::endl;
        throw std::runtime_error(err_msg.str());
    }
    if (sizeof(int) != 4) {
        std::stringstream err_msg;
        err_msg << "Error: code must be compiled with int size 4 bytes." << std::endl;
        throw std::runtime_error(err_msg.str());
    }

    // Read in extra info from binary header.
    if (m_binary) {
        int one;
        igl::_msh_eat_white_space(fin);
        fin.read(reinterpret_cast<char*>(&one), sizeof(int));
        if (one != 1) {
            std::stringstream err_msg;
                err_msg << "Binary msh file " << filename
                << " is saved with different endianness than this machine."
                << std::endl;
            throw std::runtime_error(err_msg.str());
        }
    }

    fin >> buf;
    if (buf != "$EndMeshFormat") 
    { 
        std::stringstream err_msg;
        err_msg << "Unexpected contents in the file header." << std::endl;
        throw std::runtime_error(err_msg.str());
    }

    while (!fin.eof()) {
        buf.clear();
        fin >> buf;
        if (buf == "$Nodes") {
            parse_nodes(fin);
            fin >> buf;
            if (buf != "$EndNodes") { throw std::runtime_error("Unexpected tag"); }
        } else if (buf == "$Elements") {
            parse_elements(fin);
            fin >> buf;
            if (buf != "$EndElements") { throw std::runtime_error("Unexpected tag"); }
        } else if (buf == "$NodeData") {
            parse_node_field(fin);
            fin >> buf;
            if (buf != "$EndNodeData") { throw std::runtime_error("Unexpected tag"); }
        } else if (buf == "$ElementData") {
            parse_element_field(fin);
            fin >> buf;
            if (buf != "$EndElementData") { throw std::runtime_error("Unexpected tag"); }
        } else if (fin.eof()) {
            break;
        } else {
            parse_unknown_field(fin, buf);
        }
    }
    fin.close();
}

IGL_INLINE void igl::MshLoader::parse_nodes(std::ifstream& fin) {
    size_t num_nodes;
    fin >> num_nodes;
    m_nodes.resize(num_nodes*3);

    if (m_binary) {
		size_t stride = (4+3*m_data_size);
        size_t num_bytes = stride * num_nodes;
        char* data = new char[num_bytes];
        igl::_msh_eat_white_space(fin);
        fin.read(data, num_bytes);

        for (size_t i=0; i<num_nodes; i++) {
            int node_idx;
			memcpy(&node_idx, data+i*stride, sizeof(int));
			node_idx-=1;
			// directly move into vector storage
			// this works only when m_data_size==sizeof(Float)==sizeof(double)
			memcpy(&m_nodes[node_idx*3], data+i*stride + 4, m_data_size*3);
        }
        delete [] data;
    } else {
        int node_idx;
        for (size_t i=0; i<num_nodes; i++) {
            fin >> node_idx;
            node_idx -= 1;
            // here it's 3D node explicitly
            fin >> m_nodes[node_idx*3]
                >> m_nodes[node_idx*3+1]
                >> m_nodes[node_idx*3+2];
        }
    }
}

IGL_INLINE void igl::MshLoader::parse_elements(std::ifstream& fin) {
    m_elements_tags.resize(2); //hardcoded to have 2 tags
    size_t num_elements;
    fin >> num_elements;

    size_t nodes_per_element;

    if (m_binary) {
        igl::_msh_eat_white_space(fin);
        int elem_read = 0;
        while (elem_read < num_elements) {
            // Parse element header.
            int elem_type, num_elems, num_tags;
            fin.read((char*)&elem_type, sizeof(int));
            fin.read((char*)&num_elems, sizeof(int));
            fin.read((char*)&num_tags,  sizeof(int));
            nodes_per_element = num_nodes_per_elem_type(elem_type);

            // store node info
            for (size_t i=0; i<num_elems; i++) {
                int elem_idx;

                // all elements in the segment share the same elem_type and number of nodes per element
                m_elements_types.push_back(elem_type);
                m_elements_lengths.push_back(nodes_per_element);

                fin.read((char*)&elem_idx, sizeof(int));
                elem_idx -= 1;
                m_elements_ids.push_back(elem_idx);

                // read first two tags
                for (size_t j=0; j<num_tags; j++) {
                    int tag;
                    fin.read((char*)&tag, sizeof(int));
                    if(j<2) m_elements_tags[j].push_back(tag);
                }

                for (size_t j=num_tags; j<2; j++) 
                    m_elements_tags[j].push_back(-1); // fill up tags if less then 2

                m_elements_nodes_idx.push_back(m_elements.size());
                // Element values.
                for (size_t j=0; j<nodes_per_element; j++) {
                    int idx;
                    fin.read((char*)&idx, sizeof(int));
                    
                    m_elements.push_back(idx-1);
                }
            }
            elem_read += num_elems;
        }
    } else {
        for (size_t i=0; i<num_elements; i++) {
            // Parse per element header
            int elem_num, elem_type, num_tags;
            fin >> elem_num >> elem_type >> num_tags;

            // read tags.
            for (size_t j=0; j<num_tags; j++) {
                int tag;
                fin >> tag;
                if(j<2) m_elements_tags[j].push_back(tag);
            }
            for (size_t j=num_tags; j<2; j++) 
                m_elements_tags[j].push_back(-1); // fill up tags if less then 2
            
            nodes_per_element = num_nodes_per_elem_type(elem_type);
            m_elements_types.push_back(elem_type);
            m_elements_lengths.push_back(nodes_per_element);

            elem_num -= 1;
            m_elements_ids.push_back(elem_num);
            m_elements_nodes_idx.push_back(m_elements.size());
            // Parse node idx.
            for (size_t j=0; j<nodes_per_element; j++) {
                int idx;
                fin >> idx;
                m_elements.push_back(idx-1); // msh index starts from 1.
            }
        }
    }
    // debug
    assert(m_elements_types.size()   == m_elements_ids.size());
    assert(m_elements_tags[0].size() == m_elements_ids.size());
    assert(m_elements_tags[1].size() == m_elements_ids.size());
    assert(m_elements_lengths.size() == m_elements_ids.size());
}

IGL_INLINE void igl::MshLoader::parse_node_field( std::ifstream& fin ) {
    size_t num_string_tags;
    size_t num_real_tags;
    size_t num_int_tags;

    fin >> num_string_tags;
    std::vector<std::string> str_tags(num_string_tags);

    for (size_t i=0; i<num_string_tags; i++) {
        igl::_msh_eat_white_space(fin);
        if (fin.peek() == '\"') {
            // Handle field name between quotes.
            char buf[128];
            fin.get(); // remove the quote at the beginning.
            fin.getline(buf, 128, '\"');
            str_tags[i] = std::string(buf);
        } else {
            fin >> str_tags[i];
        }
    }

    fin >> num_real_tags;
    std::vector<Float> real_tags(num_real_tags);
    for (size_t i=0; i<num_real_tags; i++)
        fin >> real_tags[i];

    fin >> num_int_tags;
    std::vector<int> int_tags(num_int_tags);
    for (size_t i=0; i<num_int_tags; i++)
        fin >> int_tags[i];

    if (num_string_tags <= 0 || num_int_tags <= 2) {
        throw std::runtime_error("Unexpected number of field tags");
    }
    std::string fieldname = str_tags[0];
    int num_components    = int_tags[1];
    int num_entries       = int_tags[2];

    std::vector<Float> field( num_entries*num_components );

    if (m_binary) {
        size_t num_bytes = (num_components * m_data_size + 4) * num_entries;
        char* data = new char[num_bytes];
        igl::_msh_eat_white_space(fin);
        fin.read(data, num_bytes);
        for (size_t i=0; i<num_entries; i++) {
			int node_idx;
			memcpy(&node_idx,&data[i*(4+num_components*m_data_size)],4);
			
            if(node_idx<1) throw std::runtime_error("Negative or zero index");
            node_idx -= 1;
			
            if(node_idx>=num_entries) throw std::runtime_error("Index too big");
            size_t base_idx = i*(4+num_components*m_data_size) + 4;
            // TODO: make this work when m_data_size != sizeof(double) ?
			memcpy(&field[node_idx*num_components], &data[base_idx], num_components*m_data_size);
        }
        delete [] data;
    } else {
        int node_idx;
        for (size_t i=0; i<num_entries; i++) {
            fin >> node_idx;
            node_idx -= 1;
            for (size_t j=0; j<num_components; j++) {
                fin >> field[node_idx*num_components+j];
            }
        }
    }
    
    m_node_fields_names.push_back(fieldname);
    m_node_fields.push_back(field);
    m_node_fields_components.push_back(num_components);
}

IGL_INLINE void igl::MshLoader::parse_element_field(std::ifstream& fin) {
    size_t num_string_tags;
    size_t num_real_tags;
    size_t num_int_tags;

    fin >> num_string_tags;
    std::vector<std::string> str_tags(num_string_tags);
    for (size_t i=0; i<num_string_tags; i++) {
        igl::_msh_eat_white_space(fin);
        if (fin.peek() == '\"') {
            // Handle field name between quoates.
            char buf[128];
            fin.get(); // remove the quote at the beginning.
            fin.getline(buf, 128, '\"');
            str_tags[i] = buf;
        } else {
            fin >> str_tags[i];
        }
    }

    fin >> num_real_tags;
    std::vector<Float> real_tags(num_real_tags);
    for (size_t i=0; i<num_real_tags; i++)
        fin >> real_tags[i];

    fin >> num_int_tags;
    std::vector<int> int_tags(num_int_tags);
    for (size_t i=0; i<num_int_tags; i++)
        fin >> int_tags[i];

    if (num_string_tags <= 0 || num_int_tags <= 2) {
        throw std::runtime_error("Invalid file format");
    }
    std::string fieldname = str_tags[0];
    int num_components = int_tags[1];
    int num_entries = int_tags[2];
    std::vector<Float> field(num_entries*num_components);

    if (m_binary) {
        size_t num_bytes = (num_components * m_data_size + 4) * num_entries;
        char* data = new char[num_bytes];
        igl::_msh_eat_white_space(fin);
        fin.read(data, num_bytes);
        for (int i=0; i<num_entries; i++) {
			int elem_idx;
			// works with sizeof(int)==4
			memcpy(&elem_idx, &data[i*(4+num_components*m_data_size)],4);
            elem_idx -= 1;
			
			// directly copy data into vector storage space
			memcpy(&field[elem_idx*num_components], &data[i*(4+num_components*m_data_size) + 4], m_data_size*num_components);
        }
        delete [] data;
    } else {
        int elem_idx;
        for (size_t i=0; i<num_entries; i++) {
            fin >> elem_idx;
            elem_idx -= 1;
            for (size_t j=0; j<num_components; j++) {
                fin >> field[elem_idx*num_components+j];
            }
        }
    }
    m_element_fields_names.push_back(fieldname);
    m_element_fields.push_back(field);
    m_element_fields_components.push_back(num_components);
}

IGL_INLINE void igl::MshLoader::parse_unknown_field(std::ifstream& fin,
        const std::string& fieldname) {
    std::cerr << "Warning: \"" << fieldname << "\" not supported yet.  Ignored." << std::endl;
    std::string endmark = fieldname.substr(0,1) + "End"
        + fieldname.substr(1,fieldname.size()-1);

    std::string buf("");
    while (buf != endmark && !fin.eof()) {
        fin >> buf;
    }
}

IGL_INLINE int igl::MshLoader::num_nodes_per_elem_type(int elem_type) {
    int nodes_per_element = 0;
    switch (elem_type) {
        case ELEMENT_LINE:         // 2-node line
            nodes_per_element = 2; 
            break;
        case ELEMENT_TRI:
            nodes_per_element = 3; // 3-node triangle
            break;
        case ELEMENT_QUAD:
            nodes_per_element = 4; // 5-node quad
            break;
        case ELEMENT_TET:
            nodes_per_element = 4; // 4-node tetrahedra
            break;
        case ELEMENT_HEX:          // 8-node hexahedron
            nodes_per_element = 8; 
            break;
        case ELEMENT_PRISM:        // 6-node prism
            nodes_per_element = 6; 
            break;
        case ELEMENT_LINE_2ND_ORDER: 
            nodes_per_element = 3;
            break;
        case ELEMENT_TRI_2ND_ORDER: 
            nodes_per_element = 6;
            break;
        case ELEMENT_QUAD_2ND_ORDER: 
            nodes_per_element = 9;
            break;
        case ELEMENT_TET_2ND_ORDER: 
            nodes_per_element = 10;
            break;
        case ELEMENT_HEX_2ND_ORDER: 
            nodes_per_element = 27;
            break;
        case ELEMENT_PRISM_2ND_ORDER: 
            nodes_per_element = 18;
            break;
        case ELEMENT_PYRAMID_2ND_ORDER: 
            nodes_per_element = 14;
            break;
        case ELEMENT_POINT:        // 1-node point
            nodes_per_element = 1; 
            break;
        default:
            std::stringstream err_msg;
                err_msg << "Element type (" << elem_type << ") is not supported yet."
                << std::endl;
            throw std::runtime_error(err_msg.str());
    }
    return nodes_per_element;
}


IGL_INLINE bool igl::MshLoader::is_element_map_identity() const
{
    for(int i=0;i<m_elements_ids.size();i++) {
        int id=m_elements_ids[i];
        if (id!=i) return false;
    }
    return true;
}


IGL_INLINE void igl::MshLoader::index_structures(int tag_column)
{
    //cleanup
    m_structure_index.clear();
    m_structures.clear();
    m_structure_length.clear();

    //index structure tags
    for(auto i=0; i != m_elements_tags[tag_column].size(); ++i )
    {
        m_structure_index.insert(
            std::pair<msh_struct,int>(
                msh_struct( m_elements_tags[tag_column][i], 
                            m_elements_types[i]), i)
            );
    }

    // identify unique structures 
    std::vector<StructIndex::value_type> _unique_structs;
    std::unique_copy(std::begin(m_structure_index), 
                     std::end(m_structure_index), 
                     std::back_inserter(_unique_structs),
                     [](const StructIndex::value_type &c1, const StructIndex::value_type &c2)
                     { return c1.first == c2.first; });

    std::for_each( _unique_structs.begin(), _unique_structs.end(), 
        [this](const StructIndex::value_type &n){ this->m_structures.push_back(n.first); });

    for(auto t = m_structures.begin(); t != m_structures.end(); ++t)
    {
        // identify all elements corresponding to this tag
        auto structure_range = m_structure_index.equal_range( *t );
        int cnt=0;

        for(auto i=structure_range.first; i!=structure_range.second; i++)
            cnt++;

        m_structure_length.insert( std::pair<msh_struct,int>( *t, cnt));
    }
}
