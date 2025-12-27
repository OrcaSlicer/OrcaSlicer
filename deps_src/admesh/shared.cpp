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

#include <stdlib.h>
#include <string.h>

#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include "stl.h"

#include "libslic3r/LocalesUtils.hpp"

void stl_generate_shared_vertices(stl_file *stl, indexed_triangle_set &its)
{
	// 3 indices to vertex per face
	its.indices.assign(stl->stats.number_of_facets, stl_triangle_vertex_indices(-1, -1, -1));
	// Shared vertices (3D coordinates)
	its.vertices.clear();
	its.vertices.reserve(stl->stats.number_of_facets / 2);

	// A degenerate mesh may contain loops: Traversing a fan will end up in an endless loop
	// while never reaching the starting face. To avoid these endless loops, traversed faces at each fan traversal
	// are marked with a unique fan_traversal_stamp.
	unsigned int			  fan_traversal_stamp = 0;
	std::vector<unsigned int> fan_traversal_facet_visited(stl->stats.number_of_facets, 0);

	for (uint32_t facet_idx = 0; facet_idx < stl->stats.number_of_facets; ++ facet_idx) {
		for (int j = 0; j < 3; ++ j) {
			if (its.indices[facet_idx][j] != -1)
				// Shared vertex was already assigned.
				continue;
			// Create a new shared vertex.
			its.vertices.emplace_back(stl->facet_start[facet_idx].vertex[j]);
			// Traverse the fan around the j-th vertex of the i-th face, assign the newly created shared vertex index to all the neighboring triangles in the triangle fan.
			int  facet_in_fan_idx 	= facet_idx;
			bool edge_direction 	= false;
			bool traversal_reversed = false;
			int  vnot      			= (j + 2) % 3;
			// Increase the 
			++ fan_traversal_stamp;
			for (;;) {
				// Next edge on facet_in_fan_idx to be traversed. The edge is indexed by its starting vertex index.
				int next_edge    = 0;
				// Vertex index in facet_in_fan_idx, which is being pivoted around, and which is being assigned a new shared vertex.
				int pivot_vertex = 0;
				if (vnot > 2) {
					// The edge of facet_in_fan_idx opposite to vnot is equally oriented, therefore
					// the neighboring facet is flipped.
			  		if (! edge_direction) {
			    		pivot_vertex = (vnot + 2) % 3;
			    		next_edge    = pivot_vertex;			    		
			  		} else {
			    		pivot_vertex = (vnot + 1) % 3;
			    		next_edge    = vnot % 3;
			  		}
			  		edge_direction = ! edge_direction;
				} else {
					// The neighboring facet is correctly oriented.
			  		if (! edge_direction) {
			    		pivot_vertex = (vnot + 1) % 3;
			    		next_edge    = vnot;
			  		} else {
			    		pivot_vertex = (vnot + 2) % 3;
			    		next_edge    = pivot_vertex;
			  		}
				}
				its.indices[facet_in_fan_idx][pivot_vertex] = its.vertices.size() - 1;
				fan_traversal_facet_visited[facet_in_fan_idx] = fan_traversal_stamp;

				// next_edge is an index of the starting vertex of the edge, not an index of the opposite vertex to the edge!
				int next_facet = stl->neighbors_start[facet_in_fan_idx].neighbor[next_edge];
				if (next_facet == -1) {
					// No neighbor going in the current direction.
					if (traversal_reversed) {
						// Went to one limit, then turned back and reached the other limit. Quit the fan traversal.
					    break;
					} else {
						// Reached the first limit. Now try to reverse and traverse up to the other limit.
					    edge_direction        = true;
					    vnot 	         	  = (j + 1) % 3;
					    traversal_reversed    = true;
				    	facet_in_fan_idx      = facet_idx;
					}
				} else if (next_facet == facet_idx) {
					// Traversed a closed fan all around.
//					assert(! traversal_reversed);
					break;
				} else if (next_facet >= (int)stl->stats.number_of_facets) {
					// The mesh is not valid!
					// assert(false);
					break;
				} else if (fan_traversal_facet_visited[next_facet] == fan_traversal_stamp) {
					// Traversed a closed fan all around, but did not reach the starting face.
					// This indicates an invalid geometry (non-manifold).
					//assert(false);
					break;
				} else {
					// Continue traversal.
					// next_edge is an index of the starting vertex of the edge, not an index of the opposite vertex to the edge!
					vnot = stl->neighbors_start[facet_in_fan_idx].which_vertex_not[next_edge];
					facet_in_fan_idx = next_facet;
				}
			}
		}
	}
}

bool its_write_off(const indexed_triangle_set &its, const char *file)
{
    Slic3r::CNumericLocalesSetter locales_setter;
	/* Open the file */
	FILE *fp = boost::nowide::fopen(file, "w");
	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_ascii: Couldn't open " << file << " for writing";
		return false;
	}

	fprintf(fp, "OFF\n");
	fprintf(fp, "%d %d 0\n", (int)its.vertices.size(), (int)its.indices.size());
	for (int i = 0; i < its.vertices.size(); ++ i)
		fprintf(fp, "\t%f %f %f\n", its.vertices[i](0), its.vertices[i](1), its.vertices[i](2));
	for (uint32_t i = 0; i < its.indices.size(); ++ i)
		fprintf(fp, "\t3 %d %d %d\n", its.indices[i][0], its.indices[i][1], its.indices[i][2]);
	fclose(fp);
	return true;
}

bool its_write_vrml(const indexed_triangle_set &its, const char *file)
{
    Slic3r::CNumericLocalesSetter locales_setter;
	/* Open the file */
  	FILE *fp = boost::nowide::fopen(file, "w");
	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_vrml: Couldn't open " << file << " for writing";
		return false;
	}

	fprintf(fp, "#VRML V1.0 ascii\n\n");
	fprintf(fp, "Separator {\n");
	fprintf(fp, "\tDEF STLShape ShapeHints {\n");
	fprintf(fp, "\t\tvertexOrdering COUNTERCLOCKWISE\n");
	fprintf(fp, "\t\tfaceType CONVEX\n");
	fprintf(fp, "\t\tshapeType SOLID\n");
	fprintf(fp, "\t\tcreaseAngle 0.0\n");
	fprintf(fp, "\t}\n");
	fprintf(fp, "\tDEF STLModel Separator {\n");
	fprintf(fp, "\t\tDEF STLColor Material {\n");
	fprintf(fp, "\t\t\temissiveColor 0.700000 0.700000 0.000000\n");
	fprintf(fp, "\t\t}\n");
	fprintf(fp, "\t\tDEF STLVertices Coordinate3 {\n");
	fprintf(fp, "\t\t\tpoint [\n");

	int i = 0;
	for (; i + 1 < its.vertices.size(); ++ i)
		fprintf(fp, "\t\t\t\t%f %f %f,\n", its.vertices[i](0), its.vertices[i](1), its.vertices[i](2));
	fprintf(fp, "\t\t\t\t%f %f %f]\n", its.vertices[i](0), its.vertices[i](1), its.vertices[i](2));
	fprintf(fp, "\t\t}\n");
	fprintf(fp, "\t\tDEF STLTriangles IndexedFaceSet {\n");
	fprintf(fp, "\t\t\tcoordIndex [\n");

	for (size_t i = 0; i + 1 < its.indices.size(); ++ i)
		fprintf(fp, "\t\t\t\t%d, %d, %d, -1,\n", its.indices[i][0], its.indices[i][1], its.indices[i][2]);
	fprintf(fp, "\t\t\t\t%d, %d, %d, -1]\n", its.indices[i][0], its.indices[i][1], its.indices[i][2]);
	fprintf(fp, "\t\t}\n");
	fprintf(fp, "\t}\n");
	fprintf(fp, "}\n");
	fclose(fp);
	return true;
}

bool its_write_obj(const indexed_triangle_set &its, const char *file)
{
    Slic3r::CNumericLocalesSetter locales_setter;
  	FILE *fp = boost::nowide::fopen(file, "w");
  	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_obj: Couldn't open " << file << " for writing";
    	return false;
  	}

	for (size_t i = 0; i < its.vertices.size(); ++ i)
    	fprintf(fp, "v %f %f %f\n", its.vertices[i](0), its.vertices[i](1), its.vertices[i](2));
  	for (size_t i = 0; i < its.indices.size(); ++ i)
    	fprintf(fp, "f %d %d %d\n", its.indices[i][0]+1, its.indices[i][1]+1, its.indices[i][2]+1);
  	fclose(fp);
  	return true;
}


// Check validity of the mesh, assert on error.
bool stl_validate(const stl_file *stl, const indexed_triangle_set &its)
{
	assert(! stl->facet_start.empty());
	assert(stl->facet_start.size() == stl->stats.number_of_facets);
	assert(stl->neighbors_start.size() == stl->stats.number_of_facets);
	assert(stl->facet_start.size() == stl->neighbors_start.size());
	assert(! stl->neighbors_start.empty());
	assert((its.indices.empty()) == (its.vertices.empty()));
	assert(stl->stats.number_of_facets > 0);
	assert(its.vertices.empty() || its.indices.size() == stl->stats.number_of_facets);

#ifdef _DEBUG
    // Verify validity of neighborship data.
    for (int facet_idx = 0; facet_idx < (int)stl->stats.number_of_facets; ++ facet_idx) {
        const stl_neighbors &nbr 		= stl->neighbors_start[facet_idx];
        const int 			*vertices 	= its.indices.empty() ? nullptr : its.indices[facet_idx].data();
        for (int nbr_idx = 0; nbr_idx < 3; ++ nbr_idx) {
            int nbr_face = stl->neighbors_start[facet_idx].neighbor[nbr_idx];
            assert(nbr_face < (int)stl->stats.number_of_facets);
            if (nbr_face != -1) {
            	int nbr_vnot = nbr.which_vertex_not[nbr_idx];
				assert(nbr_vnot >= 0 && nbr_vnot < 6);
				// Neighbor of the neighbor is the original face.
				assert(stl->neighbors_start[nbr_face].neighbor[(nbr_vnot + 1) % 3] == facet_idx);
				int vnot_back = stl->neighbors_start[nbr_face].which_vertex_not[(nbr_vnot + 1) % 3];
				assert(vnot_back >= 0 && vnot_back < 6);
				assert((nbr_vnot < 3) == (vnot_back < 3));
				assert(vnot_back % 3 == (nbr_idx + 2) % 3);
				if (vertices != nullptr) {
					// Has shared vertices.
	            	if (nbr_vnot < 3) {
	            		// Faces facet_idx and nbr_face share two vertices accross the common edge. Faces are correctly oriented.
						assert((its.indices[nbr_face][(nbr_vnot + 1) % 3] == vertices[(nbr_idx + 1) % 3] && its.indices[nbr_face][(nbr_vnot + 2) % 3] == vertices[nbr_idx]));
					} else {
	            		// Faces facet_idx and nbr_face share two vertices accross the common edge. Faces are incorrectly oriented, one of them is flipped.
						assert((its.indices[nbr_face][(nbr_vnot + 2) % 3] == vertices[(nbr_idx + 1) % 3] && its.indices[nbr_face][(nbr_vnot + 1) % 3] == vertices[nbr_idx]));
					}
				}
            }
        }
    }
#endif /* _DEBUG */

	return true;
}

// Check validity of the mesh, assert on error.
bool stl_validate(const stl_file *stl)
{
	indexed_triangle_set its;
	return stl_validate(stl, its);
}
