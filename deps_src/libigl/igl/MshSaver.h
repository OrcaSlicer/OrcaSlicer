// based on MSH writer from PyMesh 

// Copyright (c) 2015 Qingnan Zhou <qzhou@adobe.com>           
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla 
// Public License v. 2.0. If a copy of the MPL was not distributed 
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/. 
#ifndef IGL_MSH_SAVER_H
#define IGL_MSH_SAVER_H
#include "igl_inline.h"

#include <fstream>
#include <string>
#include <vector>

namespace igl {

/// Class for dumping information to .msh file
/// depends only on c++stl library
/// current implementation works only with 3D information
class MshSaver {
    public:
        typedef double Float;

        typedef std::vector<int>         IndexVector;
        typedef std::vector<int>         IntVector;
        typedef std::vector<Float>       FloatVector;
        typedef std::vector<FloatVector> FloatField;
        typedef std::vector<IntVector>   IntField;
        typedef std::vector<std::string> FieldNames;

        /// Write a .msh to a given path
        /// @param[in] filename  path to output file
        /// @param[in] binary    whether to write in binary format
        MshSaver(const std::string& filename, bool binary=true);
        ~MshSaver();

    public:
        // Only these element types are supported right now
        enum {ELEMENT_LINE=1, ELEMENT_TRI=2, ELEMENT_QUAD=3, 
              ELEMENT_TET=4,  ELEMENT_HEX=5, ELEMENT_PRISM=6 };

    public:
        // save mesh geometry
        void save_mesh(
            const FloatVector& nodes, 
            const IndexVector& elements, 
            const IntVector& element_lengths,
            const IntVector& element_type,
            const IntVector& element_tags );
        
        // save additional fields associated with the mesh

        // add node scalar field
        void save_scalar_field(const std::string& fieldname, const FloatVector& field);
        // add node vectot field
        void save_vector_field(const std::string& fieldname, const FloatVector& field);
        // add element scalar field
        void save_elem_scalar_field(const std::string& fieldname, const FloatVector& field);
        // add element vector field
        void save_elem_vector_field(const std::string& fieldname, const FloatVector& field);
        // add element tensor field
        void save_elem_tensor_field(const std::string& fieldname, const FloatVector& field);

    protected:
        void save_header();
        void save_nodes(const FloatVector& nodes);
        void save_elements(const IndexVector& elements, 
            const IntVector& element_lengths,
            const IntVector& element_type,
            const IntVector& element_tags);

    private:
        bool m_binary;
        size_t m_num_nodes;
        size_t m_num_elements;

        std::ofstream fout;
};
} //igl

#ifndef IGL_STATIC_LIBRARY
#  include "MshSaver.cpp"
#endif

#endif //MSH_SAVER_H
