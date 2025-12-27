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

#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/predef/other/endian.h>

#include "stl.h"

void stl_stats_out(stl_file *stl, FILE *file, char *input_file)
{
  	// This is here for Slic3r, without our config.h it won't use this part of the code anyway.
#ifndef VERSION
#define VERSION "unknown"
#endif
  	fprintf(file, "\n================= Results produced by ADMesh version " VERSION " ================\n");
  	fprintf(file, "Input file         : %s\n", input_file);
  	if (stl->stats.type == binary)
    	fprintf(file, "File type          : Binary STL file\n");
  	else
    	fprintf(file, "File type          : ASCII STL file\n");
  	fprintf(file, "Header             : %s\n", stl->stats.header.data());
  	fprintf(file, "============== Size ==============\n");
  	fprintf(file, "Min X = % f, Max X = % f\n", stl->stats.min(0), stl->stats.max(0));
  	fprintf(file, "Min Y = % f, Max Y = % f\n", stl->stats.min(1), stl->stats.max(1));
  	fprintf(file, "Min Z = % f, Max Z = % f\n", stl->stats.min(2), stl->stats.max(2));
  	fprintf(file, "========= Facet Status ========== Original ============ Final ====\n");
  	fprintf(file, "Number of facets                 : %5d               %5d\n", stl->stats.original_num_facets, stl->stats.number_of_facets);
  	fprintf(file, "Facets with 1 disconnected edge  : %5d               %5d\n", 
  		stl->stats.facets_w_1_bad_edge, stl->stats.connected_facets_2_edge - stl->stats.connected_facets_3_edge);
  	fprintf(file, "Facets with 2 disconnected edges : %5d               %5d\n",
    	stl->stats.facets_w_2_bad_edge, stl->stats.connected_facets_1_edge - stl->stats.connected_facets_2_edge);
  	fprintf(file, "Facets with 3 disconnected edges : %5d               %5d\n",
        stl->stats.facets_w_3_bad_edge, stl->stats.number_of_facets - stl->stats.connected_facets_1_edge);
  	fprintf(file, "Total disconnected facets        : %5d               %5d\n",
		stl->stats.facets_w_1_bad_edge + stl->stats.facets_w_2_bad_edge + stl->stats.facets_w_3_bad_edge, stl->stats.number_of_facets - stl->stats.connected_facets_3_edge);
  	fprintf(file, "=== Processing Statistics ===     ===== Other Statistics =====\n");
  	fprintf(file, "Number of parts       : %5d        Volume   : %f\n", stl->stats.number_of_parts, stl->stats.volume);
  	fprintf(file, "Degenerate facets     : %5d\n", stl->stats.degenerate_facets);
  	fprintf(file, "Edges fixed           : %5d\n", stl->stats.edges_fixed);
  	fprintf(file, "Facets removed        : %5d\n", stl->stats.facets_removed);
  	fprintf(file, "Facets added          : %5d\n", stl->stats.facets_added);
  	fprintf(file, "Facets reversed       : %5d\n", stl->stats.facets_reversed);
  	fprintf(file, "Backwards edges       : %5d\n", stl->stats.backwards_edges);
  	fprintf(file, "Normals fixed         : %5d\n", stl->stats.normals_fixed);
}

bool stl_write_ascii(stl_file *stl, const char *file, const char *label)
{
	FILE *fp = boost::nowide::fopen(file, "w");
  	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_ascii: Couldn't open " << file << " for writing";
    	return false;
  	}

	fprintf(fp, "solid  %s\n", label);

	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		fprintf(fp, "  facet normal % .8E % .8E % .8E\n", stl->facet_start[i].normal(0), stl->facet_start[i].normal(1), stl->facet_start[i].normal(2));
		fprintf(fp, "    outer loop\n");
		fprintf(fp, "      vertex % .8E % .8E % .8E\n", stl->facet_start[i].vertex[0](0), stl->facet_start[i].vertex[0](1), stl->facet_start[i].vertex[0](2));
		fprintf(fp, "      vertex % .8E % .8E % .8E\n", stl->facet_start[i].vertex[1](0), stl->facet_start[i].vertex[1](1), stl->facet_start[i].vertex[1](2));
		fprintf(fp, "      vertex % .8E % .8E % .8E\n", stl->facet_start[i].vertex[2](0), stl->facet_start[i].vertex[2](1), stl->facet_start[i].vertex[2](2));
		fprintf(fp, "    endloop\n");
		fprintf(fp, "  endfacet\n");
	}

  	fprintf(fp, "endsolid  %s\n", label);
  	fclose(fp);
  	return true;
}

bool stl_print_neighbors(stl_file *stl, char *file)
{
	FILE *fp = boost::nowide::fopen(file, "w");
	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_print_neighbors: Couldn't open " << file << " for writing";
    	return false;
  	}

  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
    	fprintf(fp, "%d, %d,%d, %d,%d, %d,%d\n",
            i,
            stl->neighbors_start[i].neighbor[0],
            (int)stl->neighbors_start[i].which_vertex_not[0],
            stl->neighbors_start[i].neighbor[1],
            (int)stl->neighbors_start[i].which_vertex_not[1],
            stl->neighbors_start[i].neighbor[2],
            (int)stl->neighbors_start[i].which_vertex_not[2]);
  	}
  	fclose(fp);
  	return true;
}

#if BOOST_ENDIAN_BIG_BYTE
// Swap a buffer of 32bit data from little endian to big endian and vice versa.
void stl_internal_reverse_quads(char *buf, size_t cnt)
{
	for (size_t i = 0; i < cnt; i += 4) {
		std::swap(buf[i], buf[i+3]);
		std::swap(buf[i+1], buf[i+2]);
	}
}
#endif

bool stl_write_binary(stl_file *stl, const char *file, const char *label)
{
	FILE *fp = boost::nowide::fopen(file, "wb");
	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_binary: Couldn't open " << file << " for writing";
    	return false;
  	}

	fprintf(fp, "%s", label);
	for (size_t i = strlen(label); i < LABEL_SIZE; ++ i)
		putc(0, fp);

#if !defined(SEEK_SET)
	#define SEEK_SET 0
#endif
	fseek(fp, LABEL_SIZE, SEEK_SET);
#if BOOST_ENDIAN_LITTLE_BYTE
	fwrite(&stl->stats.number_of_facets, 4, 1, fp);
	for (const stl_facet &facet : stl->facet_start)
	  	fwrite(&facet, SIZEOF_STL_FACET, 1, fp);
#else /* BOOST_ENDIAN_LITTLE_BYTE */
	char buffer[50];
	// Convert the number of facets to little endian.
	memcpy(buffer, &stl->stats.number_of_facets, 4);
	stl_internal_reverse_quads(buffer, 4);
	fwrite(buffer, 4, 1, fp);
	for (const stl_facet &facet : stl->facet_start) {
		memcpy(buffer, &facet, 50);
		// Convert to little endian.
		stl_internal_reverse_quads(buffer, 48);
		fwrite(buffer, SIZEOF_STL_FACET, 1, fp);
	}
#endif /* BOOST_ENDIAN_LITTLE_BYTE */
	fclose(fp);
	return true;
}

void stl_write_vertex(stl_file *stl, int facet, int vertex)
{
  	printf("  vertex %d/%d % .8E % .8E % .8E\n", vertex, facet,
         stl->facet_start[facet].vertex[vertex](0),
         stl->facet_start[facet].vertex[vertex](1),
         stl->facet_start[facet].vertex[vertex](2));
}

void stl_write_facet(stl_file *stl, char *label, int facet)
{
	printf("facet (%d)/ %s\n", facet, label);
	stl_write_vertex(stl, facet, 0);
	stl_write_vertex(stl, facet, 1);
	stl_write_vertex(stl, facet, 2);
}

void stl_write_neighbor(stl_file *stl, int facet)
{
	printf("Neighbors %d: %d, %d, %d ;  %d, %d, %d\n", facet,
		stl->neighbors_start[facet].neighbor[0],
		stl->neighbors_start[facet].neighbor[1],
		stl->neighbors_start[facet].neighbor[2],
		stl->neighbors_start[facet].which_vertex_not[0],
		stl->neighbors_start[facet].which_vertex_not[1],
		stl->neighbors_start[facet].which_vertex_not[2]);
}

bool stl_write_quad_object(stl_file *stl, char *file)
{
	stl_vertex connect_color = stl_vertex::Zero();
	stl_vertex uncon_1_color = stl_vertex::Zero();
	stl_vertex uncon_2_color = stl_vertex::Zero();
	stl_vertex uncon_3_color = stl_vertex::Zero();
	stl_vertex color;

	FILE *fp = boost::nowide::fopen(file, "w");
	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_quad_object: Couldn't open " << file << " for writing";
		return false;
	}

  	fprintf(fp, "CQUAD\n");
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
  		switch (stl->neighbors_start[i].num_neighbors()) {
    		case 0:
    		default: color = uncon_3_color; break;
    		case 1:  color = uncon_2_color; break;
    		case 2:  color = uncon_1_color; break;
  		case 3:  color = connect_color; break;
	    }
	    fprintf(fp, "%f %f %f    %1.1f %1.1f %1.1f 1\n", stl->facet_start[i].vertex[0](0), stl->facet_start[i].vertex[0](1), stl->facet_start[i].vertex[0](2), color(0), color(1), color(2));
    	fprintf(fp, "%f %f %f    %1.1f %1.1f %1.1f 1\n", stl->facet_start[i].vertex[1](0), stl->facet_start[i].vertex[1](1), stl->facet_start[i].vertex[1](2), color(0), color(1), color(2));
    	fprintf(fp, "%f %f %f    %1.1f %1.1f %1.1f 1\n", stl->facet_start[i].vertex[2](0), stl->facet_start[i].vertex[2](1), stl->facet_start[i].vertex[2](2), color(0), color(1), color(2));
    	fprintf(fp, "%f %f %f    %1.1f %1.1f %1.1f 1\n", stl->facet_start[i].vertex[2](0), stl->facet_start[i].vertex[2](1), stl->facet_start[i].vertex[2](2), color(0), color(1), color(2));
  }
  fclose(fp);
  return true;
}

bool stl_write_dxf(stl_file *stl, const char *file, char *label) 
{
	FILE *fp = boost::nowide::fopen(file, "w");
	if (fp == nullptr) {
		BOOST_LOG_TRIVIAL(error) << "stl_write_quad_object: Couldn't open " << file << " for writing";
    	return false;
  	}

	fprintf(fp, "999\n%s\n", label);
	fprintf(fp, "0\nSECTION\n2\nHEADER\n0\nENDSEC\n");
	fprintf(fp, "0\nSECTION\n2\nTABLES\n0\nTABLE\n2\nLAYER\n70\n1\n\
	0\nLAYER\n2\n0\n70\n0\n62\n7\n6\nCONTINUOUS\n0\nENDTAB\n0\nENDSEC\n");
	fprintf(fp, "0\nSECTION\n2\nBLOCKS\n0\nENDSEC\n");

	fprintf(fp, "0\nSECTION\n2\nENTITIES\n");

	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		fprintf(fp, "0\n3DFACE\n8\n0\n");
		fprintf(fp, "10\n%f\n20\n%f\n30\n%f\n", stl->facet_start[i].vertex[0](0), stl->facet_start[i].vertex[0](1), stl->facet_start[i].vertex[0](2));
		fprintf(fp, "11\n%f\n21\n%f\n31\n%f\n", stl->facet_start[i].vertex[1](0), stl->facet_start[i].vertex[1](1), stl->facet_start[i].vertex[1](2));
		fprintf(fp, "12\n%f\n22\n%f\n32\n%f\n", stl->facet_start[i].vertex[2](0), stl->facet_start[i].vertex[2](1), stl->facet_start[i].vertex[2](2));
		fprintf(fp, "13\n%f\n23\n%f\n33\n%f\n", stl->facet_start[i].vertex[2](0), stl->facet_start[i].vertex[2](1), stl->facet_start[i].vertex[2](2));
	}

  	fprintf(fp, "0\nENDSEC\n0\nEOF\n");
  	fclose(fp);
  	return true;
}
