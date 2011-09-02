
// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "readMSH.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>

template <
  typename DerivedV,
  typename DerivedT>
IGL_INLINE bool igl::readMSH(
  const std::string & filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedT> & T)
{
  // https://github.com/Yixin-Hu/TetWild/blob/master/pymesh/MshSaver.cpp
  // Original copyright: /* This file is part of PyMesh. Copyright (c) 2015 by Qingnan Zhou */
  typedef typename DerivedV::Scalar Float;
  typedef Eigen::Matrix<Float,Eigen::Dynamic,1> VectorF;
  typedef Eigen::Matrix<int,Eigen::Dynamic,1> VectorI;
  typedef std::map<std::string, VectorF> FieldMap;
  typedef std::vector<std::string> FieldNames;
  VectorF m_nodes;
  VectorI m_elements;
  FieldMap m_node_fields;
  FieldMap m_element_fields;

  bool m_binary;
  size_t m_data_size;
  size_t m_nodes_per_element;
  size_t m_element_type;
  std::ifstream fin(filename.c_str(), std::ios::in | std::ios::binary);
  if (!fin.is_open())
  {
    std::stringstream err_msg;
    err_msg << "failed to open file \"" << filename << "\"";
    return false;
  }
  // Parse header
  std::string buf;
  double version;
  int type;
  fin >> buf;
  const auto invalid_format = []()->bool
  {
    assert(false && "Invalid format");
    return false;
  };
  const auto not_implemented = []()->bool
  {
    assert(false && "Not implemented");
    return false;
  };
  if (buf != "$MeshFormat") { return invalid_format(); }

  fin >> version >> type >> m_data_size;
  m_binary = (type == 1);

  // Some sanity check.
  if (m_data_size != 8) {
      std::cerr << "Error: data size must be 8 bytes." << std::endl;
      return not_implemented();
  }
  if (sizeof(int) != 4) {
      std::cerr << "Error: code must be compiled with int size 4 bytes." << std::endl;
      return not_implemented();
  }
  const auto eat_white_space = [](std::ifstream& fin)
  {
    char next = fin.peek();
    while (next == '\n' || next == ' ' || next == '\t' || next == '\r') 
    {
      fin.get();
      next = fin.peek();
    }
  };

  // Read in extra info from binary header.
  if (m_binary) {
      int one;
      eat_white_space(fin);
      fin.read(reinterpret_cast<char*>(&one), sizeof(int));
      if (one != 1) {
          std::cerr << "Warning: binary msh file " << filename
              << " is saved with different endianness than this machine."
              << std::endl;
          return not_implemented();
      }
  }

  fin >> buf;
  if (buf != "$EndMeshFormat") { return not_implemented(); }

  const auto num_nodes_per_elem_type = [](int elem_type)->int 
  {
    size_t nodes_per_element = 0;
    switch (elem_type) {
        case 2:
            nodes_per_element = 3; // Triangle
            break;
        case 3:
            nodes_per_element = 4; // Quad
            break;
        case 4:
            nodes_per_element = 4; // Tet
            break;
        case 5:
            nodes_per_element = 8; // hexahedron
            break;
        default:
            assert(false && "not implemented");
            nodes_per_element = -1;
            break;
    }
    return nodes_per_element;
  };

  const auto parse_nodes = [&](std::ifstream& fin) 
  {
    size_t num_nodes;
    fin >> num_nodes;
    m_nodes.resize(num_nodes*3);

    if (m_binary) {
        size_t num_bytes = (4+3*m_data_size) * num_nodes;
        char* data = new char[num_bytes];
        eat_white_space(fin);
        fin.read(data, num_bytes);

        for (size_t i=0; i<num_nodes; i++) {
            int node_idx          = *reinterpret_cast<int*>  (&data[i*(4+3*m_data_size)]) - 1;
            m_nodes[node_idx*3]   = *reinterpret_cast<Float*>(&data[i*(4+3*m_data_size) + 4]);
            m_nodes[node_idx*3+1] = *reinterpret_cast<Float*>(&data[i*(4+3*m_data_size) + 4 + m_data_size]);
            m_nodes[node_idx*3+2] = *reinterpret_cast<Float*>(&data[i*(4+3*m_data_size) + 4 + 2*m_data_size]);
        }

        delete [] data;
    } else {
        int node_idx;
        for (size_t i=0; i<num_nodes; i++) {
            fin >> node_idx;
            node_idx -= 1;
            fin >> m_nodes[node_idx*3]
                >> m_nodes[node_idx*3+1]
                >> m_nodes[node_idx*3+2];
        }
    }
  };

  const auto parse_elements = [&](std::ifstream& fin) 
  {
    size_t num_elements;
    fin >> num_elements;

    // Tmp storage of elements;
    std::vector<int> triangle_element_idx;
    std::vector<int> triangle_elements;
    std::vector<int> quad_element_idx;
    std::vector<int> quad_elements;
    std::vector<int> tet_element_idx;
    std::vector<int> tet_elements;
    std::vector<int> hex_element_idx;
    std::vector<int> hex_elements;

    auto get_element_storage = [&](int elem_type) -> std::vector<int>* {
        switch (elem_type) {
            default:
                assert(false && "Unsupported element type encountered");
            case 2:
                return &triangle_elements;
            case 3:
                return &quad_elements;
            case 4:
                return &tet_elements;
            case 5:
                return &hex_elements;
        };
    };

    auto get_element_idx_storage = [&](int elem_type) -> std::vector<int>* {
        switch (elem_type) {
            default:
                assert(false && "Unsupported element type encountered");
            case 2:
                return &triangle_element_idx;
            case 3:
                return &quad_element_idx;
            case 4:
                return &tet_element_idx;
            case 5:
                return &hex_element_idx;
        };
    };

    size_t nodes_per_element;
    int glob_elem_type = -1;


  if (m_binary) 
  {
    eat_white_space(fin);
    int elem_read = 0;
    while (elem_read < num_elements) {
        // Parse element header.
        int elem_type, num_elems, num_tags;
        fin.read((char*)&elem_type, sizeof(int));
        fin.read((char*)&num_elems, sizeof(int));
        fin.read((char*)&num_tags, sizeof(int));
        nodes_per_element = num_nodes_per_elem_type(elem_type);
        std::vector<int>& elements = *get_element_storage(elem_type);
        std::vector<int>& element_idx = *get_element_idx_storage(elem_type);

        for (size_t i=0; i<num_elems; i++) {
            int elem_idx;
            fin.read((char*)&elem_idx, sizeof(int));
            elem_idx -= 1;
            element_idx.push_back(elem_idx);

            // Eat up tags.
            for (size_t j=0; j<num_tags; j++) {
                int tag;
                fin.read((char*)&tag, sizeof(int));
            }

            // Element values.
            for (size_t j=0; j<nodes_per_element; j++) {
                int idx;
                fin.read((char*)&idx, sizeof(int));
                elements.push_back(idx-1);
            }
        }

        elem_read += num_elems;
    }
  } else 
  {
        for (size_t i=0; i<num_elements; i++) {
            // Parse per element header
            int elem_num, elem_type, num_tags;
            fin >> elem_num >> elem_type >> num_tags;
            for (size_t j=0; j<num_tags; j++) {
                int tag;
                fin >> tag;
            }
            nodes_per_element = num_nodes_per_elem_type(elem_type);
            std::vector<int>& elements = *get_element_storage(elem_type);
            std::vector<int>& element_idx = *get_element_idx_storage(elem_type);

            elem_num -= 1;
            element_idx.push_back(elem_num);

            // Parse node idx.
            for (size_t j=0; j<nodes_per_element; j++) {
                int idx;
                fin >> idx;
                elements.push_back(idx-1); // msh index starts from 1.
            }
        }
    }

    auto copy_to_array = [&](
            const std::vector<int>& elements,
            const int nodes_per_element) {
        const size_t num_elements = elements.size() / nodes_per_element;
        if (elements.size() % nodes_per_element != 0) {
            assert(false && "parsing element failed");
            return;
        }
        m_elements.resize(elements.size());
        std::copy(elements.begin(), elements.end(), m_elements.data());
        m_nodes_per_element = nodes_per_element;
    };

    if (!tet_elements.empty()) {
        copy_to_array(tet_elements, 4);
        m_element_type = 4;
    } else if (!hex_elements.empty()) {
        copy_to_array(hex_elements, 8);
        m_element_type = 5;
    } else if (!triangle_elements.empty()) {
        copy_to_array(triangle_elements, 3);
        m_element_type = 2;
    } else if (!quad_elements.empty()) {
        copy_to_array(quad_elements, 4);
        m_element_type = 3;
    } else {
        // 0 elements, use triangle by default.
        m_element_type = 2;
    }
  };
  const auto parse_element_field = [&](std::ifstream& fin) 
  {
    size_t num_string_tags;
    size_t num_real_tags;
    size_t num_int_tags;

    fin >> num_string_tags;
    std::string* str_tags = new std::string[num_string_tags];
    for (size_t i=0; i<num_string_tags; i++) {
        eat_white_space(fin);
        if (fin.peek() == '\"') {
            // Handle field name between quoates.
            char buf[128];
            fin.get(); // remove the quote at the beginning.
            fin.getline(buf, 128, '\"');
            str_tags[i] = std::string(buf);
        } else {
            fin >> str_tags[i];
        }
    }

    fin >> num_real_tags;
    Float* real_tags = new Float[num_real_tags];
    for (size_t i=0; i<num_real_tags; i++)
        fin >> real_tags[i];

    fin >> num_int_tags;
    int* int_tags = new int[num_int_tags];
    for (size_t i=0; i<num_int_tags; i++)
        fin >> int_tags[i];

    if (num_string_tags <= 0 || num_int_tags <= 2) { assert(false && "invalid format"); return; }
    std::string fieldname = str_tags[0];
    int num_components = int_tags[1];
    int num_entries = int_tags[2];
    VectorF field(num_entries * num_components);

    delete [] str_tags;
    delete [] real_tags;
    delete [] int_tags;

    if (m_binary) {
        size_t num_bytes = (num_components * m_data_size + 4) * num_entries;
        char* data = new char[num_bytes];
        eat_white_space(fin);
        fin.read(data, num_bytes);
        for (size_t i=0; i<num_entries; i++) {
            int elem_idx = *reinterpret_cast<int*>(&data[i*(4+num_components*m_data_size)]);
            elem_idx -= 1;
            size_t base_idx = i*(4+num_components*m_data_size) + 4;
            for (size_t j=0; j<num_components; j++) {
                field[elem_idx * num_components + j] = *reinterpret_cast<Float*>(&data[base_idx+j*m_data_size]);
            }
        }
        delete [] data;
    } else {
        int elem_idx;
        for (size_t i=0; i<num_entries; i++) {
            fin >> elem_idx;
            elem_idx -= 1;
            for (size_t j=0; j<num_components; j++) {
                fin >> field[elem_idx * num_components + j];
            }
        }
    }

    m_element_fields[fieldname] = field;
  };

  const auto parse_node_field = [&](std::ifstream& fin) 
  {
    size_t num_string_tags;
    size_t num_real_tags;
    size_t num_int_tags;

    fin >> num_string_tags;
    std::string* str_tags = new std::string[num_string_tags];
    for (size_t i=0; i<num_string_tags; i++) {
        eat_white_space(fin);
        if (fin.peek() == '\"') {
            // Handle field name between quoates.
            char buf[128];
            fin.get(); // remove the quote at the beginning.
            fin.getline(buf, 128, '\"');
            str_tags[i] = std::string(buf);
        } else {
            fin >> str_tags[i];
        }
    }

    fin >> num_real_tags;
    Float* real_tags = new Float[num_real_tags];
    for (size_t i=0; i<num_real_tags; i++)
        fin >> real_tags[i];

    fin >> num_int_tags;
    int* int_tags = new int[num_int_tags];
    for (size_t i=0; i<num_int_tags; i++)
        fin >> int_tags[i];

    if (num_string_tags <= 0 || num_int_tags <= 2) { assert(false && "invalid format"); return; }
    std::string fieldname = str_tags[0];
    int num_components = int_tags[1];
    int num_entries = int_tags[2];
    VectorF field(num_entries * num_components);

    delete [] str_tags;
    delete [] real_tags;
    delete [] int_tags;

    if (m_binary) {
        size_t num_bytes = (num_components * m_data_size + 4) * num_entries;
        char* data = new char[num_bytes];
        eat_white_space(fin);
        fin.read(data, num_bytes);
        for (size_t i=0; i<num_entries; i++) {
            int node_idx = *reinterpret_cast<int*>(&data[i*(4+num_components*m_data_size)]);
            node_idx -= 1;
            size_t base_idx = i*(4+num_components*m_data_size) + 4;
            for (size_t j=0; j<num_components; j++) {
                field[node_idx * num_components + j] = *reinterpret_cast<Float*>(&data[base_idx+j*m_data_size]);
            }
        }
        delete [] data;
    } else {
        int node_idx;
        for (size_t i=0; i<num_entries; i++) {
            fin >> node_idx;
            node_idx -= 1;
            for (size_t j=0; j<num_components; j++) {
                fin >> field[node_idx * num_components + j];
            }
        }
    }

    m_node_fields[fieldname] = field;
  };
  const auto parse_unknown_field = [](std::ifstream& fin,
        const std::string& fieldname) 
  {
    std::cerr << "Warning: \"" << fieldname << "\" not supported yet.  Ignored." << std::endl;
    std::string endmark = fieldname.substr(0,1) + "End"
        + fieldname.substr(1,fieldname.size()-1);

    std::string buf("");
    while (buf != endmark && !fin.eof()) {
        fin >> buf;
    }
  };


  while (!fin.eof()) {
      buf.clear();
      fin >> buf;
      if (buf == "$Nodes") {
          parse_nodes(fin);
          fin >> buf;
          if (buf != "$EndNodes") { return invalid_format(); }
      } else if (buf == "$Elements") {
          parse_elements(fin);
          fin >> buf;
          if (buf != "$EndElements") { return invalid_format(); }
      } else if (buf == "$NodeData") {
          parse_node_field(fin);
          fin >> buf;
          if (buf != "$EndNodeData") { return invalid_format(); }
      } else if (buf == "$ElementData") {
          parse_element_field(fin);
          fin >> buf;
          if (buf != "$EndElementData") { return invalid_format(); }
      } else if (fin.eof()) {
          break;
      } else {
          parse_unknown_field(fin, buf);
      }
  }
  fin.close();
  V.resize(m_nodes.rows()/3,3);
  for (int i = 0; i < m_nodes.rows() / 3; i++) 
  {
    for (int j = 0; j < 3; j++)
    {
      V(i,j) = m_nodes(i * 3 + j);
    }
  }
  int ss = num_nodes_per_elem_type(m_element_type);
  T.resize(m_elements.rows()/ss,ss);
  for (int i = 0; i < m_elements.rows() / ss; i++) 
  {
    for (int j = 0; j < ss; j++)
    {
      T(i, j) = m_elements(i * ss + j);
    }
  }
  return true;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
