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

#include <algorithm>
#include <vector>

#include <boost/predef/other/endian.h>
#include <boost/log/trivial.hpp>
// Boost pool: Don't use mutexes to synchronize memory allocation.
#define BOOST_POOL_NO_MT
#include <boost/pool/object_pool.hpp>

#include "stl.h"

struct HashEdge {
	// Key of a hash edge: sorted vertices of the edge.
	uint32_t       key[6];
	// Compare two keys.
	bool operator==(const HashEdge &rhs) const { return memcmp(key, rhs.key, sizeof(key)) == 0; }
	bool operator!=(const HashEdge &rhs) const { return ! (*this == rhs); }
	int  hash(int M) const { return ((key[0] / 11 + key[1] / 7 + key[2] / 3) ^ (key[3] / 11  + key[4] / 7 + key[5] / 3)) % M; }

	// Index of a facet owning this edge.
	int        facet_number;
	// Index of this edge inside the facet with an index of facet_number.
	// If this edge is stored backwards, which_edge is increased by 3.
	int        which_edge;
	HashEdge  *next;

	void load_exact(stl_file *stl, const stl_vertex *a, const stl_vertex *b)
	{
		{
	    	stl_vertex diff = (*a - *b).cwiseAbs();
	    	float max_diff = std::max(diff(0), std::max(diff(1), diff(2)));
	    	stl->stats.shortest_edge = std::min(max_diff, stl->stats.shortest_edge);
	  	}

	  	// Ensure identical vertex ordering of equal edges.
	  	// This method is numerically robust.
	  	if (vertex_lower(*a, *b)) {
	  	} else {
	  		// This edge is loaded backwards.
		    std::swap(a, b);
		    this->which_edge += 3;
	  	}
	  	memcpy(&this->key[0], a->data(), sizeof(stl_vertex));
	  	memcpy(&this->key[3], b->data(), sizeof(stl_vertex));
	  	// Switch negative zeros to positive zeros, so memcmp will consider them to be equal.
	  	for (size_t i = 0; i < 6; ++ i) {
	    	unsigned char *p = (unsigned char*)(this->key + i);
	#if BOOST_ENDIAN_LITTLE_BYTE
	    	if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0x80)
	      		// Negative zero, switch to positive zero.
	      		p[3] = 0;
	#else /* BOOST_ENDIAN_LITTLE_BYTE */
	    	if (p[0] == 0x80 && p[1] == 0 && p[2] == 0 && p[3] == 0)
	      		// Negative zero, switch to positive zero.
	      		p[0] = 0;
	#endif /* BOOST_ENDIAN_LITTLE_BYTE */
	  	}
	}

	bool load_nearby(const stl_file *stl, const stl_vertex &a, const stl_vertex &b, float tolerance)
	{
		// Index of a grid cell spaced by tolerance.
		typedef Eigen::Matrix<int32_t,  3, 1, Eigen::DontAlign> Vec3i32;
		Vec3i32 vertex1 = ((a - stl->stats.min) / tolerance).cast<int32_t>();
		Vec3i32 vertex2 = ((b - stl->stats.min) / tolerance).cast<int32_t>();
		static_assert(sizeof(Vec3i32) == 12, "size of Vec3i32 incorrect");

		if (vertex1 == vertex2)
			// Both vertices hash to the same value
			return false;

		// Ensure identical vertex ordering of edges, which vertices land into equal grid cells.
		// This method is numerically robust.
		if ((vertex1[0] != vertex2[0]) ? 
		    (vertex1[0] < vertex2[0]) : 
		    ((vertex1[1] != vertex2[1]) ? 
		        (vertex1[1] < vertex2[1]) : 
		        (vertex1[2] < vertex2[2]))) {
			memcpy(&this->key[0], vertex1.data(), sizeof(stl_vertex));
			memcpy(&this->key[3], vertex2.data(), sizeof(stl_vertex));
		} else {
			memcpy(&this->key[0], vertex2.data(), sizeof(stl_vertex));
			memcpy(&this->key[3], vertex1.data(), sizeof(stl_vertex));
			this->which_edge += 3; /* this edge is loaded backwards */
		}
		return true;
	}

private:
	inline bool vertex_lower(const stl_vertex &a, const stl_vertex &b) {
	  	return (a(0) != b(0)) ? (a(0) < b(0)) :
	           ((a(1) != b(1)) ? (a(1) < b(1)) : (a(2) < b(2)));
	}
};

struct HashTableEdges {
	HashTableEdges(size_t number_of_faces) {
		this->M = (int)hash_size_from_nr_faces(number_of_faces);
		this->heads.assign(this->M, nullptr);
		this->tail = pool.construct();
		this->tail->next = this->tail;
		for (int i = 0; i < this->M; ++ i)
			this->heads[i] = this->tail;
	}
	~HashTableEdges() {
#ifndef NDEBUG
		for (int i = 0; i < this->M; ++ i)
	    	for (HashEdge *temp = this->heads[i]; temp != this->tail; temp = temp->next)
	        	++ this->freed;
		this->tail = nullptr;
#endif /* NDEBUG */
	}

	void insert_edge_exact(stl_file *stl, const HashEdge &edge)
	{
		this->insert_edge(stl, edge, [stl](const HashEdge& edge1, const HashEdge& edge2) { record_neighbors(stl, edge1, edge2); });
	}

	void insert_edge_nearby(stl_file *stl, const HashEdge &edge)
	{
		this->insert_edge(stl, edge, [stl](const HashEdge& edge1, const HashEdge& edge2) { match_neighbors_nearby(stl, edge1, edge2); });
	}

	// Hash table on edges
	std::vector<HashEdge*> 	heads;
	HashEdge* 				tail;
	int           			M;
	boost::object_pool<HashEdge> pool;

#ifndef NDEBUG
	size_t 					malloced   	= 0;
	size_t 					freed 	  	= 0;
	size_t 					collisions 	= 0;
#endif /* NDEBUG */

private:
	static inline size_t hash_size_from_nr_faces(const size_t nr_faces)
	{
		// Good primes for addressing a cca. 30 bit space.
		// https://planetmath.org/goodhashtableprimes
		static std::vector<uint32_t> primes{ 98317, 196613, 393241, 786433, 1572869, 3145739, 6291469, 12582917, 25165843, 50331653, 100663319, 201326611, 402653189, 805306457, 1610612741 };
		// Find a prime number for 50% filling of the shared triangle edges in the mesh.
		auto it = std::upper_bound(primes.begin(), primes.end(), nr_faces * 3 * 2 - 1);
		return (it == primes.end()) ? primes.back() : *it;
	}


	// MatchNeighbors(stl_file *stl, const HashEdge &edge_a, const HashEdge &edge_b)
	template<typename MatchNeighbors>
	void insert_edge(stl_file *stl, const HashEdge &edge, MatchNeighbors match_neighbors)
	{
		int       chain_number = edge.hash(this->M);
		HashEdge *link         = this->heads[chain_number];
		if (link == this->tail) {
			// This list doesn't have any edges currently in it.  Add this one.
			HashEdge *new_edge = pool.construct(edge);
#ifndef NDEBUG
			++ this->malloced;
#endif /* NDEBUG */
			new_edge->next = this->tail;
			this->heads[chain_number] = new_edge;
		} else if (edges_equal(edge, *link)) {
			// This is a match.  Record result in neighbors list.
			match_neighbors(edge, *link);
			// Delete the matched edge from the list.
			this->heads[chain_number] = link->next;
			// pool.destroy(link);
#ifndef NDEBUG
			++ this->freed;
#endif /* NDEBUG */
		} else {
			// Continue through the rest of the list.
			for (;;) {
				if (link->next == this->tail) {
					// This is the last item in the list. Insert a new edge.
					HashEdge *new_edge = pool.construct();
#ifndef NDEBUG
					++ this->malloced;
#endif /* NDEBUG */
					*new_edge = edge;
					new_edge->next = this->tail;
					link->next = new_edge;
#ifndef NDEBUG
					++ this->collisions;
#endif /* NDEBUG */
					break;
				}
				if (edges_equal(edge, *link->next)) {
					// This is a match.  Record result in neighbors list.
					match_neighbors(edge, *link->next);
					// Delete the matched edge from the list.
					HashEdge *temp = link->next;
					link->next = link->next->next;
					// pool.destroy(temp);
#ifndef NDEBUG
					++ this->freed;
#endif /* NDEBUG */
					break;
				}
				// This is not a match.  Go to the next link.
				link = link->next;
#ifndef NDEBUG
				++ this->collisions;
#endif /* NDEBUG */
			}
		}
	}

	// Edges equal for hashing. Edgesof different facet are allowed to be matched.
	static inline bool edges_equal(const HashEdge &edge_a, const HashEdge &edge_b)
	{
	    return edge_a.facet_number != edge_b.facet_number && edge_a == edge_b;
	}

	// Connect edge_a with edge_b, update edge connection statistics.
	static void record_neighbors(stl_file *stl, const HashEdge &edge_a, const HashEdge &edge_b)
	{
		// Facet a's neighbor is facet b
		stl->neighbors_start[edge_a.facet_number].neighbor[edge_a.which_edge % 3] = edge_b.facet_number;	/* sets the .neighbor part */
		stl->neighbors_start[edge_a.facet_number].which_vertex_not[edge_a.which_edge % 3] = (edge_b.which_edge + 2) % 3; /* sets the .which_vertex_not part */

		// Facet b's neighbor is facet a
		stl->neighbors_start[edge_b.facet_number].neighbor[edge_b.which_edge % 3] = edge_a.facet_number;	/* sets the .neighbor part */
		stl->neighbors_start[edge_b.facet_number].which_vertex_not[edge_b.which_edge % 3] = (edge_a.which_edge + 2) % 3; /* sets the .which_vertex_not part */

		if ((edge_a.which_edge < 3 && edge_b.which_edge < 3) || (edge_a.which_edge > 2 && edge_b.which_edge > 2)) {
			// These facets are oriented in opposite directions, their normals are probably messed up.
			stl->neighbors_start[edge_a.facet_number].which_vertex_not[edge_a.which_edge % 3] += 3;
			stl->neighbors_start[edge_b.facet_number].which_vertex_not[edge_b.which_edge % 3] += 3;
		}

		// Count successful connects:
		// Total connects:
		stl->stats.connected_edges += 2;
		// Count individual connects:
		switch (stl->neighbors_start[edge_a.facet_number].num_neighbors()) {
		case 1:	++ stl->stats.connected_facets_1_edge; break;
		case 2: ++ stl->stats.connected_facets_2_edge; break;
		case 3: ++ stl->stats.connected_facets_3_edge; break;
		default: assert(false);
		}
		switch (stl->neighbors_start[edge_b.facet_number].num_neighbors()) {
		case 1:	++ stl->stats.connected_facets_1_edge; break;
		case 2: ++ stl->stats.connected_facets_2_edge; break;
		case 3: ++ stl->stats.connected_facets_3_edge; break;
		default: assert(false);
		}
	}

	static void match_neighbors_nearby(stl_file *stl, const HashEdge &edge_a, const HashEdge &edge_b)
	{
		record_neighbors(stl, edge_a, edge_b);

		// Which vertices to change
		int facet1 = -1;
		int facet2 = -1;
		int vertex1, vertex2;
		stl_vertex new_vertex1, new_vertex2;
		{
			int v1a; // pair 1, facet a
			int v1b; // pair 1, facet b
			int v2a; // pair 2, facet a
			int v2b; // pair 2, facet b
			// Find first pair.
			if (edge_a.which_edge < 3) {
				v1a = edge_a.which_edge;
				v2a = (edge_a.which_edge + 1) % 3;
			} else {
				v2a = edge_a.which_edge % 3;
				v1a = (edge_a.which_edge + 1) % 3;
			}
			if (edge_b.which_edge < 3) {
				v1b = edge_b.which_edge;
				v2b = (edge_b.which_edge + 1) % 3;
			} else {
				v2b = edge_b.which_edge % 3;
				v1b = (edge_b.which_edge + 1) % 3;
			}

			// Of the first pair, which vertex, if any, should be changed
			if (stl->facet_start[edge_a.facet_number].vertex[v1a] != stl->facet_start[edge_b.facet_number].vertex[v1b]) {
				// These facets are different.
				if (   (stl->neighbors_start[edge_a.facet_number].neighbor[v1a] == -1)
			        && (stl->neighbors_start[edge_a.facet_number].neighbor[(v1a + 2) % 3] == -1)) {
			  		// This vertex has no neighbors.  This is a good one to change.
			  		facet1 = edge_a.facet_number;
			  		vertex1 = v1a;
			  		new_vertex1 = stl->facet_start[edge_b.facet_number].vertex[v1b];
				} else {
				  	facet1 = edge_b.facet_number;
			  		vertex1 = v1b;
			  		new_vertex1 = stl->facet_start[edge_a.facet_number].vertex[v1a];
				}
			}

			// Of the second pair, which vertex, if any, should be changed.
			if (stl->facet_start[edge_a.facet_number].vertex[v2a] != stl->facet_start[edge_b.facet_number].vertex[v2b]) {
				// These facets are different.
				if (  (stl->neighbors_start[edge_a.facet_number].neighbor[v2a] == -1)
			       && (stl->neighbors_start[edge_a.facet_number].neighbor[(v2a + 2) % 3] == -1)) {
			  		// This vertex has no neighbors.  This is a good one to change.
			  		facet2 = edge_a.facet_number;
			  		vertex2 = v2a;
			  		new_vertex2 = stl->facet_start[edge_b.facet_number].vertex[v2b];
				} else {
			  		facet2 = edge_b.facet_number;
			  		vertex2 = v2b;
			  		new_vertex2 = stl->facet_start[edge_a.facet_number].vertex[v2a];
				}
			}
		}

		auto change_vertices = [stl](int facet_num, int vnot, stl_vertex new_vertex)
		{
			int first_facet = facet_num;
			bool direction = false;

			for (;;) {
				int pivot_vertex;
				int next_edge;
				if (vnot > 2) {
					if (direction) {
						pivot_vertex = (vnot + 1) % 3;
						next_edge = vnot % 3;
					}
					else {
						pivot_vertex = (vnot + 2) % 3;
						next_edge = pivot_vertex;
					}
					direction = !direction;
				}
				else {
					if (direction) {
						pivot_vertex = (vnot + 2) % 3;
						next_edge = pivot_vertex;
					}
					else {
						pivot_vertex = (vnot + 1) % 3;
						next_edge = vnot;
					}
				}
	#if 0
				if (stl->facet_start[facet_num].vertex[pivot_vertex](0) == new_vertex(0) &&
					stl->facet_start[facet_num].vertex[pivot_vertex](1) == new_vertex(1) &&
					stl->facet_start[facet_num].vertex[pivot_vertex](2) == new_vertex(2))
					printf("Changing vertex %f,%f,%f: Same !!!\r\n", new_vertex(0), new_vertex(1), new_vertex(2));
				else {
					if (stl->facet_start[facet_num].vertex[pivot_vertex](0) != new_vertex(0))
						printf("Changing coordinate x, vertex %e (0x%08x) to %e(0x%08x)\r\n",
							stl->facet_start[facet_num].vertex[pivot_vertex](0),
							*reinterpret_cast<const int*>(&stl->facet_start[facet_num].vertex[pivot_vertex](0)),
							new_vertex(0),
							*reinterpret_cast<const int*>(&new_vertex(0)));
					if (stl->facet_start[facet_num].vertex[pivot_vertex](1) != new_vertex(1))
						printf("Changing coordinate x, vertex %e (0x%08x) to %e(0x%08x)\r\n",
							stl->facet_start[facet_num].vertex[pivot_vertex](1),
							*reinterpret_cast<const int*>(&stl->facet_start[facet_num].vertex[pivot_vertex](1)),
							new_vertex(1),
							*reinterpret_cast<const int*>(&new_vertex(1)));
					if (stl->facet_start[facet_num].vertex[pivot_vertex](2) != new_vertex(2))
						printf("Changing coordinate x, vertex %e (0x%08x) to %e(0x%08x)\r\n",
							stl->facet_start[facet_num].vertex[pivot_vertex](2),
							*reinterpret_cast<const int*>(&stl->facet_start[facet_num].vertex[pivot_vertex](2)),
							new_vertex(2),
							*reinterpret_cast<const int*>(&new_vertex(2)));
				}
	#endif
				stl->facet_start[facet_num].vertex[pivot_vertex] = new_vertex;
				vnot = stl->neighbors_start[facet_num].which_vertex_not[next_edge];
				facet_num = stl->neighbors_start[facet_num].neighbor[next_edge];
				if (facet_num == -1)
					break;

				if (facet_num == first_facet) {
					// back to the beginning
					BOOST_LOG_TRIVIAL(info) << "Back to the first facet changing vertices: probably a mobius part. Try using a smaller tolerance or don't do a nearby check.";
					return;
				}
			}
		};

		if (facet1 != -1) {
			int vnot1 = (facet1 == edge_a.facet_number) ? 
		  		(edge_a.which_edge + 2) % 3 :
				(edge_b.which_edge + 2) % 3;
			if (((vnot1 + 2) % 3) == vertex1)
		  		vnot1 += 3;
			change_vertices(facet1, vnot1, new_vertex1);
		}
		if (facet2 != -1) {
			int vnot2 = (facet2 == edge_a.facet_number) ?
		  		(edge_a.which_edge + 2) % 3 :
				(edge_b.which_edge + 2) % 3;
			if (((vnot2 + 2) % 3) == vertex2)
		  		vnot2 += 3;
			change_vertices(facet2, vnot2, new_vertex2);
		}
		stl->stats.edges_fixed += 2;
	}
};

// This function builds the neighbors list.  No modifications are made
// to any of the facets.  The edges are said to match only if all six
// floats of the first edge matches all six floats of the second edge.
void stl_check_facets_exact(stl_file *stl)
{
	assert(stl->facet_start.size() == stl->neighbors_start.size());

  	stl->stats.connected_edges         = 0;
  	stl->stats.connected_facets_1_edge = 0;
  	stl->stats.connected_facets_2_edge = 0;
  	stl->stats.connected_facets_3_edge = 0;

  	// If any two of the three vertices are found to be exactally the same, call them degenerate and remove the facet.
  	// Do it before the next step, as the next step stores references to the face indices in the hash tables and removing a facet
  	// will break the references.
  	for (uint32_t i = 0; i < stl->stats.number_of_facets;) {
		stl_facet &facet = stl->facet_start[i];
	  	if (facet.vertex[0] == facet.vertex[1] || facet.vertex[1] == facet.vertex[2] || facet.vertex[0] == facet.vertex[2]) {
		  	// Remove the degenerate facet.
		  	facet = stl->facet_start[-- stl->stats.number_of_facets];
			stl->facet_start.pop_back();
			stl->neighbors_start.pop_back();
		  	stl->stats.facets_removed += 1;
		  	stl->stats.degenerate_facets += 1;
	  	} else
		  	++ i;
  	}

  	// Initialize hash table.
  	HashTableEdges hash_table(stl->stats.number_of_facets);
	for (auto &neighbor : stl->neighbors_start)
		neighbor.reset();

  	// Connect neighbor edges.
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		const stl_facet &facet = stl->facet_start[i];
		for (int j = 0; j < 3; ++ j) {
			HashEdge edge;
			edge.facet_number = i;
			edge.which_edge = j;
			edge.load_exact(stl, &facet.vertex[j], &facet.vertex[(j + 1) % 3]);
			hash_table.insert_edge_exact(stl, edge);
		}
	}

#if 0
	printf("Number of faces: %d, number of manifold edges: %d, number of connected edges: %d, number of unconnected edges: %d\r\n", 
    	stl->stats.number_of_facets, stl->stats.number_of_facets * 3, 
    	stl->stats.connected_edges, stl->stats.number_of_facets * 3 - stl->stats.connected_edges);
#endif
}

void stl_check_facets_nearby(stl_file *stl, float tolerance)
{
	assert(stl->stats.connected_facets_3_edge <= stl->stats.connected_facets_2_edge);
	assert(stl->stats.connected_facets_2_edge <= stl->stats.connected_facets_1_edge);
	assert(stl->stats.connected_facets_1_edge <= stl->stats.number_of_facets);

  	if (stl->stats.connected_facets_3_edge == stl->stats.number_of_facets)
    	// No need to check any further.  All facets are connected.
    	return;

  	HashTableEdges hash_table(stl->stats.number_of_facets);
  	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
    	//FIXME is the copy necessary?
    	stl_facet facet = stl->facet_start[i];
    	for (int j = 0; j < 3; j++) {
      		if (stl->neighbors_start[i].neighbor[j] == -1) {
        		HashEdge edge;
        		edge.facet_number = i;
        		edge.which_edge = j;
        		if (edge.load_nearby(stl, facet.vertex[j], facet.vertex[(j + 1) % 3], tolerance))
          			// Only insert edges that have different keys.
          			hash_table.insert_edge_nearby(stl, edge);
      		}
    	}
  	}
}

void stl_remove_unconnected_facets(stl_file *stl)
{
	// A couple of things need to be done here.  One is to remove any completely unconnected facets (0 edges connected) since these are
	// useless and could be completely wrong.   The second thing that needs to be done is to remove any degenerate facets that were created during
	// stl_check_facets_nearby().
	auto remove_facet = [stl](int facet_number)
	{
		++ stl->stats.facets_removed;
		/* Update list of connected edges */
		stl_neighbors &neighbors = stl->neighbors_start[facet_number];
		// Update statistics on unconnected triangle edges.
		switch (neighbors.num_neighbors()) {
		case 3: -- stl->stats.connected_facets_3_edge; // fall through
		case 2: -- stl->stats.connected_facets_2_edge; // fall through
		case 1: -- stl->stats.connected_facets_1_edge; // fall through
		case 0: break;
		default: assert(false);
		}

	  	if (facet_number < int(-- stl->stats.number_of_facets)) {
	  		// Removing a face, which was not the last one.
		  	// Copy the face and neighborship from the last face to facet_number.
		  	stl->facet_start[facet_number] = stl->facet_start[stl->stats.number_of_facets];
		  	neighbors = stl->neighbors_start[stl->stats.number_of_facets];
		  	// Update neighborship of faces, which used to point to the last face, now moved to facet_number.
		  	for (int i = 0; i < 3; ++ i)
		    	if (neighbors.neighbor[i] != -1) {
			    	int &other_face_idx = stl->neighbors_start[neighbors.neighbor[i]].neighbor[(neighbors.which_vertex_not[i] + 1) % 3];
			  		if (other_face_idx != stl->stats.number_of_facets) {
			  			BOOST_LOG_TRIVIAL(info) << "in remove_facet: neighbor = " << other_face_idx << " numfacets = " << stl->stats.number_of_facets << " this is wrong";
			    		return;
			  		}
			  		other_face_idx = facet_number;
		  		}
		}

	  	stl->facet_start.pop_back();
	  	stl->neighbors_start.pop_back();
	};

	auto remove_degenerate = [stl, remove_facet](int facet)
	{
		// Update statistics on face connectivity after one edge was disconnected on the facet "facet_num".
		auto update_connects_remove_1 = [stl](int facet_num) {
			switch (stl->neighbors_start[facet_num].num_neighbors()) {
			case 0: assert(false); break;
			case 1: -- stl->stats.connected_facets_1_edge; break;
			case 2: -- stl->stats.connected_facets_2_edge; break;
			case 3: -- stl->stats.connected_facets_3_edge; break;
			default: assert(false);
		  	}
		};

		int edge_to_collapse = 0;
	   	if (stl->facet_start[facet].vertex[0] == stl->facet_start[facet].vertex[1]) {
			if (stl->facet_start[facet].vertex[1] == stl->facet_start[facet].vertex[2]) {
				// All 3 vertices are equal. Collapse the edge with no neighbor if it exists.
				const int *nbr = stl->neighbors_start[facet].neighbor;
				edge_to_collapse = (nbr[0] == -1) ? 0 : (nbr[1] == -1) ? 1 : 2;
			} else {
				edge_to_collapse = 0;
			}
	  	} else if (stl->facet_start[facet].vertex[1] == stl->facet_start[facet].vertex[2]) {
			edge_to_collapse = 1;
	  	} else if (stl->facet_start[facet].vertex[2] == stl->facet_start[facet].vertex[0]) {
			edge_to_collapse = 2;
	  	} else {
	    	// No degenerate. Function shouldn't have been called.
	    	return;
	  	}

		int edge[3] = { (edge_to_collapse + 1) % 3, (edge_to_collapse + 2) % 3, edge_to_collapse };
		int neighbor[] = {
			stl->neighbors_start[facet].neighbor[edge[0]],
			stl->neighbors_start[facet].neighbor[edge[1]],
			stl->neighbors_start[facet].neighbor[edge[2]]
		};
		int vnot[] = {
			stl->neighbors_start[facet].which_vertex_not[edge[0]],
			stl->neighbors_start[facet].which_vertex_not[edge[1]],
			stl->neighbors_start[facet].which_vertex_not[edge[2]]
		};

		// Update statistics on edge connectivity.
		if ((neighbor[0] == -1) && (neighbor[1] != -1))
			update_connects_remove_1(neighbor[1]);
		if ((neighbor[1] == -1) && (neighbor[0] != -1))
			update_connects_remove_1(neighbor[0]);

	  	if (neighbor[0] >= 0) {
			if (neighbor[1] >= 0) {
				// Adjust the "flip" flag for the which_vertex_not values.
				if (vnot[0] > 2) {
					if (vnot[1] > 2) {
						// The face to be removed has its normal flipped compared to the left & right neighbors, therefore after removing this face
						// the two remaining neighbors will be oriented correctly.
						vnot[0] -= 3;
						vnot[1] -= 3;
					} else
						// One neighbor has its normal inverted compared to the face to be removed, the other is oriented equally.
						// After removal, the two neighbors will have their normals flipped.
						vnot[1] += 3;
				} else if (vnot[1] > 2)
					// One neighbor has its normal inverted compared to the face to be removed, the other is oriented equally.
					// After removal, the two neighbors will have their normals flipped.
					vnot[0] += 3;
			}
			stl->neighbors_start[neighbor[0]].neighbor[(vnot[0] + 1) % 3] = (neighbor[0] == neighbor[1]) ? -1 : neighbor[1];
	    	stl->neighbors_start[neighbor[0]].which_vertex_not[(vnot[0] + 1) % 3] = vnot[1];
	  	}
	  	if (neighbor[1] >= 0) {
			stl->neighbors_start[neighbor[1]].neighbor[(vnot[1] + 1) % 3] = (neighbor[0] == neighbor[1]) ? -1 : neighbor[0];
	    	stl->neighbors_start[neighbor[1]].which_vertex_not[(vnot[1] + 1) % 3] = vnot[0];
	  	}
		if (neighbor[2] >= 0) {
			update_connects_remove_1(neighbor[2]);
			stl->neighbors_start[neighbor[2]].neighbor[(vnot[2] + 1) % 3] = -1;
		}

	  	remove_facet(facet);
	};

	// remove degenerate facets
	for (uint32_t i = 0; i < stl->stats.number_of_facets;)
		if (stl->facet_start[i].vertex[0] == stl->facet_start[i].vertex[1] ||
			stl->facet_start[i].vertex[0] == stl->facet_start[i].vertex[2] ||
			stl->facet_start[i].vertex[1] == stl->facet_start[i].vertex[2]) {
			remove_degenerate(i);
//			assert(stl_validate(stl));
		} else
			++ i;

	if (stl->stats.connected_facets_1_edge < (int)stl->stats.number_of_facets) {
		// There are some faces with no connected edge at all. Remove completely unconnected facets.
		for (uint32_t i = 0; i < stl->stats.number_of_facets;)
			if (stl->neighbors_start[i].num_neighbors() == 0) {
				// This facet is completely unconnected.  Remove it.
				remove_facet(i);
				assert(stl_validate(stl));
			} else
				++ i;
	}
}

void stl_fill_holes(stl_file *stl)
{
	// Insert all unconnected edges into hash list.
	HashTableEdges hash_table(stl->stats.number_of_facets);
	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
  		stl_facet facet = stl->facet_start[i];
		for (int j = 0; j < 3; ++ j) {
	  		if(stl->neighbors_start[i].neighbor[j] != -1)
	  			continue;
			HashEdge edge;
	  		edge.facet_number = i;
	  		edge.which_edge = j;
	  		edge.load_exact(stl, &facet.vertex[j], &facet.vertex[(j + 1) % 3]);
	  		hash_table.insert_edge_exact(stl, edge);
		}
	}

	for (uint32_t i = 0; i < stl->stats.number_of_facets; ++ i) {
		stl_facet facet = stl->facet_start[i];
		int neighbors_initial[3] = { stl->neighbors_start[i].neighbor[0], stl->neighbors_start[i].neighbor[1], stl->neighbors_start[i].neighbor[2] };
		int first_facet = i;
		for (int j = 0; j < 3; ++ j) {
	  		if (stl->neighbors_start[i].neighbor[j] != -1)
	  			continue;

  			stl_facet new_facet;
	  		new_facet.vertex[0] = facet.vertex[j];
	  		new_facet.vertex[1] = facet.vertex[(j + 1) % 3];
		  	bool direction = neighbors_initial[(j + 2) % 3] == -1;
  			int facet_num = i;
		  	int vnot = (j + 2) % 3;

	  		for (;;) {
				int pivot_vertex = 0;
				int next_edge = 0;
	    		if (vnot > 2) {
	      			if (direction) {
	        			pivot_vertex = (vnot + 1) % 3;
	        			next_edge = vnot % 3;
	      			} else {
	        			pivot_vertex = (vnot + 2) % 3;
	        			next_edge = pivot_vertex;
	      			}
	      			direction = ! direction;
	    		} else {
	      			if(direction == 0) {
	        			pivot_vertex = (vnot + 1) % 3;
	        			next_edge = vnot;
	      			} else {
	        			pivot_vertex = (vnot + 2) % 3;
	        			next_edge = pivot_vertex;
	      			}
	    		}

	    		int next_facet = stl->neighbors_start[facet_num].neighbor[next_edge];
	    		if (next_facet == -1) {
	      			new_facet.vertex[2] = stl->facet_start[facet_num].vertex[vnot % 3];
				    stl_add_facet(stl, &new_facet);
	      			for (int k = 0; k < 3; ++ k) {
	      				HashEdge edge;
	        			edge.facet_number = stl->stats.number_of_facets - 1;
	        			edge.which_edge = k;
	        			edge.load_exact(stl, &new_facet.vertex[k], &new_facet.vertex[(k + 1) % 3]);
	        			hash_table.insert_edge_exact(stl, edge);
	      			}
	      			break;
	    		}

	      		vnot = stl->neighbors_start[facet_num].which_vertex_not[next_edge];
	      		facet_num = next_facet;

	    		if (facet_num == first_facet) {
	      			// back to the beginning
		  			BOOST_LOG_TRIVIAL(info) << "Back to the first facet filling holes: probably a mobius part. Try using a smaller tolerance or don't do a nearby check.";
	      			return;
	    		}
	  		}
		}
	}
}

void stl_add_facet(stl_file *stl, const stl_facet *new_facet)
{
	assert(stl->facet_start.size() == stl->stats.number_of_facets);
	assert(stl->neighbors_start.size() == stl->stats.number_of_facets);
	stl->facet_start.emplace_back(*new_facet);
  	// note that the normal vector is not set here, just initialized to 0.
  	stl->facet_start[stl->stats.number_of_facets].normal = stl_normal::Zero();
  	stl->neighbors_start.emplace_back();
	++ stl->stats.facets_added;
	++ stl->stats.number_of_facets;
}
