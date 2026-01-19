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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Boost pool: Don't use mutexes to synchronize memory allocation.
#define BOOST_POOL_NO_MT
#include <boost/pool/object_pool.hpp>

#include "stl.h"

static void reverse_facet(stl_file *stl, int facet_num)
{
	++ stl->stats.facets_reversed;

	int neighbor[3] = { stl->neighbors_start[facet_num].neighbor[0], stl->neighbors_start[facet_num].neighbor[1], stl->neighbors_start[facet_num].neighbor[2] };
	int vnot[3] = { stl->neighbors_start[facet_num].which_vertex_not[0], stl->neighbors_start[facet_num].which_vertex_not[1], stl->neighbors_start[facet_num].which_vertex_not[2] };

	// reverse the facet
	stl_vertex tmp_vertex = stl->facet_start[facet_num].vertex[0];
	stl->facet_start[facet_num].vertex[0] = stl->facet_start[facet_num].vertex[1];
	stl->facet_start[facet_num].vertex[1] = tmp_vertex;

	// fix the vnots of the neighboring facets
	if (neighbor[0] != -1)
		stl->neighbors_start[neighbor[0]].which_vertex_not[(vnot[0] + 1) % 3] = (stl->neighbors_start[neighbor[0]].which_vertex_not[(vnot[0] + 1) % 3] + 3) % 6;
	if (neighbor[1] != -1)
		stl->neighbors_start[neighbor[1]].which_vertex_not[(vnot[1] + 1) % 3] = (stl->neighbors_start[neighbor[1]].which_vertex_not[(vnot[1] + 1) % 3] + 4) % 6;
	if (neighbor[2] != -1)
		stl->neighbors_start[neighbor[2]].which_vertex_not[(vnot[2] + 1) % 3] = (stl->neighbors_start[neighbor[2]].which_vertex_not[(vnot[2] + 1) % 3] + 2) % 6;

	// swap the neighbors of the facet that is being reversed
	stl->neighbors_start[facet_num].neighbor[1] = neighbor[2];
	stl->neighbors_start[facet_num].neighbor[2] = neighbor[1];

	// swap the vnots of the facet that is being reversed 
	stl->neighbors_start[facet_num].which_vertex_not[1] = vnot[2];
	stl->neighbors_start[facet_num].which_vertex_not[2] = vnot[1];

	// reverse the values of the vnots of the facet that is being reversed
	stl->neighbors_start[facet_num].which_vertex_not[0] = (stl->neighbors_start[facet_num].which_vertex_not[0] + 3) % 6;
	stl->neighbors_start[facet_num].which_vertex_not[1] = (stl->neighbors_start[facet_num].which_vertex_not[1] + 3) % 6;
	stl->neighbors_start[facet_num].which_vertex_not[2] = (stl->neighbors_start[facet_num].which_vertex_not[2] + 3) % 6;
}

// Returns true if the normal was flipped.
static bool check_normal_vector(stl_file *stl, int facet_num, int normal_fix_flag)
{
	stl_facet *facet = &stl->facet_start[facet_num];

	stl_normal normal;
	stl_calculate_normal(normal, facet);
	stl_normalize_vector(normal);
	stl_normal normal_dif = (normal - facet->normal).cwiseAbs();

	const float eps = 0.001f;
	if (normal_dif(0) < eps && normal_dif(1) < eps && normal_dif(2) < eps) {
		// Normal is within tolerance. It is not really necessary to change the values here, but just for consistency, I will.
		facet->normal = normal;
		return false;
	}

	stl_normal test_norm = facet->normal;
	stl_normalize_vector(test_norm);
	normal_dif = (normal - test_norm).cwiseAbs();
	if (normal_dif(0) < eps && normal_dif(1) < eps && normal_dif(2) < eps) {
		// The normal is not within tolerance, but direction is OK.
		if (normal_fix_flag) {
	  		facet->normal = normal;
	  		++ stl->stats.normals_fixed;
		}
		return false;
	}

	test_norm *= -1.f;
	normal_dif = (normal - test_norm).cwiseAbs();
	if (normal_dif(0) < eps && normal_dif(1) < eps && normal_dif(2) < eps) {
		// The normal is not within tolerance and backwards.
		if (normal_fix_flag) {
	  		facet->normal = normal;
	  		++ stl->stats.normals_fixed;
		}
		return true;
	}
	if (normal_fix_flag) {
		facet->normal = normal;
		++ stl->stats.normals_fixed;
	}
	// Status is unknown.
	return false;
}

void stl_fix_normal_directions(stl_file *stl)
{
 	// This may happen for malformed models
  	if (stl->stats.number_of_facets == 0)
  		return;

	struct stl_normal {
    	int         facet_num;
    	stl_normal *next;
  	};

  	// Initialize linked list.
  	boost::object_pool<stl_normal> pool;
   	stl_normal *head = pool.construct();
  	stl_normal *tail = pool.construct();
	head->next = tail;
	tail->next = tail;

	// Initialize list that keeps track of already fixed facets.
	std::vector<char> norm_sw(stl->stats.number_of_facets, 0);
	// Initialize list that keeps track of reversed facets.
    std::vector<int>  reversed_ids;
    reversed_ids.reserve(stl->stats.number_of_facets);

  	int facet_num = 0;
  	// If normal vector is not within tolerance and backwards:
    // Arbitrarily starts at face 0.  If this one is wrong, we're screwed. Thankfully, the chances
    // of it being wrong randomly are low if most of the triangles are right:
  	if (check_normal_vector(stl, 0, 0)) {
    	reverse_facet(stl, 0);
      	reversed_ids.emplace_back(0);
  	}

  	// Say that we've fixed this facet:
  	norm_sw[facet_num] = 1;
	int checked = 1;

  	for (;;) {
    	// Add neighbors_to_list. Add unconnected neighbors to the list.
    	bool force_exit = false;
    	for (int j = 0; j < 3; ++ j) {
      		// Reverse the neighboring facets if necessary.
      		if (stl->neighbors_start[facet_num].which_vertex_not[j] > 2) {
        		// If the facet has a neighbor that is -1, it means that edge isn't shared by another facet
        		if (stl->neighbors_start[facet_num].neighbor[j] != -1) {
            		if (norm_sw[stl->neighbors_start[facet_num].neighbor[j]] == 1) {
                		// trying to modify a facet already marked as fixed, revert all changes made until now and exit (fixes: #716, #574, #413, #269, #262, #259, #230, #228, #206)
                		for (int id = int(reversed_ids.size()) - 1; id >= 0; -- id)
                    		reverse_facet(stl, reversed_ids[id]);
                		force_exit = true;
                		break;
            		}
            		reverse_facet(stl, stl->neighbors_start[facet_num].neighbor[j]);
            		reversed_ids.emplace_back(stl->neighbors_start[facet_num].neighbor[j]);
        		}
      		}
      		// If this edge of the facet is connected:
      		if (stl->neighbors_start[facet_num].neighbor[j] != -1) {
        		// If we haven't fixed this facet yet, add it to the list:
        		if (norm_sw[stl->neighbors_start[facet_num].neighbor[j]] != 1) {
	          		// Add node to beginning of list.
	          		stl_normal *newn = pool.construct();
	          		newn->facet_num = stl->neighbors_start[facet_num].neighbor[j];
	          		newn->next = head->next;
	          		head->next = newn;
	        	}
	      	}
	    }

    	// an error occourred, quit the for loop and exit
    	if (force_exit)
    		break;

    	// Get next facet to fix from top of list.
    	if (head->next != tail) {
      		facet_num = head->next->facet_num;
            assert(facet_num < stl->stats.number_of_facets);
      		if (norm_sw[facet_num] != 1) { // If facet is in list multiple times
        		norm_sw[facet_num] = 1; // Record this one as being fixed.
        		++ checked;
      		}
      		stl_normal *temp = head->next;	// Delete this facet from the list.
      		head->next = head->next->next;
      		// pool.destroy(temp);
    	} else { // If we ran out of facets to fix: All of the facets in this part have been fixed.
      		++ stl->stats.number_of_parts;
      		if (checked >= int(stl->stats.number_of_facets))
        		// All of the facets have been checked.  Bail out.
        		break;
    		// There is another part here.  Find it and continue.
    		for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
      			if (norm_sw[i] == 0) {
        			// This is the first facet of the next part.
        			facet_num = i;
        			if (check_normal_vector(stl, i, 0)) {
            			reverse_facet(stl, i);
            			reversed_ids.emplace_back(i);
        			}
        			norm_sw[facet_num] = 1;
        			++ checked;
        			break;
      			}
    	}
  	}

	// pool.destroy(head);
	// pool.destroy(tail);
}

void stl_fix_normal_values(stl_file *stl)
{
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i)
    	check_normal_vector(stl, i, 1);
}

void stl_reverse_all_facets(stl_file *stl)
{
	stl_normal normal;
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
    	reverse_facet(stl, i);
    	stl_calculate_normal(normal, &stl->facet_start[i]);
    	stl_normalize_vector(normal);
    	stl->facet_start[i].normal = normal;
  	}
}
