// based on MSH reader from PyMesh 

// Copyright (c) 2015 Qingnan Zhou <qzhou@adobe.com>           
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/. 
#ifndef IGL_MSH_LOADER_H
#define IGL_MSH_LOADER_H
#include "igl_inline.h"

#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace igl {

/// Class for loading information from .msh file
/// depends only on c++stl library
class MshLoader {
    public:

        struct msh_struct {
            int tag,el_type;
            msh_struct(int _tag=0,int _type=0):
                tag(_tag),el_type(_type){}
            bool operator== (const msh_struct& a) const {
                return this->tag==a.tag && 
                       this->el_type==a.el_type;
            }

            bool operator< (const msh_struct& a) const {
                return (this->tag*100+this->el_type) < 
                       (a.tag*100+a.el_type);
            }
        };

        typedef double Float;
        
        typedef std::vector<int>      IndexVector;
        typedef std::vector<int>      IntVector;
        typedef std::vector<Float>    FloatVector;
        typedef std::vector<FloatVector> FloatField;
        typedef std::vector<IntVector> IntField;
        typedef std::vector<std::string> FieldNames;
        typedef std::multimap<msh_struct,int> StructIndex;
        typedef std::vector<msh_struct> StructVector;

        enum {ELEMENT_LINE=1, ELEMENT_TRI=2, ELEMENT_QUAD=3, 
              ELEMENT_TET=4,  ELEMENT_HEX=5, ELEMENT_PRISM=6,
              ELEMENT_PYRAMID=7,
              // 2nd order elements
              ELEMENT_LINE_2ND_ORDER=8, ELEMENT_TRI_2ND_ORDER=9, 
              ELEMENT_QUAD_2ND_ORDER=10,ELEMENT_TET_2ND_ORDER=11, 
              ELEMENT_HEX_2ND_ORDER=12, ELEMENT_PRISM_2ND_ORDER=13, 
              ELEMENT_PYRAMID_2ND_ORDER=14,
              // other elements
              ELEMENT_POINT=15 };
    public:
        /// Load a .msh file from a given path
        /// @param[in] filename  path to .msh
        MshLoader(const std::string &filename);

    public:

        // get nodes , x,y,z sequentially
        const FloatVector& get_nodes()    const { return m_nodes; } 
        // get elements , identifying nodes that create an element
        // variable length per element
        const IndexVector& get_elements() const { return m_elements; }

        // get element types 
        const IntVector& get_elements_types() const { return m_elements_types; }
        // get element lengths
        const IntVector& get_elements_lengths() const { return m_elements_lengths; }
        // get element tags ( physical (0) and elementary (1) )
        const IntField&  get_elements_tags() const { return m_elements_tags; }
        // get element IDs
        const IntVector& get_elements_ids() const { return m_elements_ids; }

        // get reverse index from node to element
        const IndexVector& get_elements_nodes_idx() const { return m_elements_nodes_idx; }

        // get fields assigned per node, all fields and components sequentially
        const FloatField& get_node_fields() const { return m_node_fields;}
        // get node field names, 
        const FieldNames& get_node_fields_names() const { return m_node_fields_names;}
        // get number of node field components
        const IntVector&  get_node_fields_components() const {return m_node_fields_components;}

        int get_node_field_components(size_t c)  const 
        {
            return m_node_fields_components[c];
        }

        // get fields assigned per element, all fields and components sequentially
        const FloatField& get_element_fields() const { return m_element_fields;}
        // get element field names
        const FieldNames& get_element_fields_names() const { return m_element_fields_names;}
        // get number of element field components
        const IntVector&  get_element_fields_components() const {return m_element_fields_components;}

        int get_element_field_components(size_t c)  const {
            return m_element_fields_components[c];
        }
        // check if field is present at node level
        bool is_node_field(const std::string& fieldname)  const {
            return (std::find(std::begin(m_node_fields_names),
                              std::end(m_node_fields_names),
                              fieldname) != std::end(m_node_fields_names) );
        }
        // check if field is present at element level
        bool is_element_field(const std::string& fieldname) const {
            return (std::find(std::begin(m_element_fields_names),
                              std::end(m_element_fields_names),
                              fieldname) != std::end(m_node_fields_names) );
        }

        // check if all elements have ids assigned sequentially
        bool is_element_map_identity() const ;

        // create tag index
        // tag_column: ( physical (0) or elementary (1) ) specifying which tag to use
        void index_structures(int tag_column); 

        // get tag index, call index_structure_tags first
        const StructIndex& get_structure_index() const 
        {
            return m_structure_index;
        }

        // get size of a structure identified by tag and element type
        const StructIndex& get_structure_length() const 
        {
            return m_structure_length;
        }

        //! get list of structures
        const StructVector& get_structures() const 
        {
            return m_structures;
        }
        
    public:
        // helper function, calculate number of nodes associated with an element
        static int num_nodes_per_elem_type(int elem_type);

    private:
        void parse_nodes(std::ifstream& fin);
        void parse_elements(std::ifstream& fin);
        void parse_node_field(std::ifstream& fin);
        void parse_element_field(std::ifstream& fin);
        void parse_unknown_field(std::ifstream& fin,
                const std::string& fieldname);

    private:
        bool   m_binary;
        size_t m_data_size;
        
        FloatVector m_nodes;    // len x 3 vector 

        IndexVector m_elements; // linear array for nodes corresponding to each element 
        IndexVector m_elements_nodes_idx; // element indexes  

        IntVector   m_elements_ids;     // element id's 
        IntVector   m_elements_types;   // Element types 
        IntVector   m_elements_lengths; // Element lengths 
        IntField    m_elements_tags;    // Element tags, currently 2xtags per element 

        FloatField  m_node_fields;      // Float field defined at each node 
        IntVector   m_node_fields_components; // Number of components for node field 
        FieldNames  m_node_fields_names; // Node field name 

        FloatField  m_element_fields;    // Float field defined at each element 
        IntVector   m_element_fields_components; // Number of components for element field 
        FieldNames  m_element_fields_names; // Element field name 

        StructIndex  m_structure_index; // index tag ids  
        StructVector m_structures;  // unique structures
        StructIndex  m_structure_length; // length of structures with consistent element type
};

} //igl

#ifndef IGL_STATIC_LIBRARY
#  include "MshLoader.cpp"
#endif

#endif //IGL_MSH_LOADER_H
