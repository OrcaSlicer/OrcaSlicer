/*  ADMesh -- process triangulated solid meshes
 *  Copyright (C) 1995, 1996  Anthony D. Martin <amartin@engr.csulb.edu>
 *  Copyright (C) 2013, 2014  several contributors, see AUTHORS
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *  Questions, comments, suggestions, etc to
 *           https://github.com/admesh/admesh/issues
 */

#ifndef __admesh_stl__
#define __admesh_stl__

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include <vector>
#include <Eigen/Geometry> 

// Size of the binary STL header, free form.
#define LABEL_SIZE             80
// Binary STL, length of the "number of faces" counter.
#define NUM_FACET_SIZE         4
// Binary STL, sizeof header + number of faces.
#define HEADER_SIZE            84
#define STL_MIN_FILE_SIZE      284
#define ASCII_LINES_PER_FACET  7

typedef Eigen::Matrix<float, 3, 1, Eigen::DontAlign> stl_vertex;
typedef Eigen::Matrix<float, 3, 1, Eigen::DontAlign> stl_normal;
typedef Eigen::Matrix<int,   3, 1, Eigen::DontAlign> stl_triangle_vertex_indices;
static_assert(sizeof(stl_vertex) == 12, "size of stl_vertex incorrect");
static_assert(sizeof(stl_normal) == 12, "size of stl_normal incorrect");

typedef std::function<void(int current, int total, bool& cancel, std::string& model_id, std::string& code)> ImportstlProgressFn;

typedef enum {
    eNormal,  // normal face
    eSmallOverhang,  // small overhang
    eSmallHole,      // face with small hole
    eExteriorAppearance,  // exterior appearance
    eMaxNumFaceTypes
}EnumFaceTypes;

struct stl_facet {
	stl_normal normal;
	stl_vertex vertex[3];
	char       extra[2];

	stl_facet  rotated(const Eigen::Quaternion<float, Eigen::DontAlign> &rot) const {
		stl_facet out;
		out.normal    = rot * this->normal;
		out.vertex[0] = rot * this->vertex[0];
		out.vertex[1] = rot * this->vertex[1];
		out.vertex[2] = rot * this->vertex[2];
		return out;
	}
};

#define SIZEOF_STL_FACET       50

static_assert(offsetof(stl_facet, normal) == 0, "stl_facet.normal has correct offset");
static_assert(offsetof(stl_facet, vertex) == 12, "stl_facet.vertex has correct offset");
static_assert(offsetof(stl_facet, extra ) == 48, "stl_facet.extra has correct offset");
static_assert(sizeof(stl_facet) >= SIZEOF_STL_FACET, "size of stl_facet incorrect");

typedef enum {binary, ascii, inmemory} stl_type;

struct stl_neighbors {
  	stl_neighbors() { reset(); }
  	void reset() {
  		neighbor[0] = -1;
  		neighbor[1] = -1;
  		neighbor[2] = -1;
  		which_vertex_not[0] = -1;
  		which_vertex_not[1] = -1;
  		which_vertex_not[2] = -1;
  	}
  	int num_neighbors() const { return 3 - ((this->neighbor[0] == -1) + (this->neighbor[1] == -1) + (this->neighbor[2] == -1)); }

  	// Index of a neighbor facet.
  	int   neighbor[3];
  	// Index of an opposite vertex at the neighbor face.
  	char  which_vertex_not[3];
};

struct stl_stats {
    stl_stats() {}
    void  reset_header(int size) {
        header.clear();
        header.resize(size +1);
    }
    std::vector<char>    header;
    stl_type      type                      = (stl_type)0;
    // Should always match the number of facets stored inside stl_file::facet_start.
    uint32_t      number_of_facets          = 0;
    // Bounding box.
    stl_vertex    max                       = stl_vertex::Zero();
    stl_vertex    min                       = stl_vertex::Zero();
    stl_vertex    size                      = stl_vertex::Zero();
    float         bounding_diameter         = 0.f;
    float         shortest_edge             = 0.f;
    // After repair, the volume shall always be positive.
    float         volume                    = -1.f;
    // Number of face edges connected to another face.
    // Don't use this statistics after repair, use the connected_facets_1/2/3_edge instead!
    int           connected_edges           = 0;
    // Faces with >=1, >=2 and 3 edges connected to another face.
    int           connected_facets_1_edge   = 0;
    int           connected_facets_2_edge   = 0;
    int           connected_facets_3_edge   = 0;
    // Faces with 1, 2 and 3 open edges after exact chaining, but before repair.
    int           facets_w_1_bad_edge       = 0;
    int           facets_w_2_bad_edge       = 0;
    int           facets_w_3_bad_edge       = 0;
    // Number of faces read form an STL file.
    int           original_num_facets       = 0;
    // Number of edges connected one to another by snapping their end vertices.
    int           edges_fixed               = 0;
    // Number of faces removed because they were degenerated.
    int           degenerate_facets         = 0;
    // Total number of facets removed: Degenerate faces and unconnected faces.
    int           facets_removed            = 0;
    // Number of faces added by hole filling.
    int           facets_added              = 0;
    // Number of faces reversed because of negative volume or because one patch was connected to another patch with incompatible normals.
    int           facets_reversed           = 0;
    // Number of incompatible edges remaining after the patches were connected together and possibly their normals flipped.
    int           backwards_edges           = 0;
    // Number of triangles, which were flipped during the fixing process.
    int           normals_fixed             = 0;
    // Number of connected triangle patches.
    int           number_of_parts           = 0;

    void clear() { *this = stl_stats(); }
};

struct stl_file {
	stl_file() {}

	void clear() {
		this->facet_start.clear();
		this->neighbors_start.clear();
        this->stats.clear();
	}

	size_t memsize() const {
		return sizeof(*this) + sizeof(stl_facet) * facet_start.size() + sizeof(stl_neighbors) * neighbors_start.size();
	}

    char mw_data[256];
	std::vector<stl_facet>     		facet_start;
	std::vector<stl_neighbors> 		neighbors_start;
	// Statistics
	stl_stats     					stats;
};

struct FaceProperty
{   // triangle face property
    EnumFaceTypes type;
    double area;
    // stl_normal normal;

    std::string to_string() const
    {
        std::string str;
        // skip normal type facet to improve performance 
        if (type > eNormal && type < eMaxNumFaceTypes) {
            str += std::to_string(type);
            if (area != 0.f)
                str += " " + std::to_string(area);
        }
        return str;
    }

    void from_string(const std::string& str)
    {
        std::string val_str, area_str;
        do {
            if (str.empty())
                break;

            this->type = (EnumFaceTypes)std::atoi(str.c_str());
            if (this->type <= eNormal || this->type >= eMaxNumFaceTypes)
                break;

            size_t type_end_pos = str.find(" ");
            if (type_end_pos == std::string::npos) {
                this->area = 0.f;
                return;
            }

            area_str = str.substr(type_end_pos + 1);
            if (!area_str.empty())
                this->area = std::atof(area_str.c_str());
            else
                this->area = 0.f;
            return;
        } while (0);

        this->type = eNormal;
        this->area = 0.f;
    }
};

struct indexed_triangle_set
{
    indexed_triangle_set(std::vector<stl_triangle_vertex_indices>    indices_,
        std::vector<stl_vertex>                     vertices_) :indices(indices_), vertices(vertices_) {
        properties.resize(indices_.size());
    }
    indexed_triangle_set() {}

    void clear() { indices.clear(); vertices.clear(); properties.clear(); }

    size_t memsize() const {
        return sizeof(*this) + (sizeof(stl_triangle_vertex_indices) + sizeof(FaceProperty)) * indices.size() + sizeof(stl_vertex) * vertices.size();
    }

    std::vector<stl_triangle_vertex_indices>    indices;
    std::vector<stl_vertex>                     vertices;
    std::vector<FaceProperty>                   properties;

    bool empty() const { return indices.empty() || vertices.empty(); }
    stl_vertex get_vertex(int facet_idx, int vertex_idx) const{
        return vertices[indices[facet_idx][vertex_idx]];
    }
    float facet_area(int facet_idx) const {
        return std::abs((get_vertex(facet_idx, 0) - get_vertex(facet_idx, 1))
            .cross(get_vertex(facet_idx, 0) - get_vertex(facet_idx, 2)).norm()) / 2;
    }
    FaceProperty& get_property(int face_idx) {
        if (properties.size() != indices.size()) {
            properties.clear();
            properties.resize(indices.size());
        }
        return properties[face_idx];
    }
};

extern bool stl_open(stl_file *stl, const char *file, ImportstlProgressFn stlFn = nullptr,int custom_header_length = 80);
extern void stl_stats_out(stl_file *stl, FILE *file, char *input_file);
extern bool stl_print_neighbors(stl_file *stl, char *file);
extern bool stl_write_ascii(stl_file *stl, const char *file, const char *label);
extern bool stl_write_binary(stl_file *stl, const char *file, const char *label);
extern void stl_check_facets_exact(stl_file *stl);
extern void stl_check_facets_nearby(stl_file *stl, float tolerance);
extern void stl_remove_unconnected_facets(stl_file *stl);
extern void stl_write_vertex(stl_file *stl, int facet, int vertex);
extern void stl_write_facet(stl_file *stl, char *label, int facet);
extern void stl_write_neighbor(stl_file *stl, int facet);
extern bool stl_write_quad_object(stl_file *stl, char *file);
extern void stl_verify_neighbors(stl_file *stl);
extern void stl_fill_holes(stl_file *stl);
extern void stl_fix_normal_directions(stl_file *stl);
extern void stl_fix_normal_values(stl_file *stl);
extern void stl_reverse_all_facets(stl_file *stl);
extern void stl_translate(stl_file *stl, float x, float y, float z);
extern void stl_translate_relative(stl_file *stl, float x, float y, float z);
extern void stl_scale_versor(stl_file *stl, const stl_vertex &versor);
inline void stl_scale(stl_file *stl, float factor) { stl_scale_versor(stl, stl_vertex(factor, factor, factor)); }
extern void stl_rotate_x(stl_file *stl, float angle);
extern void stl_rotate_y(stl_file *stl, float angle);
extern void stl_rotate_z(stl_file *stl, float angle);
extern void stl_mirror_xy(stl_file *stl);
extern void stl_mirror_yz(stl_file *stl);
extern void stl_mirror_xz(stl_file *stl);

extern float get_area(stl_facet* facet);
extern void stl_get_size(stl_file *stl);

// the following function is not used
/*
template<typename T>
extern void stl_transform(stl_file *stl, T *trafo3x4)
{
    Eigen::Matrix<T, 3, 3, Eigen::DontAlign> trafo3x3;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            trafo3x3(i, j) = (i * 4) + j;
        }
    }
    Eigen::Matrix<T, 3, 3, Eigen::DontAlign> r = trafo3x3.inverse().transpose();
    for (uint32_t i_face = 0; i_face < stl->stats.number_of_facets; ++ i_face) {
		stl_facet &face = stl->facet_start[i_face];
		for (int i_vertex = 0; i_vertex < 3; ++ i_vertex) {
			stl_vertex &v_dst = face.vertex[i_vertex];
			stl_vertex  v_src = v_dst;
			v_dst(0) = T(trafo3x4[0] * v_src(0) + trafo3x4[1] * v_src(1) + trafo3x4[2]  * v_src(2) + trafo3x4[3]);
			v_dst(1) = T(trafo3x4[4] * v_src(0) + trafo3x4[5] * v_src(1) + trafo3x4[6]  * v_src(2) + trafo3x4[7]);
			v_dst(2) = T(trafo3x4[8] * v_src(0) + trafo3x4[9] * v_src(1) + trafo3x4[10] * v_src(2) + trafo3x4[11]);
		}
        face.normal = (r * face.normal.template cast<T>()).template cast<float>().eval();
    }

	stl_get_size(stl);
}
*/

template<typename T>
inline void stl_transform(stl_file *stl, const Eigen::Transform<T, 3, Eigen::Affine, Eigen::DontAlign>& t)
{
    const Eigen::Matrix<T, 3, 3, Eigen::DontAlign> r = t.matrix().template block<3, 3>(0, 0).inverse().transpose();
    for (size_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		stl_facet &f = stl->facet_start[i];
		for (size_t j = 0; j < 3; ++j)
			f.vertex[j] = (t * f.vertex[j].template cast<T>()).template cast<float>().eval();
		f.normal = (r * f.normal.template cast<T>()).template cast<float>().eval();
	}

	stl_get_size(stl);
}

template<typename T>
inline void stl_transform(stl_file *stl, const Eigen::Matrix<T, 3, 3, Eigen::DontAlign>& m)
{
    const Eigen::Matrix<T, 3, 3, Eigen::DontAlign> r = m.inverse().transpose();
    for (size_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		stl_facet &f = stl->facet_start[i];
		for (size_t j = 0; j < 3; ++j)
			f.vertex[j] = (m * f.vertex[j].template cast<T>()).template cast<float>().eval();
        f.normal = (r * f.normal.template cast<T>()).template cast<float>().eval();
    }

	stl_get_size(stl);
}

template<typename V>
inline void its_translate(indexed_triangle_set &its, const V v)
{
  for (stl_vertex &v_dst : its.vertices)
    v_dst += v;
}

template<typename T>
inline void its_transform(indexed_triangle_set &its, T *trafo3x4)
{
	for (stl_vertex &v_dst : its.vertices) {
		stl_vertex  v_src = v_dst;
		v_dst(0) = T(trafo3x4[0] * v_src(0) + trafo3x4[1] * v_src(1) + trafo3x4[2]  * v_src(2) + trafo3x4[3]);
		v_dst(1) = T(trafo3x4[4] * v_src(0) + trafo3x4[5] * v_src(1) + trafo3x4[6]  * v_src(2) + trafo3x4[7]);
		v_dst(2) = T(trafo3x4[8] * v_src(0) + trafo3x4[9] * v_src(1) + trafo3x4[10] * v_src(2) + trafo3x4[11]);
	}
}

template<typename T>
inline void its_transform(indexed_triangle_set &its, const Eigen::Transform<T, 3, Eigen::Affine, Eigen::DontAlign>& t, bool fix_left_handed = false)
{
	//const Eigen::Matrix<double, 3, 3, Eigen::DontAlign> r = t.matrix().template block<3, 3>(0, 0);
	for (stl_vertex &v : its.vertices)
		v = (t * v.template cast<T>()).template cast<float>().eval();
  if (fix_left_handed && t.matrix().block(0, 0, 3, 3).determinant() < 0.)
    for (stl_triangle_vertex_indices &i : its.indices)
      std::swap(i[0], i[1]);
}

template<typename T>
inline void its_transform(indexed_triangle_set &its, const Eigen::Matrix<T, 3, 3, Eigen::DontAlign>& m, bool fix_left_handed = false)
{
  for (stl_vertex &v : its.vertices)
		v = (m * v.template cast<T>()).template cast<float>().eval();
  if (fix_left_handed && m.determinant() < 0.)
    for (stl_triangle_vertex_indices &i : its.indices)
      std::swap(i[0], i[1]);
}

extern void its_rotate_x(indexed_triangle_set &its, float angle);
extern void its_rotate_y(indexed_triangle_set &its, float angle);
extern void its_rotate_z(indexed_triangle_set &its, float angle);

extern void stl_generate_shared_vertices(stl_file *stl, indexed_triangle_set &its);
extern bool its_write_obj(const indexed_triangle_set &its, const char *file);
extern bool its_write_off(const indexed_triangle_set &its, const char *file);
extern bool its_write_vrml(const indexed_triangle_set &its, const char *file);

extern bool stl_write_dxf(stl_file *stl, const char *file, char *label);
inline void stl_calculate_normal(stl_normal &normal, stl_facet *facet) {
  normal = (facet->vertex[1] - facet->vertex[0]).cross(facet->vertex[2] - facet->vertex[0]);
}
inline void stl_normalize_vector(stl_normal &normal) {
  double length = normal.cast<double>().norm();
  if (length < 0.000000000001)
    normal = stl_normal::Zero();
  else
    normal *= float(1.0 / length);
}
extern void stl_calculate_volume(stl_file *stl);

extern void stl_repair(stl_file *stl, bool fixall_flag, bool exact_flag, bool tolerance_flag, float tolerance, bool increment_flag, float increment, bool nearby_flag, int iterations, bool remove_unconnected_flag, bool fill_holes_flag, bool normal_directions_flag, bool normal_values_flag, bool reverse_all_flag, bool verbose_flag);

extern void stl_allocate(stl_file *stl);
extern void stl_read(stl_file *stl, int first_facet, bool first, ImportstlProgressFn stlFn = nullptr);
extern void stl_facet_stats(stl_file *stl, stl_facet facet, bool &first);
extern void stl_reallocate(stl_file *stl);
extern void stl_add_facet(stl_file *stl, const stl_facet *new_facet);

// Validate the mesh, assert on error.
extern bool stl_validate(const stl_file *stl);
extern bool stl_validate(const stl_file *stl, const indexed_triangle_set &its);

#endif
