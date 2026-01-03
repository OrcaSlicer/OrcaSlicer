// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "exact_geodesic.h"

//Copyright (C) 2008 Danil Kirsanov, MIT License
//Code from https://code.google.com/archive/p/geodesic/
// Compiled into a single file by Zhongshi Jiang

#include "PI.h"
#include <algorithm>
#include "IGL_ASSERT.h"
#include <cmath>
#include <cstddef>
#include <ctime>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>
#include <memory>
#include <cmath>
namespace igl{
namespace geodesic{

//#include "geodesic_constants_and_simple_functions.h"

//double const GEODESIC_INF = std::numeric_limits<double>::max();
double const GEODESIC_INF = 1e100;

//in order to avoid numerical problems with "infinitely small" intervals,
//we drop all the intervals smaller than SMALLEST_INTERVAL_RATIO*edge_length
double const SMALLEST_INTERVAL_RATIO = 1e-6;
//double const SMALL_EPSILON = 1e-10;


inline double cos_from_edges(double const a,			//compute the cosine of the angle given the lengths of the edges
							 double const b,
							 double const c)
{
	assert(a>1e-50);
	assert(b>1e-50);
	assert(c>1e-50);

	double result = (b*b + c*c - a*a)/(2.0*b*c);
	result = std::max(result, -1.0);
	return std::min(result, 1.0);
}

inline double angle_from_edges(double const a,			//compute the cosine of the angle given the lengths of the edges
							   double const b,
							   double const c)
{
	return acos(cos_from_edges(a,b,c));
}

template<class Points, class Faces>
inline bool read_mesh_from_file(char* filename,
								Points& points,
								Faces& faces)
{
	std::ifstream file(filename);
	assert(file.is_open());
	if(!file.is_open()) return false;

	unsigned num_points;
	file >> num_points;
	assert(num_points>=3);

	unsigned num_faces;
	file >> num_faces;

	points.resize(num_points*3);
	for(typename Points::iterator i=points.begin(); i!=points.end(); ++i)
	{
		file >> *i;
	}

	faces.resize(num_faces*3);
	for(typename Faces::iterator i=faces.begin(); i!=faces.end(); ++i)
	{
		file >> *i;
	}
	file.close();

	return true;
}

// #include "geodesic_memory"
template<class T>			//quickly allocates multiple elements of a given type; no deallocation
class SimlpeMemoryAllocator
{
public:
	typedef T* pointer;

	SimlpeMemoryAllocator(unsigned block_size = 0,
						  unsigned max_number_of_blocks = 0)
	{
		reset(block_size,
			  max_number_of_blocks);
	};

	~SimlpeMemoryAllocator(){};

	void reset(unsigned block_size,
			   unsigned max_number_of_blocks)
	{
		m_block_size = block_size;
		m_max_number_of_blocks = max_number_of_blocks;


		m_current_position = 0;

		m_storage.reserve(max_number_of_blocks);
		m_storage.resize(1);
		m_storage[0].resize(block_size);
	};

	pointer allocate(unsigned const n)		//allocate n units
	{
		assert(n < m_block_size);

		if(m_current_position + n >= m_block_size)
		{
			m_storage.push_back( std::vector<T>() );
			m_storage.back().resize(m_block_size);
			m_current_position = 0;
		}
		pointer result = & m_storage.back()[m_current_position];
		m_current_position += n;

		return result;
	};
private:
	std::vector<std::vector<T> > m_storage;
	unsigned m_block_size;				//size of a single block
	unsigned m_max_number_of_blocks;		//maximum allowed number of blocks
	unsigned m_current_position;			//first unused element inside the current block
};


template<class T>		//quickly allocates and deallocates single elements of a given type
class MemoryAllocator
{
public:
	typedef T* pointer;

	MemoryAllocator(unsigned block_size = 1024,
				    unsigned max_number_of_blocks = 1024)
	{
		reset(block_size,
			  max_number_of_blocks);
	};

	~MemoryAllocator(){};

	void clear()
	{
		reset(m_block_size,
			  m_max_number_of_blocks);
	}

	void reset(unsigned block_size,
			   unsigned max_number_of_blocks)
	{
		m_block_size = block_size;
		m_max_number_of_blocks = max_number_of_blocks;

		assert(m_block_size > 0);
		assert(m_max_number_of_blocks > 0);

		m_current_position = 0;

		m_storage.reserve(max_number_of_blocks);
		m_storage.resize(1);
		m_storage[0].resize(block_size);

		m_deleted.clear();
		m_deleted.reserve(2*block_size);
	};

	pointer allocate()		//allocates single unit of memory
	{
		pointer result;
		if(m_deleted.empty())
		{
			if(m_current_position + 1 >= m_block_size)
			{
				m_storage.push_back( std::vector<T>() );
				m_storage.back().resize(m_block_size);
				m_current_position = 0;
			}
			result = & m_storage.back()[m_current_position];
			++m_current_position;
		}
		else
		{
			result = m_deleted.back();
			m_deleted.pop_back();
		}

		return result;
	};

	void deallocate(pointer p)		//allocate n units
	{
		if(m_deleted.size() < m_deleted.capacity())
		{
			m_deleted.push_back(p);
		}
	};

private:
	std::vector<std::vector<T> > m_storage;
	unsigned m_block_size;				//size of a single block
	unsigned m_max_number_of_blocks;		//maximum allowed number of blocks
	unsigned m_current_position;			//first unused element inside the current block

	std::vector<pointer> m_deleted;			//pointers to deleted elemets
};


class OutputBuffer
{
public:
	OutputBuffer():
		m_num_bytes(0)
	{}

	void clear()
	{
		m_num_bytes = 0;
		m_buffer = std::shared_ptr<double>();
	}

	template<class T>
	T* allocate(unsigned n)
	{
		double wanted = n*sizeof(T);
		if(wanted > m_num_bytes)
		{
			unsigned new_size = (unsigned) std::ceil(wanted / (double)sizeof(double));
			m_buffer = std::shared_ptr<double>(new double[new_size]);
			m_num_bytes = new_size*sizeof(double);
		}

		return (T*)m_buffer.get();
	}

	template <class T>
	T* get()
	{
		return (T*)m_buffer.get();
	}

	template<class T>
	unsigned capacity()
	{
		return (unsigned)std::floor((double)m_num_bytes/(double)sizeof(T));
	};

private:

	std::shared_ptr<double> m_buffer;
	unsigned m_num_bytes;
};




class Vertex;
class Edge;
class Face;
class Mesh;
class MeshElementBase;

typedef Vertex* vertex_pointer;
typedef Edge* edge_pointer;
typedef Face* face_pointer;
typedef Mesh* mesh_pointer;
typedef MeshElementBase* base_pointer;

template <class Data>		//simple vector that stores info about mesh references
class SimpleVector			//for efficiency, it uses an outside memory allocator
{
public:
	SimpleVector():
	  m_size(0),
	  m_begin(NULL)
	{};

	typedef Data* iterator;

	unsigned size(){return m_size;};
	iterator begin(){return m_begin;};
	iterator end(){return m_begin + m_size;};

	template<class DataPointer>
	void set_allocation(DataPointer begin, unsigned size)
	{
		assert(begin != NULL || size == 0);
		m_size = size;
		m_begin = (iterator)begin;
	}

	Data& operator[](unsigned i)
	{
		assert(i < m_size);
		return *(m_begin + i);
	}

	void clear()
	{
		m_size = 0;
		m_begin = NULL;
	}

private:
	unsigned m_size;
	Data* m_begin;
};

enum PointType
{
    VERTEX,
    EDGE,
    FACE,
	UNDEFINED_POINT
};

class MeshElementBase	//prototype of vertices, edges and faces
{
public:
	typedef SimpleVector<vertex_pointer> vertex_pointer_vector;
	typedef SimpleVector<edge_pointer> edge_pointer_vector;
	typedef SimpleVector<face_pointer> face_pointer_vector;

	MeshElementBase():
		m_id(0),
		m_type(UNDEFINED_POINT)
	{};

	vertex_pointer_vector& adjacent_vertices(){return m_adjacent_vertices;};
	edge_pointer_vector& adjacent_edges(){return m_adjacent_edges;};
	face_pointer_vector& adjacent_faces(){return m_adjacent_faces;};

	unsigned& id(){return m_id;};
	PointType type(){return m_type;};

protected:
	vertex_pointer_vector m_adjacent_vertices;		//list of the adjacent vertices
	edge_pointer_vector m_adjacent_edges;			//list of the adjacent edges
	face_pointer_vector m_adjacent_faces;			//list of the adjacent faces

	unsigned m_id;							//unique id
	PointType m_type;							//vertex, edge or face
};

class Point3D			//point in 3D and corresponding operations
{
public:
	Point3D(){};
	Point3D(Point3D* p)
	{
		x() = p->x();
		y() = p->y();
		z() = p->z();
	};

	double* xyz(){return m_coordinates;};
	double& x(){return *m_coordinates;};
	double& y(){return *(m_coordinates+1);};
	double& z(){return *(m_coordinates+2);};

	void set(double new_x, double new_y, double new_z)
	{
		x() = new_x;
		y() = new_y;
		z() = new_z;
	}

	void set(double* data)
	{
		x() = *data;
		y() = *(data+1);
		z() = *(data+2);
	}

	double distance(double* v)
	{
		double dx = m_coordinates[0] - v[0];
		double dy = m_coordinates[1] - v[1];
		double dz = m_coordinates[2] - v[2];

		return sqrt(dx*dx + dy*dy + dz*dz);
	};

    double distance(Point3D* v)
	{
		return distance(v->xyz());
	};

	void add(Point3D* v)
	{
		x() += v->x();
		y() += v->y();
		z() += v->z();
	};

	void multiply(double v)
	{
		x() *= v;
		y() *= v;
		z() *= v;
	};

private:
	double m_coordinates[3];					//xyz
};

class Vertex: public MeshElementBase, public Point3D
{
public:
	Vertex()
	{
		m_type = VERTEX;
	};

	~Vertex(){};

	bool& saddle_or_boundary(){return m_saddle_or_boundary;};
private:
									//this flag speeds up exact geodesic algorithm
	bool m_saddle_or_boundary;		//it is true if total adjacent angle is larger than 2*PI or this vertex belongs to the mesh boundary
};


class Face: public MeshElementBase
{
public:
	Face()
	{
		m_type = FACE;
	};

	~Face(){};

	edge_pointer opposite_edge(vertex_pointer v);
	vertex_pointer opposite_vertex(edge_pointer e);
	edge_pointer next_edge(edge_pointer e, vertex_pointer v);

	double vertex_angle(vertex_pointer v)
	{
		for(unsigned i=0; i<3; ++i)
		{
			if(adjacent_vertices()[i]->id() == v->id())
			{
				return m_corner_angles[i];
			}
		}
		assert(0);
		return 0;
	}

	double* corner_angles(){return m_corner_angles;};

private:
	double m_corner_angles[3];		//triangle angles in radians; angles correspond to vertices in m_adjacent_vertices
};

class Edge: public MeshElementBase
{
public:
	Edge()
	{
		m_type = EDGE;
	};

	~Edge(){};

	double& length(){return m_length;};

	face_pointer opposite_face(face_pointer f)
	{
		if(adjacent_faces().size() == 1)
		{
			assert(adjacent_faces()[0]->id() == f->id());
			return NULL;
		}

		assert(adjacent_faces()[0]->id() == f->id() ||
			   adjacent_faces()[1]->id() == f->id());

		return adjacent_faces()[0]->id() == f->id() ?
			   adjacent_faces()[1] : adjacent_faces()[0];
	};

	vertex_pointer opposite_vertex(vertex_pointer v)
	{
		assert(belongs(v));

		return adjacent_vertices()[0]->id() == v->id() ?
			   adjacent_vertices()[1] : adjacent_vertices()[0];
	};

	bool belongs(vertex_pointer v)
	{
		return adjacent_vertices()[0]->id() == v->id() ||
			   adjacent_vertices()[1]->id() == v->id();
	}

	bool is_boundary(){return adjacent_faces().size() == 1;};

	vertex_pointer v0(){return adjacent_vertices()[0];};
	vertex_pointer v1(){return adjacent_vertices()[1];};

	void local_coordinates(Point3D* point,
						   double& x,
						   double& y)
	{
		double d0 = point->distance(v0());
		if(d0 < 1e-50)
		{
			x = 0.0;
			y = 0.0;
			return;
		}

		double d1 = point->distance(v1());
		if(d1 < 1e-50)
		{
			x = m_length;
			y = 0.0;
			return;
		}

		x = m_length/2.0 + (d0*d0 - d1*d1)/(2.0*m_length);
		y = sqrt(std::max(0.0, d0*d0 - x*x));
		return;
	}

private:
	double m_length;							//length of the edge
};

class SurfacePoint:public Point3D  //point on the surface of the mesh
{
public:
	SurfacePoint():
		m_p(NULL)
	{};

	SurfacePoint(vertex_pointer v):		//set the surface point in the vertex
		SurfacePoint::Point3D(v),
		m_p(v)
	{};

	SurfacePoint(face_pointer f):		//set the surface point in the center of the face
		m_p(f)
	{
		set(0,0,0);
		add(f->adjacent_vertices()[0]);
		add(f->adjacent_vertices()[1]);
		add(f->adjacent_vertices()[2]);
		multiply(1./3.);
	};

	SurfacePoint(edge_pointer e,		//set the surface point in the middle of the edge
				 double a = 0.5):
		m_p(e)
	{
		double b = 1 - a;

		vertex_pointer v0 = e->adjacent_vertices()[0];
		vertex_pointer v1 = e->adjacent_vertices()[1];

		x() = b*v0->x() + a*v1->x();
		y() = b*v0->y() + a*v1->y();
		z() = b*v0->z() + a*v1->z();
	};

	SurfacePoint(base_pointer g,
				 double x,
				 double y,
				 double z,
				 PointType /*t = UNDEFINED_POINT*/):
		m_p(g)
	{
		set(x,y,z);
	};

	void initialize(SurfacePoint const& p)
	{
		*this = p;
	}

	~SurfacePoint(){};

	PointType type(){return m_p ? m_p->type() : UNDEFINED_POINT;};
	base_pointer& base_element(){return m_p;};
protected:
	base_pointer m_p;			//could be face, vertex or edge pointer
};

inline edge_pointer Face::opposite_edge(vertex_pointer v)
{
	for(unsigned i=0; i<3; ++i)
	{
		edge_pointer e = adjacent_edges()[i];
		if(!e->belongs(v))
		{
			return e;
		}
	}
	assert(0);
	return NULL;
}

inline vertex_pointer Face::opposite_vertex(edge_pointer e)
{
	for(unsigned i=0; i<3; ++i)
	{
		vertex_pointer v = adjacent_vertices()[i];
		if(!e->belongs(v))
		{
			return v;
		}
	}
	assert(0);
	return NULL;
}

inline edge_pointer Face::next_edge(edge_pointer e, vertex_pointer v)
{
	assert(e->belongs(v));

	for(unsigned i=0; i<3; ++i)
	{
		edge_pointer next = adjacent_edges()[i];
		if(e->id() != next->id() && next->belongs(v))
		{
			return next;
		}
	}
	assert(0);
	return NULL;
}

struct HalfEdge			//prototype of the edge; used for mesh construction
{
	unsigned face_id;
	unsigned vertex_0;		//adjacent vertices sorted by id value
	unsigned vertex_1;		//they are sorted, vertex_0 < vertex_1
};

inline bool operator < (const HalfEdge &x, const HalfEdge &y)
{
	if(x.vertex_0 == y.vertex_0)
	{
	    return x.vertex_1 < y.vertex_1;
	}
	else
	{
		return x.vertex_0 < y.vertex_0;
	}
}

inline bool operator != (const HalfEdge &x, const HalfEdge &y)
{
	return x.vertex_0 != y.vertex_0 || x.vertex_1 != y.vertex_1;
}

inline bool operator == (const HalfEdge &x, const HalfEdge &y)
{
	return x.vertex_0 == y.vertex_0 && x.vertex_1 == y.vertex_1;
}

struct edge_visible_from_source
{
	unsigned source;
	edge_pointer edge;
};

class Mesh
{
public:
	Mesh()
	{};

	~Mesh(){};

	template<class Points, class Faces>
	void initialize_mesh_data(unsigned num_vertices,
							  Points& p,
							  unsigned num_faces,
							  Faces& tri);		//build mesh from regular point-triangle representation

	template<class Points, class Faces>
	void initialize_mesh_data(Points& p, Faces& tri);		//build mesh from regular point-triangle representation

	std::vector<Vertex>& vertices(){return m_vertices;};
	std::vector<Edge>& edges(){return m_edges;};
	std::vector<Face>& faces(){return m_faces;};

	unsigned closest_vertices(SurfacePoint* p,
								 std::vector<vertex_pointer>* storage = NULL);		//list vertices closest to the point

private:

	void build_adjacencies();		//build internal structure of the mesh
	bool verify();					//verifies connectivity of the mesh and prints some debug info

	typedef void* void_pointer;
	void_pointer allocate_pointers(unsigned n)
	{
		return m_pointer_allocator.allocate(n);
	}

	std::vector<Vertex> m_vertices;
	std::vector<Edge> m_edges;
	std::vector<Face> m_faces;

	SimlpeMemoryAllocator<void_pointer> m_pointer_allocator;	//fast memory allocating for Face/Vertex/Edge cross-references
};

inline unsigned Mesh::closest_vertices(SurfacePoint* p,
										  std::vector<vertex_pointer>* storage)
{
	assert(p->type() != UNDEFINED_POINT);

	if(p->type() == VERTEX)
	{
		if(storage)
		{
			storage->push_back(static_cast<vertex_pointer>(p->base_element()));
		}
		return 1;
	}
	else if(p->type() == FACE)
	{
		if(storage)
		{
			vertex_pointer* vp= p->base_element()->adjacent_vertices().begin();
			storage->push_back(*vp);
			storage->push_back(*(vp+1));
			storage->push_back(*(vp+2));
		}
		return 2;
	}
	else if(p->type() == EDGE)		//for edge include all 4 adjacent vertices
	{
		edge_pointer edge = static_cast<edge_pointer>(p->base_element());

		if(storage)
		{
			storage->push_back(edge->adjacent_vertices()[0]);
			storage->push_back(edge->adjacent_vertices()[1]);

			for(unsigned i = 0; i < edge->adjacent_faces().size(); ++i)
			{
				face_pointer face = edge->adjacent_faces()[i];
				storage->push_back(face->opposite_vertex(edge));
			}
		}
		return 2 + edge->adjacent_faces().size();
	}

	assert(0);
	return 0;
}

template<class Points, class Faces>
void Mesh::initialize_mesh_data(Points& p, Faces& tri)		//build mesh from regular point-triangle representation
{
	assert(p.size() % 3 == 0);
	unsigned const num_vertices = p.size() / 3;
	assert(tri.size() % 3 == 0);
	unsigned const num_faces = tri.size() / 3;

	initialize_mesh_data(num_vertices, p, num_faces, tri);
}

template<class Points, class Faces>
void Mesh::initialize_mesh_data(unsigned num_vertices,
								Points& p,
								unsigned num_faces,
								Faces& tri)
{
	unsigned const approximate_number_of_internal_pointers = (num_vertices + num_faces)*4;
	unsigned const max_number_of_pointer_blocks = 100;
	m_pointer_allocator.reset(approximate_number_of_internal_pointers,
							  max_number_of_pointer_blocks);

	m_vertices.resize(num_vertices);
	for(unsigned i=0; i<num_vertices; ++i)		//copy coordinates to vertices
	{
		Vertex& v = m_vertices[i];
		v.id() = i;

		unsigned shift = 3*i;
		v.x() = p[shift];
		v.y() = p[shift + 1];
		v.z() = p[shift + 2];
	}

	m_faces.resize(num_faces);
	for(unsigned i=0; i<num_faces; ++i)		//copy adjacent vertices to polygons/faces
	{
		Face& f = m_faces[i];
		f.id() = i;
		f.adjacent_vertices().set_allocation(allocate_pointers(3),3);	//allocate three units of memory

		unsigned shift = 3*i;
		for(unsigned j=0; j<3; ++j)
		{
			unsigned vertex_index = tri[shift + j];
			assert(vertex_index < num_vertices);
			f.adjacent_vertices()[j] = &m_vertices[vertex_index];
		}
	}

	build_adjacencies();	//build the structure of the mesh
}

inline void Mesh::build_adjacencies()
{
	//		Vertex->adjacent Faces
	std::vector<unsigned> count(m_vertices.size());	//count adjacent vertices
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		Face& f = m_faces[i];
		for(unsigned j=0; j<3; ++j)
		{
			unsigned vertex_id = f.adjacent_vertices()[j]->id();
			assert(vertex_id < m_vertices.size());
			count[vertex_id]++;
		}
	}

	for(unsigned i=0; i<m_vertices.size(); ++i)		//reserve space
	{
		Vertex& v = m_vertices[i];
		unsigned num_adjacent_faces = count[i];

		v.adjacent_faces().set_allocation(allocate_pointers(num_adjacent_faces),		//allocate three units of memory
										  num_adjacent_faces);
	}

	std::fill(count.begin(), count.end(), 0);
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		Face& f = m_faces[i];
		for(unsigned j=0; j<3; ++j)
		{
			vertex_pointer v = f.adjacent_vertices()[j];
			v->adjacent_faces()[count[v->id()]++] = &f;
		}
	}

	//find all edges
	//i.e. find all half-edges, sort and combine them into edges
	std::vector<HalfEdge> half_edges(m_faces.size()*3);
	unsigned k = 0;
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		Face& f = m_faces[i];
		for(unsigned j=0; j<3; ++j)
		{
			half_edges[k].face_id = i;
			unsigned vertex_id_1 = f.adjacent_vertices()[j]->id();
			unsigned vertex_id_2 = f.adjacent_vertices()[(j+1) % 3]->id();
			half_edges[k].vertex_0 = std::min(vertex_id_1, vertex_id_2);
			half_edges[k].vertex_1 = std::max(vertex_id_1, vertex_id_2);

			k++;
		}
	}
	std::sort(half_edges.begin(), half_edges.end());

	unsigned number_of_edges = 1;
	for(unsigned i=1; i<half_edges.size(); ++i)
	{
		if(half_edges[i] != half_edges[i-1])
		{
			++number_of_edges;
		}
		else
		{
			if(i<half_edges.size()-1)		//sanity check: there should be at most two equal half-edges
			{								//if it fails, most likely the input data are messed up
				assert(half_edges[i] != half_edges[i+1]);
			}
		}
	}

	//		Edges->adjacent Vertices and Faces
	m_edges.resize(number_of_edges);
	unsigned edge_id = 0;
	for(unsigned i=0; i<half_edges.size();)
	{
		Edge& e = m_edges[edge_id];
		e.id() = edge_id++;

		e.adjacent_vertices().set_allocation(allocate_pointers(2),2);		//allocate two units of memory

		e.adjacent_vertices()[0] = &m_vertices[half_edges[i].vertex_0];
		e.adjacent_vertices()[1] = &m_vertices[half_edges[i].vertex_1];

		e.length() = e.adjacent_vertices()[0]->distance(e.adjacent_vertices()[1]);
		assert(e.length() > 1e-100);		//algorithm works well with non-degenerate meshes only

		if(i != half_edges.size()-1 && half_edges[i] == half_edges[i+1])	//double edge
		{
			e.adjacent_faces().set_allocation(allocate_pointers(2),2);
			e.adjacent_faces()[0] = &m_faces[half_edges[i].face_id];
			e.adjacent_faces()[1] = &m_faces[half_edges[i+1].face_id];
			i += 2;
		}
		else			//single edge
		{
			e.adjacent_faces().set_allocation(allocate_pointers(1),1);		//one adjucent faces
			e.adjacent_faces()[0] = &m_faces[half_edges[i].face_id];
			i += 1;
		}
	}

	//			Vertices->adjacent Edges
	std::fill(count.begin(), count.end(), 0);
	for(unsigned i=0; i<m_edges.size(); ++i)
	{
		Edge& e = m_edges[i];
		assert(e.adjacent_vertices().size()==2);
		count[e.adjacent_vertices()[0]->id()]++;
		count[e.adjacent_vertices()[1]->id()]++;
	}
	for(unsigned i=0; i<m_vertices.size(); ++i)
	{
		m_vertices[i].adjacent_edges().set_allocation(allocate_pointers(count[i]),
													  count[i]);
	}
	std::fill(count.begin(), count.end(), 0);
	for(unsigned i=0; i<m_edges.size(); ++i)
	{
		Edge& e = m_edges[i];
		for(unsigned j=0; j<2; ++j)
		{
			vertex_pointer v = e.adjacent_vertices()[j];
			v->adjacent_edges()[count[v->id()]++] = &e;
		}
	}

	//			Faces->adjacent Edges
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		m_faces[i].adjacent_edges().set_allocation(allocate_pointers(3),3);
	}

	count.resize(m_faces.size());
	std::fill(count.begin(), count.end(), 0);
	for(unsigned i=0; i<m_edges.size(); ++i)
	{
		Edge& e = m_edges[i];
		for(unsigned j=0; j<e.adjacent_faces().size(); ++j)
		{
			face_pointer f = e.adjacent_faces()[j];
			assert(count[f->id()]<3);
			f->adjacent_edges()[count[f->id()]++] = &e;
		}
	}

		//compute angles for the faces
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		Face& f = m_faces[i];
		double abc[3];
		double sum = 0;
		for(unsigned j=0; j<3; ++j)		//compute angle adjacent to the vertex j
		{
			for(unsigned k=0; k<3; ++k)
			{
				vertex_pointer v = f.adjacent_vertices()[(j + k)%3];
				abc[k] = f.opposite_edge(v)->length();
			}

			double angle = angle_from_edges(abc[0], abc[1], abc[2]);
			assert(angle>1e-5);						//algorithm works well with non-degenerate meshes only

			f.corner_angles()[j] = angle;
			sum += angle;
		}
		IGL_ASSERT(std::abs(sum - igl::PI) < 1e-5);		//algorithm works well with non-degenerate meshes only
	}

		//define m_turn_around_flag for vertices
	std::vector<double> total_vertex_angle(m_vertices.size());
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		Face& f = m_faces[i];
		for(unsigned j=0; j<3; ++j)
		{
			vertex_pointer v = f.adjacent_vertices()[j];
			total_vertex_angle[v->id()] += f.corner_angles()[j];
		}
	}

	for(unsigned i=0; i<m_vertices.size(); ++i)
	{
		Vertex& v = m_vertices[i];
		v.saddle_or_boundary() = (total_vertex_angle[v.id()] > 2.0*igl::PI - 1e-5);
	}

	for(unsigned i=0; i<m_edges.size(); ++i)
	{
		Edge& e = m_edges[i];
		if(e.is_boundary())
		{
			e.adjacent_vertices()[0]->saddle_or_boundary() = true;
			e.adjacent_vertices()[1]->saddle_or_boundary() = true;
		}
	}

	assert(verify());
}

inline bool Mesh::verify()		//verifies connectivity of the mesh and prints some debug info
{
	// make sure that all vertices are mentioned at least once.
	// though the loose vertex is not a bug, it most likely indicates that something is wrong with the mesh
	std::vector<bool> map(m_vertices.size(), false);
	for(unsigned i=0; i<m_edges.size(); ++i)
	{
		edge_pointer e = &m_edges[i];
		map[e->adjacent_vertices()[0]->id()] = true;
		map[e->adjacent_vertices()[1]->id()] = true;
	}
	assert(std::find(map.begin(), map.end(), false) == map.end());

	//make sure that the mesh is connected trough its edges
	//if mesh has more than one connected component, it is most likely a bug
	std::vector<face_pointer> stack(1,&m_faces[0]);
	stack.reserve(m_faces.size());

	map.resize(m_faces.size());
	std::fill(map.begin(), map.end(), false);
	map[0] = true;

	while(!stack.empty())
	{
		face_pointer f = stack.back();
		stack.pop_back();

		for(unsigned i=0; i<3; ++i)
		{
			edge_pointer e = f->adjacent_edges()[i];
			face_pointer f_adjacent = e->opposite_face(f);
			if(f_adjacent && !map[f_adjacent->id()])
			{
				map[f_adjacent->id()] = true;
				stack.push_back(f_adjacent);
			}
		}
	}
	assert(std::find(map.begin(), map.end(), false) == map.end());

	//print some mesh statistics that can be useful in debugging
	// std::cout << "mesh has "	<< m_vertices.size()
	// 		  << " vertices, "	<< m_faces.size()
	// 		  << " faces, "		<< m_edges.size()
	// 		  << " edges\n";

	//unsigned total_boundary_edges = 0;
	double longest_edge = 0;
	double shortest_edge = 1e100;
	for(unsigned i=0; i<m_edges.size(); ++i)
	{
		Edge& e = m_edges[i];
		//total_boundary_edges += e.is_boundary() ? 1 : 0;
		longest_edge = std::max(longest_edge, e.length());
		shortest_edge = std::min(shortest_edge, e.length());
	}
	// std::cout << total_boundary_edges << " edges are boundary edges\n";
	// std::cout << "shortest/longest edges are "
	// 		  << shortest_edge << "/"
	// 		  << longest_edge << " = "
	// 		  << shortest_edge/longest_edge
	// 		  << std::endl;

	double minx = 1e100;
	double maxx = -1e100;
	double miny = 1e100;
	double maxy = -1e100;
	double minz = 1e100;
	double maxz = -1e100;
	for(unsigned i=0; i<m_vertices.size(); ++i)
	{
		Vertex& v = m_vertices[i];
		minx = std::min(minx, v.x());
		maxx = std::max(maxx, v.x());
		miny = std::min(miny, v.y());
		maxy = std::max(maxy, v.y());
		minz = std::min(minz, v.z());
		maxz = std::max(maxz, v.z());
	}
	// std::cout << "enclosing XYZ box:"
	// 		  <<" X[" << minx << "," << maxx << "]"
	// 		  <<" Y[" << miny << "," << maxy << "]"
	// 		  <<" Z[" << minz << "," << maxz << "]"
	// 		  << std::endl;

	//double dx = maxx - minx;
	//double dy = maxy - miny;
	//double dz = maxz - minz;
	// std::cout << "approximate diameter of the mesh is "
	// 		  << sqrt(dx*dx + dy*dy + dz*dz)
	// 		  << std::endl;

	double min_angle = 1e100;
	double max_angle = -1e100;
	for(unsigned i=0; i<m_faces.size(); ++i)
	{
		Face& f = m_faces[i];
		for(unsigned j=0; j<3; ++j)
		{
			double angle = f.corner_angles()[j];
			min_angle = std::min(min_angle, angle);
			max_angle = std::max(max_angle, angle);
		}
	}
	// std::cout << "min/max face angles are "
	// 		  << min_angle/igl::PI*180.0 << "/"
	// 		  << max_angle/igl::PI*180.0
	// 		  << " degrees\n";

	// std::cout << std::endl;
	return true;
}

inline void fill_surface_point_structure(geodesic::SurfacePoint* point,
										 double* data,
										 Mesh* mesh)
{
	point->set(data);
	unsigned type = (unsigned) data[3];
	unsigned id = (unsigned) data[4];


	if(type == 0)		//vertex
	{
		point->base_element() = &mesh->vertices()[id];
	}
	else if(type == 1)	//edge
	{
		point->base_element() = &mesh->edges()[id];
	}
	else				//face
	{
		point->base_element() = &mesh->faces()[id];
	}
}

inline void fill_surface_point_double(geodesic::SurfacePoint* point,
									  double* data,
									  long /*mesh_id*/)
{
	data[0] = point->x();
	data[1] = point->y();
	data[2] = point->z();
	data[4] = point->base_element()->id();

	if(point->type() == VERTEX)		//vertex
	{
		data[3] = 0;
	}
	else if(point->type() == EDGE)	//edge
	{
		data[3] = 1;
	}
	else				//face
	{
		data[3] = 2;
	}
}

class Interval;
class IntervalList;
typedef Interval* interval_pointer;
typedef IntervalList* list_pointer;

class Interval						//interval of the edge
{
public:

	Interval(){};
	~Interval(){};

	enum DirectionType
    {
        FROM_FACE_0,
		FROM_FACE_1,
		FROM_SOURCE,
		UNDEFINED_DIRECTION
    };

	double signal(double x)		//geodesic distance function at point x
	{
		assert(x>=0.0 && x <= m_edge->length());

		if(m_d == GEODESIC_INF)
		{
			return GEODESIC_INF;
		}
		else
		{
			double dx = x - m_pseudo_x;
			if(m_pseudo_y == 0.0)
			{
				return m_d + std::abs(dx);
			}
			else
			{
				return m_d + sqrt(dx*dx + m_pseudo_y*m_pseudo_y);
			}
		}
	}

	double max_distance(double end)
	{
		if(m_d == GEODESIC_INF)
		{
			return GEODESIC_INF;
		}
		else
		{
			double a = std::abs(m_start - m_pseudo_x);
			double b = std::abs(end - m_pseudo_x);

			return a > b ? m_d + sqrt(a*a + m_pseudo_y*m_pseudo_y):
						   m_d + sqrt(b*b + m_pseudo_y*m_pseudo_y);
		}
	}

	void compute_min_distance(double stop)			//compute min, given c,d theta, start, end.
	{
		assert(stop > m_start);

		if(m_d == GEODESIC_INF)
		{
			m_min = GEODESIC_INF;
		}
		else if(m_start > m_pseudo_x)
		{
			m_min = signal(m_start);
		}
		else if(stop < m_pseudo_x)
		{
			m_min = signal(stop);
		}
		else
		{
			assert(m_pseudo_y<=0);
			m_min = m_d - m_pseudo_y;
		}
	}
			//compare two intervals in the queue
	bool operator()(interval_pointer const x, interval_pointer const y) const
	{
		if(x->min() != y->min())
		{
			return x->min() < y->min();
		}
		else if(x->start() != y->start())
		{
			return x->start() < y->start();
		}
		else
		{
			return x->edge()->id() < y->edge()->id();
		}
	}

	double stop()		//return the endpoint of the interval
	{
		return m_next ? m_next->start() : m_edge->length();
	}

	double hypotenuse(double a, double b)
	{
		return sqrt(a*a + b*b);
	}

	void find_closest_point(double const x,
						    double const y,
						    double& offset,
						    double& distance);			//find the point on the interval that is closest to the point (alpha, s)

	double& start(){return m_start;};
	double& d(){return m_d;};
	double& pseudo_x(){return m_pseudo_x;};
	double& pseudo_y(){return m_pseudo_y;};
	double& min(){return m_min;};
	interval_pointer& next(){return m_next;};
	edge_pointer& edge(){return m_edge;};
	DirectionType& direction(){return m_direction;};
	bool visible_from_source(){return m_direction == FROM_SOURCE;};
	unsigned& source_index(){return m_source_index;};

	void initialize(edge_pointer edge,
					SurfacePoint* point = NULL,
					unsigned source_index = 0);

protected:
	double m_start;						//initial point of the interval on the edge
	double m_d;							//distance from the source to the pseudo-source
	double m_pseudo_x;					//coordinates of the pseudo-source in the local coordinate system
	double m_pseudo_y;					//y-coordinate should be always negative
	double m_min;						//minimum distance on the interval

	interval_pointer m_next;			//pointer to the next interval in the list
	edge_pointer m_edge;				//edge that the interval belongs to
	unsigned m_source_index;			//the source it belongs to
	DirectionType m_direction;			//where the interval is coming from
};

struct IntervalWithStop : public Interval
{
public:
	double& stop(){return m_stop;};
protected:
	double m_stop;
};

class IntervalList						//list of the of intervals of the given edge
{
public:
	IntervalList(){m_first = NULL;};
	~IntervalList(){};

	void clear()
	{
		m_first = NULL;
	};

	void initialize(edge_pointer e)
	{
		m_edge = e;
		m_first = NULL;
	};

	interval_pointer covering_interval(double offset)			//returns the interval that covers the offset
	{
		assert(offset >= 0.0 && offset <= m_edge->length());

		interval_pointer p = m_first;
		while(p && p->stop() < offset)
		{
			p = p->next();
		}

		return p;// && p->start() <= offset ? p : NULL;
	};

	void find_closest_point(SurfacePoint* point,
							double& offset,
							double& distance,
							interval_pointer& interval)
	{
		interval_pointer p = m_first;
		distance = GEODESIC_INF;
		interval = NULL;

		double x,y;
		m_edge->local_coordinates(point, x, y);

		while(p)
		{
			if(p->min()<GEODESIC_INF)
			{
				double o, d;
				p->find_closest_point(x, y, o, d);
				if(d < distance)
				{
					distance = d;
					offset = o;
					interval = p;
				}
			}
			p = p->next();
		}
	};

	unsigned number_of_intervals()
	{
		interval_pointer p = m_first;
		unsigned count = 0;
		while(p)
		{
			++count;
			p = p->next();
		}
		return count;
	}

	interval_pointer last()
	{
		interval_pointer p = m_first;
		if(p)
		{
			while(p->next())
			{
				p = p->next();
			}
		}
		return p;
	}

	double signal(double x)
	{
		interval_pointer interval = covering_interval(x);

		return interval ? interval->signal(x) : GEODESIC_INF;
	}

	interval_pointer& first(){return m_first;};
	edge_pointer& edge(){return m_edge;};
private:
	interval_pointer m_first;			//pointer to the first member of the list
	edge_pointer m_edge;				//edge that owns this list
};

class SurfacePointWithIndex : public SurfacePoint
{
public:
	unsigned index(){return m_index;};

	void initialize(SurfacePoint& p, unsigned index)
	{
		SurfacePoint::initialize(p);
		m_index = index;
	}

	bool operator()(SurfacePointWithIndex* x, SurfacePointWithIndex* y) const //used for sorting
	{
		assert(x->type() != UNDEFINED_POINT && y->type() !=UNDEFINED_POINT);

		if(x->type() != y->type())
		{
			return x->type() < y->type();
		}
		else
		{
			return x->base_element()->id() < y->base_element()->id();
		}
	}

private:
	unsigned m_index;
};

class SortedSources : public std::vector<SurfacePointWithIndex>
{
private:
	typedef std::vector<SurfacePointWithIndex*> sorted_vector_type;
public:
	typedef sorted_vector_type::iterator sorted_iterator;
	typedef std::pair<sorted_iterator, sorted_iterator> sorted_iterator_pair;

	sorted_iterator_pair sources(base_pointer mesh_element)
	{
		m_search_dummy.base_element() = mesh_element;

		return equal_range(m_sorted.begin(),
						   m_sorted.end(),
						   &m_search_dummy,
						   m_compare_less);
	}

	void initialize(std::vector<SurfacePoint>& sources)	//we initialize the sources by copie
	{
		resize(sources.size());
		m_sorted.resize(sources.size());
		for(unsigned i=0; i<sources.size(); ++i)
		{
			SurfacePointWithIndex& p = *(begin() + i);

			p.initialize(sources[i],i);
			m_sorted[i] = &p;
		}

		std::sort(m_sorted.begin(), m_sorted.end(), m_compare_less);
	};

	SurfacePointWithIndex& operator[](unsigned i)
	{
		assert(i < size());
		return *(begin() + i);
	}

private:
	sorted_vector_type m_sorted;
	SurfacePointWithIndex m_search_dummy;		//used as a search template
	SurfacePointWithIndex m_compare_less;			//used as a compare functor
};


inline void Interval::find_closest_point(double const rs,
										 double const hs,
										 double& r,
										 double& d_out)			//find the point on the interval that is closest to the point (alpha, s)
	{
		if(m_d == GEODESIC_INF)
		{
			r = GEODESIC_INF;
			d_out = GEODESIC_INF;
			return;
		}

		double hc = -m_pseudo_y;
		double rc = m_pseudo_x;
		double end = stop();

		double local_epsilon = SMALLEST_INTERVAL_RATIO*m_edge->length();
		if(std::abs(hs+hc) < local_epsilon)
		{
			if(rs<=m_start)
			{
				r = m_start;
				d_out = signal(m_start) + std::abs(rs - m_start);
			}
			else if(rs>=end)
			{
				r = end;
				d_out = signal(end) + fabs(end - rs);
			}
			else
			{
				r = rs;
				d_out = signal(rs);
			}
		}
		else
		{
			double ri = (rs*hc + hs*rc)/(hs+hc);

			if(ri<m_start)
			{
				r = m_start;
				d_out = signal(m_start) + hypotenuse(m_start - rs, hs);
			}
			else if(ri>end)
			{
				r = end;
				d_out = signal(end) + hypotenuse(end - rs, hs);
			}
			else
			{
				r = ri;
				d_out = m_d + hypotenuse(rc - rs, hc + hs);
			}
		}
	}


inline void Interval::initialize(edge_pointer edge,
								 SurfacePoint* source,
								 unsigned source_index)
{
	m_next = NULL;
	//m_geodesic_previous = NULL;
	m_direction = UNDEFINED_DIRECTION;
	m_edge = edge;
	m_source_index = source_index;

	m_start = 0.0;
	//m_stop = edge->length();
	if(!source)
	{
		m_d = GEODESIC_INF;
		m_min = GEODESIC_INF;
		return;
	}
	m_d = 0;

	if(source->base_element()->type() == VERTEX)
	{
		if(source->base_element()->id() == edge->v0()->id())
		{
			m_pseudo_x = 0.0;
			m_pseudo_y = 0.0;
			m_min = 0.0;
			return;
		}
		else if(source->base_element()->id() == edge->v1()->id())
		{
			m_pseudo_x = stop();
			m_pseudo_y = 0.0;
			m_min = 0.0;
			return;
		}
	}

	edge->local_coordinates(source, m_pseudo_x, m_pseudo_y);
	m_pseudo_y = -m_pseudo_y;

	compute_min_distance(stop());
}



// #include "geodesic_algorithm_base.h"
class GeodesicAlgorithmBase
{
public:
    enum AlgorithmType
    {
        EXACT,
		DIJKSTRA,
        SUBDIVISION,
		UNDEFINED_ALGORITHM
    };

	GeodesicAlgorithmBase(geodesic::Mesh* mesh):
		m_type(UNDEFINED_ALGORITHM),
		m_max_propagation_distance(1e100),
		m_mesh(mesh)
	{};

	virtual ~GeodesicAlgorithmBase(){};

	virtual void propagate(std::vector<SurfacePoint>& sources,
   						   double max_propagation_distance = GEODESIC_INF,			//propagation algorithm stops after reaching the certain distance from the source
						   std::vector<SurfacePoint>* stop_points = NULL) = 0; //or after ensuring that all the stop_points are covered

	virtual void trace_back(SurfacePoint& destination,		//trace back piecewise-linear path
							std::vector<SurfacePoint>& path) = 0;

	void geodesic(SurfacePoint& source,
						  SurfacePoint& destination,
						  std::vector<SurfacePoint>& path); //lazy people can find geodesic path with one function call

	void geodesic(std::vector<SurfacePoint>& sources,
						  std::vector<SurfacePoint>& destinations,
						  std::vector<std::vector<SurfacePoint> >& paths); //lazy people can find geodesic paths with one function call

	virtual unsigned best_source(SurfacePoint& point,			//after propagation step is done, quickly find what source this point belongs to and what is the distance to this source
								 double& best_source_distance) = 0;

	virtual void print_statistics()		//print info about timing and memory usage in the propagation step of the algorithm
	{
		std::cout << "propagation step took " << m_time_consumed << " seconds " << std::endl;
	};

	AlgorithmType type(){return m_type;};

	virtual std::string name();

	geodesic::Mesh* mesh(){return m_mesh;};
protected:

	void set_stop_conditions(std::vector<SurfacePoint>* stop_points,
						     double stop_distance);
	double stop_distance()
	{
		return m_max_propagation_distance;
	}

	AlgorithmType m_type;					   // type of the algorithm

	typedef std::pair<vertex_pointer, double> stop_vertex_with_distace_type;
	std::vector<stop_vertex_with_distace_type> m_stop_vertices; // algorithm stops propagation after covering certain vertices
	double m_max_propagation_distance;			 // or reaching the certain distance

	geodesic::Mesh* m_mesh;

	double m_time_consumed;		//how much time does the propagation step takes
	double m_propagation_distance_stopped;		//at what distance (if any) the propagation algorithm stopped
};

inline double length(std::vector<SurfacePoint>& path)
{
	double length = 0;
	if(!path.empty())
	{
		for(unsigned i=0; i<path.size()-1; ++i)
		{
			length += path[i].distance(&path[i+1]);
		}
	}
	return length;
}

inline void print_info_about_path(std::vector<SurfacePoint>& path)
{
	std::cout << "number of the points in the path = " << path.size()
			  << ", length of the path = " << length(path)
			  << std::endl;
}

inline std::string GeodesicAlgorithmBase::name()
{
	switch(m_type)
	{
	case EXACT:
		return "exact";
	case DIJKSTRA:
		return "dijkstra";
	case SUBDIVISION:
		return "subdivision";
	default:
	case UNDEFINED_ALGORITHM:
		return "undefined";
	}
}

inline void GeodesicAlgorithmBase::geodesic(SurfacePoint& source,
											SurfacePoint& destination,
											std::vector<SurfacePoint>& path) //lazy people can find geodesic path with one function call
{
	std::vector<SurfacePoint> sources(1, source);
	std::vector<SurfacePoint> stop_points(1, destination);
	double const max_propagation_distance = GEODESIC_INF;

	propagate(sources,
			  max_propagation_distance,
			  &stop_points);

	trace_back(destination, path);
}

inline void GeodesicAlgorithmBase::geodesic(std::vector<SurfacePoint>& sources,
											std::vector<SurfacePoint>& destinations,
											std::vector<std::vector<SurfacePoint> >& paths) //lazy people can find geodesic paths with one function call
{
	double const max_propagation_distance = GEODESIC_INF;

	propagate(sources,
			  max_propagation_distance,
			  &destinations);		//we use desinations as stop points

	paths.resize(destinations.size());

	for(unsigned i=0; i<paths.size(); ++i)
	{
		trace_back(destinations[i], paths[i]);
	}
}

inline void GeodesicAlgorithmBase::set_stop_conditions(std::vector<SurfacePoint>* stop_points,
														double stop_distance)
{
	m_max_propagation_distance = stop_distance;

	if(!stop_points)
	{
		m_stop_vertices.clear();
		return;
	}

	m_stop_vertices.resize(stop_points->size());

	std::vector<vertex_pointer> possible_vertices;
	for(unsigned i = 0; i < stop_points->size(); ++i)
	{
		SurfacePoint* point = &(*stop_points)[i];

		possible_vertices.clear();
		m_mesh->closest_vertices(point, &possible_vertices);

		vertex_pointer closest_vertex = NULL;
		double min_distance = 1e100;
		for(unsigned j = 0; j < possible_vertices.size(); ++j)
		{
			double distance = point->distance(possible_vertices[j]);
			if(distance < min_distance)
			{
				min_distance = distance;
				closest_vertex = possible_vertices[j];
			}
		}
		assert(closest_vertex);

		m_stop_vertices[i].first = closest_vertex;
		m_stop_vertices[i].second = min_distance;
	}
}



class GeodesicAlgorithmExact : public GeodesicAlgorithmBase
{
public:
	GeodesicAlgorithmExact(geodesic::Mesh* mesh):
	  	GeodesicAlgorithmBase(mesh),
		m_memory_allocator(mesh->edges().size(), mesh->edges().size()),
		m_edge_interval_lists(mesh->edges().size())
	{
		m_type = EXACT;

		for(unsigned i=0; i<m_edge_interval_lists.size(); ++i)
		{
			m_edge_interval_lists[i].initialize(&mesh->edges()[i]);
		}
	};

	~GeodesicAlgorithmExact(){};

	void propagate(std::vector<SurfacePoint>& sources,
   				   double max_propagation_distance = GEODESIC_INF,			//propagation algorithm stops after reaching the certain distance from the source
				   std::vector<SurfacePoint>* stop_points = NULL); //or after ensuring that all the stop_points are covered

	void trace_back(SurfacePoint& destination,		//trace back piecewise-linear path
					std::vector<SurfacePoint>& path);

	unsigned best_source(SurfacePoint& point,			//quickly find what source this point belongs to and what is the distance to this source
		double& best_source_distance);

	void print_statistics();

private:
	typedef std::set<interval_pointer, Interval> IntervalQueue;

	void update_list_and_queue(list_pointer list,
							   IntervalWithStop* candidates,	//up to two candidates
							   unsigned num_candidates);

	unsigned compute_propagated_parameters(double pseudo_x,
											double pseudo_y,
											double d,		//parameters of the interval
											double start,
											double end,		//start/end of the interval
											double alpha,	//corner angle
											double L,		//length of the new edge
											bool first_interval,		//if it is the first interval on the edge
											bool last_interval,
											bool turn_left,
											bool turn_right,
											IntervalWithStop* candidates);		//if it is the last interval on the edge

	void construct_propagated_intervals(bool invert,
									  edge_pointer edge,
									  face_pointer face,		//constructs iNew from the rest of the data
									  IntervalWithStop* candidates,
									  unsigned& num_candidates,
									  interval_pointer source_interval);

	double compute_positive_intersection(double start,
										 double pseudo_x,
										 double pseudo_y,
										 double sin_alpha,
										 double cos_alpha);		//used in construct_propagated_intervals

	unsigned intersect_intervals(interval_pointer zero,
								    IntervalWithStop* one);			//intersecting two intervals with up to three intervals in the end

	interval_pointer best_first_interval(SurfacePoint& point,
										double& best_total_distance,
										double& best_interval_position,
										unsigned& best_source_index);

	bool check_stop_conditions(unsigned& index);

	void clear()
	{
		m_memory_allocator.clear();
		m_queue.clear();
		for(unsigned i=0; i<m_edge_interval_lists.size(); ++i)
		{
			m_edge_interval_lists[i].clear();
		}
		m_propagation_distance_stopped = GEODESIC_INF;
	};

	list_pointer interval_list(edge_pointer e)
	{
		return &m_edge_interval_lists[e->id()];
	};

	void set_sources(std::vector<SurfacePoint>& sources)
	{
		m_sources.initialize(sources);
	}

	void initialize_propagation_data();

	void list_edges_visible_from_source(MeshElementBase* p,
										std::vector<edge_pointer>& storage); //used in initialization

	long visible_from_source(SurfacePoint& point);	//used in backtracing

	void best_point_on_the_edge_set(SurfacePoint& point,
									std::vector<edge_pointer> const& storage,
									interval_pointer& best_interval,
									double& best_total_distance,
									double& best_interval_position);

	void possible_traceback_edges(SurfacePoint& point,
								  std::vector<edge_pointer>& storage);

	bool erase_from_queue(interval_pointer p);

	IntervalQueue m_queue;	//interval queue

	MemoryAllocator<Interval> m_memory_allocator;			//quickly allocate and deallocate intervals
	std::vector<IntervalList> m_edge_interval_lists;		//every edge has its interval data

	enum MapType {OLD, NEW};		//used for interval intersection
	MapType map[5];
	double start[6];
	interval_pointer i_new[5];

	unsigned m_queue_max_size;			//used for statistics
	unsigned m_iterations;			//used for statistics

	SortedSources m_sources;
};

inline void GeodesicAlgorithmExact::best_point_on_the_edge_set(SurfacePoint& point,
															   std::vector<edge_pointer> const& storage,
															   interval_pointer& best_interval,
															   double& best_total_distance,
															   double& best_interval_position)
{
	best_total_distance = 1e100;
	for(unsigned i=0; i<storage.size(); ++i)
	{
		edge_pointer e = storage[i];
		list_pointer list = interval_list(e);

		double offset;
		double distance;
		interval_pointer interval;

		list->find_closest_point(&point,
								 offset,
								 distance,
								 interval);

		if(distance < best_total_distance)
		{
			best_interval = interval;
			best_total_distance = distance;
			best_interval_position = offset;
		}
	}
}

inline void GeodesicAlgorithmExact::possible_traceback_edges(SurfacePoint& point,
															 std::vector<edge_pointer>& storage)
{
	storage.clear();

	if(point.type() == VERTEX)
	{
		vertex_pointer v = static_cast<vertex_pointer>(point.base_element());
		for(unsigned i=0; i<v->adjacent_faces().size(); ++i)
		{
			face_pointer f = v->adjacent_faces()[i];
			storage.push_back(f->opposite_edge(v));
		}
	}
	else if(point.type() == EDGE)
	{
		edge_pointer e = static_cast<edge_pointer>(point.base_element());
		for(unsigned i=0; i<e->adjacent_faces().size(); ++i)
		{
			face_pointer f = e->adjacent_faces()[i];

			storage.push_back(f->next_edge(e,e->v0()));
			storage.push_back(f->next_edge(e,e->v1()));
		}
	}
	else
	{
		face_pointer f = static_cast<face_pointer>(point.base_element());
		storage.push_back(f->adjacent_edges()[0]);
		storage.push_back(f->adjacent_edges()[1]);
		storage.push_back(f->adjacent_edges()[2]);
	}
}


inline long GeodesicAlgorithmExact::visible_from_source(SurfacePoint& point)	//negative if not visible
{
	assert(point.type() != UNDEFINED_POINT);

	if(point.type() == EDGE)
	{
		edge_pointer e = static_cast<edge_pointer>(point.base_element());
		list_pointer list = interval_list(e);
		double position = std::min(point.distance(e->v0()), e->length());
		interval_pointer interval = list->covering_interval(position);
		//assert(interval);
		if(interval && interval->visible_from_source())
		{
			return (long)interval->source_index();
		}
		else
		{
			return -1;
		}
	}
	else if(point.type() == FACE)
	{
		return -1;
	}
	else if(point.type() == VERTEX)
	{
		vertex_pointer v = static_cast<vertex_pointer>(point.base_element());
		for(unsigned i=0; i<v->adjacent_edges().size(); ++i)
		{
			edge_pointer e = v->adjacent_edges()[i];
			list_pointer list = interval_list(e);

			double position = e->v0()->id() == v->id() ? 0.0 : e->length();
			interval_pointer interval = list->covering_interval(position);
			if(interval && interval->visible_from_source())
			{
				return (long)interval->source_index();
			}
		}

		return -1;
	}

	assert(0);
	return 0;
}

inline double GeodesicAlgorithmExact::compute_positive_intersection(double start,
																	double pseudo_x,
																	double pseudo_y,
																	double sin_alpha,
																	double cos_alpha)
{
	assert(pseudo_y < 0);

	double denominator = sin_alpha*(pseudo_x - start) - cos_alpha*pseudo_y;
	if(denominator<0.0)
	{
		return -1.0;
	}

	double numerator = -pseudo_y*start;

	if(numerator < 1e-30)
	{
		return 0.0;
	}

	if(denominator < 1e-30)
	{
		return -1.0;
	}

	return numerator/denominator;
}

inline void GeodesicAlgorithmExact::list_edges_visible_from_source(MeshElementBase* p,
																   std::vector<edge_pointer>& storage)
{
	assert(p->type() != UNDEFINED_POINT);

	if(p->type() == FACE)
	{
		face_pointer f = static_cast<face_pointer>(p);
		for(unsigned i=0; i<3; ++i)
		{
			storage.push_back(f->adjacent_edges()[i]);
		}
	}
	else if(p->type() == EDGE)
	{
		edge_pointer e = static_cast<edge_pointer>(p);
		storage.push_back(e);
	}
	else			//VERTEX
	{
		vertex_pointer v = static_cast<vertex_pointer>(p);
		for(unsigned i=0; i<v->adjacent_edges().size(); ++i)
		{
			storage.push_back(v->adjacent_edges()[i]);
		}

	}
}

inline bool GeodesicAlgorithmExact::erase_from_queue(interval_pointer p)
{
	if(p->min() < GEODESIC_INF/10.0)// && p->min >= queue->begin()->first)
	{
		assert(m_queue.count(p)<=1);			//the set is unique

		IntervalQueue::iterator it = m_queue.find(p);

		if(it != m_queue.end())
		{
			m_queue.erase(it);
			return true;
		}
	}

	return false;
}

inline unsigned GeodesicAlgorithmExact::intersect_intervals(interval_pointer zero,
															   IntervalWithStop* one)			//intersecting two intervals with up to three intervals in the end
{
	assert(zero->edge()->id() == one->edge()->id());
	assert(zero->stop() > one->start() && zero->start() < one->stop());
	assert(one->min() < GEODESIC_INF/10.0);

	double const local_epsilon = SMALLEST_INTERVAL_RATIO*one->edge()->length();

	unsigned N=0;
	if(zero->min() > GEODESIC_INF/10.0)
	{
		start[0] = zero->start();
		if(zero->start() < one->start() - local_epsilon)
		{
			map[0] = OLD;
			start[1] = one->start();
			map[1] = NEW;
			N = 2;
		}
		else
		{
			map[0] = NEW;
			N = 1;
		}

		if(zero->stop() > one->stop() + local_epsilon)
		{
			map[N] = OLD;							//"zero" interval
			start[N++] = one->stop();
		}

		start[N+1] = zero->stop();
		return N;
	}

	double const local_small_epsilon = 1e-8*one->edge()->length();

	double D = zero->d() - one->d();
	double x0 = zero->pseudo_x();
	double x1 = one->pseudo_x();
	double R0 = x0*x0 + zero->pseudo_y()*zero->pseudo_y();
	double R1 = x1*x1 + one->pseudo_y()*one->pseudo_y();

	double inter[2];									//points of intersection
	char Ninter=0;										//number of the points of the intersection

	if(std::abs(D)<local_epsilon)					//if d1 == d0, equation is linear
	{
		double denom = x1 - x0;
		if(std::abs(denom)>local_small_epsilon)
		{
			inter[0] =  (R1 - R0)/(2.*denom);					//one solution
			Ninter = 1;
		}
	}
	else
	{
		double D2 = D*D;
		double Q = 0.5*(R1-R0-D2);
		double X = x0 - x1;

		double A = X*X - D2;
		double B = Q*X + D2*x0;
		double C = Q*Q - D2*R0;

		if (std::abs(A)<local_small_epsilon)							//if A == 0, linear equation
		{
			if(std::abs(B)>local_small_epsilon)
			{
				inter[0] =  -C/B;							//one solution
				Ninter = 1;
			}
		}
		else
		{
			double det = B*B-A*C;
			if(det>local_small_epsilon*local_small_epsilon)			//two roots
			{
				det = sqrt(det);
				if(A>0.0)								//make sure that the roots are ordered
				{
					inter[0] = (-B - det)/A;
					inter[1] = (-B + det)/A;
				}
				else
				{
					inter[0] = (-B + det)/A;
					inter[1] = (-B - det)/A;
				}

				if(inter[1] - inter[0] > local_small_epsilon)
				{
					Ninter = 2;
				}
				else
				{
					Ninter = 1;
				}
			}
			else if(det>=0.0)					//single root
			{
				inter[0] = -B/A;
				Ninter = 1;
			}
		}
	}
	//---------------------------find possible intervals---------------------------------------
	double left = std::max(zero->start(), one->start());		//define left and right boundaries of the intersection of the intervals
	double right = std::min(zero->stop(), one->stop());

	double good_start[4];										//points of intersection within the (left, right) limits +"left" + "right"
	good_start[0] = left;
	unsigned char Ngood_start=1;										//number of the points of the intersection

	for(unsigned char i=0; i<Ninter; ++i)							//for all points of intersection
	{
		double x = inter[i];
		if(x > left + local_epsilon && x < right - local_epsilon)
		{
			good_start[Ngood_start++] = x;
		}
	}
	good_start[Ngood_start++] = right;

	MapType mid_map[3];
	for(unsigned char i=0; i<Ngood_start-1; ++i)
	{
		double mid = (good_start[i] + good_start[i+1])*0.5;
		mid_map[i] = zero->signal(mid) <= one->signal(mid) ? OLD : NEW;
	}

	//-----------------------------------output----------------------------------
	N = 0;
	if(zero->start() < left - local_epsilon)						//additional "zero" interval
	{
		if(mid_map[0] == OLD)				//first interval in the map is already the old one
		{
			good_start[0] = zero->start();
		}
		else
		{
			map[N] = OLD;					//"zero" interval
			start[N++] = zero->start();
		}
	}

	for(long i=0;i<Ngood_start-1;++i)							//for all intervals
	{
		MapType current_map = mid_map[i];
		if(N==0 || map[N-1] != current_map)
		{
			map[N] = current_map;
			start[N++] = good_start[i];
		}
	}

	if(zero->stop() > one->stop() + local_epsilon)
	{
		if(N==0 || map[N-1] == NEW)
		{
			map[N] = OLD;							//"zero" interval
			start[N++] = one->stop();
		}
	}

	start[0] = zero->start();		// just to make sure that epsilons do not damage anything
	//start[N] = zero->stop();

	return N;
}

inline void GeodesicAlgorithmExact::initialize_propagation_data()
{
	clear();

	IntervalWithStop candidate;
	std::vector<edge_pointer> edges_visible_from_source;
	for(unsigned i=0; i<m_sources.size(); ++i)		//for all edges adjacent to the starting vertex
	{
		SurfacePoint* source = &m_sources[i];

		edges_visible_from_source.clear();
		list_edges_visible_from_source(source->base_element(),
									   edges_visible_from_source);

		for(unsigned j=0; j<edges_visible_from_source.size(); ++j)
		{
			edge_pointer e = edges_visible_from_source[j];
			candidate.initialize(e, source, i);
            candidate.stop() = e->length();
			candidate.compute_min_distance(candidate.stop());
			candidate.direction() = Interval::FROM_SOURCE;

			update_list_and_queue(interval_list(e), &candidate, 1);
		}
	}
}

inline void GeodesicAlgorithmExact::propagate(std::vector<SurfacePoint>& sources,
   									   double max_propagation_distance,			//propagation algorithm stops after reaching the certain distance from the source
									   std::vector<SurfacePoint>* stop_points)
{
	set_stop_conditions(stop_points, max_propagation_distance);
	set_sources(sources);
	initialize_propagation_data();

	clock_t start = clock();

	unsigned satisfied_index = 0;

	m_iterations = 0;		//for statistics
	m_queue_max_size = 0;

	IntervalWithStop candidates[2];

	while(!m_queue.empty())
	{
		m_queue_max_size = std::max(static_cast<unsigned int>(m_queue.size()), m_queue_max_size);

		unsigned const check_period = 10;
    	if(++m_iterations % check_period == 0)		//check if we covered all required vertices
		{
			if (check_stop_conditions(satisfied_index))
			{
				break;
			}
		}

		interval_pointer min_interval = *m_queue.begin();
		m_queue.erase(m_queue.begin());
		edge_pointer edge = min_interval->edge();
		//list_pointer list = interval_list(edge);

		assert(min_interval->d() < GEODESIC_INF);

		bool const first_interval = min_interval->start() == 0.0;
		//bool const last_interval = min_interval->stop() == edge->length();
		bool const last_interval = min_interval->next() == NULL;

		bool const turn_left = edge->v0()->saddle_or_boundary();
		bool const turn_right = edge->v1()->saddle_or_boundary();

		for(unsigned i=0; i<edge->adjacent_faces().size(); ++i)		//two possible faces to propagate
		{
			if(!edge->is_boundary())		//just in case, always propagate boundary edges
			{
				if((i == 0 && min_interval->direction() == Interval::FROM_FACE_0) ||
					(i == 1 && min_interval->direction() == Interval::FROM_FACE_1))
				{
					continue;
				}
			}

			face_pointer face = edge->adjacent_faces()[i];			//if we come from 1, go to 2
			edge_pointer next_edge = face->next_edge(edge,edge->v0());

			unsigned num_propagated = compute_propagated_parameters(min_interval->pseudo_x(),
																	 min_interval->pseudo_y(),
																	 min_interval->d(),		//parameters of the interval
																	 min_interval->start(),
																	 min_interval->stop(),		//start/end of the interval
																	 face->vertex_angle(edge->v0()),	//corner angle
																	 next_edge->length(),		//length of the new edge
																	 first_interval,		//if it is the first interval on the edge
																	 last_interval,
																	 turn_left,
																	 turn_right,
																	 candidates);		//if it is the last interval on the edge
			bool propagate_to_right = true;

			if(num_propagated)
			{
				if(candidates[num_propagated-1].stop() != next_edge->length())
				{
					propagate_to_right = false;
				}

				bool const invert = next_edge->v0()->id() != edge->v0()->id(); //if the origins coinside, do not invert intervals

				construct_propagated_intervals(invert,		//do not inverse
											 next_edge,
											 face,
											 candidates,
											 num_propagated,
											 min_interval);

				update_list_and_queue(interval_list(next_edge),
									  candidates,
									  num_propagated);
			}

			if(propagate_to_right)
			{
									//propogation to the right edge
				double length = edge->length();
				next_edge = face->next_edge(edge,edge->v1());

				num_propagated = compute_propagated_parameters(length - min_interval->pseudo_x(),
															 min_interval->pseudo_y(),
															 min_interval->d(),		//parameters of the interval
															 length - min_interval->stop(),
															 length - min_interval->start(),		//start/end of the interval
															 face->vertex_angle(edge->v1()),	//corner angle
															 next_edge->length(),		//length of the new edge
															 last_interval,		//if it is the first interval on the edge
															 first_interval,
															 turn_right,
															 turn_left,
															 candidates);		//if it is the last interval on the edge

				if(num_propagated)
				{
					bool const invert = next_edge->v0()->id() != edge->v1()->id();		//if the origins coinside, do not invert intervals

					construct_propagated_intervals(invert,		//do not inverse
												 next_edge,
												 face,
												 candidates,
												 num_propagated,
												 min_interval);

					update_list_and_queue(interval_list(next_edge),
									      candidates,
										  num_propagated);
				}
			}
		}
	}

	m_propagation_distance_stopped = m_queue.empty() ? GEODESIC_INF : (*m_queue.begin())->min();
	clock_t stop = clock();
	m_time_consumed = (static_cast<double>(stop)-static_cast<double>(start))/CLOCKS_PER_SEC;

/*	for(unsigned i=0; i<m_edge_interval_lists.size(); ++i)
	{
		list_pointer list = &m_edge_interval_lists[i];
		interval_pointer p = list->first();
		assert(p->start() == 0.0);
		while(p->next())
		{
			assert(p->stop() == p->next()->start());
			assert(p->d() < GEODESIC_INF);
			p = p->next();
		}
	}*/
}


inline bool GeodesicAlgorithmExact::check_stop_conditions(unsigned& index)
{
	double queue_distance = (*m_queue.begin())->min();
	if(queue_distance < stop_distance())
	{
		return false;
	}

	while(index < m_stop_vertices.size())
	{
		vertex_pointer v = m_stop_vertices[index].first;
		edge_pointer edge = v->adjacent_edges()[0];				//take any edge

		double distance = edge->v0()->id() == v->id() ?
						  interval_list(edge)->signal(0.0) :
						  interval_list(edge)->signal(edge->length());

		if(queue_distance < distance + m_stop_vertices[index].second)
		{
			return false;
		}

		++index;
	}
	return true;
}


inline void GeodesicAlgorithmExact::update_list_and_queue(list_pointer list,
												IntervalWithStop* candidates,	//up to two candidates
												unsigned num_candidates)
{
	assert(num_candidates <= 2);
	//assert(list->first() != NULL);
	edge_pointer edge = list->edge();
	double const local_epsilon = SMALLEST_INTERVAL_RATIO * edge->length();

	if(list->first() == NULL)
	{
		interval_pointer* p = &list->first();
		IntervalWithStop* first;
		IntervalWithStop* second;

		if(num_candidates == 1)
		{
			first = candidates;
			second = candidates;
			first->compute_min_distance(first->stop());
		}
		else
		{
			if(candidates->start() <= (candidates+1)->start())
			{
				first = candidates;
				second = candidates+1;
			}
			else
			{
				first = candidates+1;
				second = candidates;
			}
			assert(first->stop() == second->start());

			first->compute_min_distance(first->stop());
			second->compute_min_distance(second->stop());
		}

		if(first->start() > 0.0)
		{
			*p = m_memory_allocator.allocate();
			(*p)->initialize(edge);
			p = &(*p)->next();
		}

		*p = m_memory_allocator.allocate();
		memcpy(*p,first,sizeof(Interval));
		m_queue.insert(*p);

		if(num_candidates == 2)
		{
			p = &(*p)->next();
			*p = m_memory_allocator.allocate();
			memcpy(*p,second,sizeof(Interval));
			m_queue.insert(*p);
		}

		if(second->stop() < edge->length())
		{
			p = &(*p)->next();
			*p = m_memory_allocator.allocate();
			(*p)->initialize(edge);
			(*p)->start() = second->stop();
		}
		else
		{
			(*p)->next() = NULL;
		}
		return;
	}

	bool propagate_flag;

	for(unsigned i=0; i<num_candidates; ++i)				//for all new intervals
	{
		IntervalWithStop* q = &candidates[i];

		interval_pointer previous = NULL;

		interval_pointer p = list->first();
		assert(p->start() == 0.0);

		while(p != NULL && p->stop() - local_epsilon < q->start())
		{
			p = p->next();
		}

		while(p != NULL && p->start() < q->stop() - local_epsilon)			//go through all old intervals
		{
			unsigned const N = intersect_intervals(p, q);								//interset two intervals

			if(N == 1)
			{
				if(map[0]==OLD)	//if "p" is always better, we do not need to update anything)
				{
					if(previous)		//close previous interval and put in into the queue
					{
						previous->next() = p;
						previous->compute_min_distance(p->start());
						m_queue.insert(previous);
						previous = NULL;
					}

					p = p->next();

				}
				else if(previous)	//extend previous interval to cover everything; remove p
				{
					previous->next() = p->next();
					erase_from_queue(p);
					m_memory_allocator.deallocate(p);

					p = previous->next();
				}
				else				//p becomes "previous"
				{
					previous = p;
					interval_pointer next = p->next();
					erase_from_queue(p);

					memcpy(previous,q,sizeof(Interval));

					previous->start() = start[0];
					previous->next() = next;

					p = next;
				}
				continue;
			}

			//update_flag = true;

			Interval swap(*p);							//used for swapping information
			propagate_flag = erase_from_queue(p);

			for(unsigned j=1; j<N; ++j)				//no memory is needed for the first one
			{
				i_new[j] = m_memory_allocator.allocate();	//create new intervals
			}

			if(map[0]==OLD)	//finish previous, if any
			{
				if(previous)
				{
					previous->next() = p;
					previous->compute_min_distance(previous->stop());
					m_queue.insert(previous);
					previous = NULL;
				}
				i_new[0] = p;
				p->next() = i_new[1];
				p->start() = start[0];
			}
			else if(previous)	//extend previous interval to cover everything; remove p
			{
				i_new[0] = previous;
				previous->next() = i_new[1];
				m_memory_allocator.deallocate(p);
				previous = NULL;
			}
			else				//p becomes "previous"
			{
				i_new[0] = p;
				memcpy(p,q,sizeof(Interval));

				p->next() = i_new[1];
				p->start() = start[0];
			}

			assert(!previous);

			for(unsigned j=1; j<N; ++j)
			{
				interval_pointer current_interval = i_new[j];

				if(map[j] == OLD)
				{
					memcpy(current_interval,&swap,sizeof(Interval));
				}
				else
				{
					memcpy(current_interval,q,sizeof(Interval));
				}

				if(j == N-1)
				{
					current_interval->next() = swap.next();
				}
				else
				{
					current_interval->next() = i_new[j+1];
				}

				current_interval->start() = start[j];
			}

			for(unsigned j=0; j<N; ++j)								//find "min" and add the intervals to the queue
			{
				if(j==N-1 && map[j]==NEW)
				{
					previous = i_new[j];
				}
				else
				{
					interval_pointer current_interval = i_new[j];

					current_interval->compute_min_distance(current_interval->stop());					//compute minimal distance

					if(map[j]==NEW || (map[j]==OLD && propagate_flag))
					{
						m_queue.insert(current_interval);
					}
				}
			}

			p = swap.next();
		}

		if(previous)		//close previous interval and put in into the queue
		{
			previous->compute_min_distance(previous->stop());
			m_queue.insert(previous);
			previous = NULL;
		}
	}
}

inline unsigned GeodesicAlgorithmExact::compute_propagated_parameters(double pseudo_x,
																		double pseudo_y,
																		double d,		//parameters of the interval
																		double begin,
																		double end,		//start/end of the interval
																		double alpha,	//corner angle
																		double L,		//length of the new edge
																		bool first_interval,		//if it is the first interval on the edge
																		bool last_interval,
																		bool turn_left,
																		bool turn_right,
																		IntervalWithStop* candidates)		//if it is the last interval on the edge
{
	assert(pseudo_y<=0.0);
	assert(d<GEODESIC_INF/10.0);
	assert(begin<=end);
	assert(first_interval ? (begin == 0.0) : true);

	IntervalWithStop* p = candidates;

	if(std::abs(pseudo_y) <= 1e-30)				//pseudo-source is on the edge
	{
		if(first_interval && pseudo_x <= 0.0)
		{
			p->start() = 0.0;
			p->stop() = L;
			p->d() = d - pseudo_x;
			p->pseudo_x() = 0.0;
			p->pseudo_y() = 0.0;
			return 1;
		}
		else if(last_interval && pseudo_x >= end)
		{
			p->start() = 0.0;
			p->stop() = L;
			p->d() = d + pseudo_x-end;
			p->pseudo_x() = end*cos(alpha);
			p->pseudo_y() = -end*sin(alpha);
			return 1;
		}
		else if(pseudo_x >= begin && pseudo_x <= end)
		{
			p->start() = 0.0;
			p->stop() = L;
			p->d() = d;
			p->pseudo_x() = pseudo_x*cos(alpha);
			p->pseudo_y() = -pseudo_x*sin(alpha);
			return 1;
		}
		else
		{
			return 0;
		}
	}

	double sin_alpha = sin(alpha);
	double cos_alpha = cos(alpha);

	//important: for the first_interval, this function returns zero only if the new edge is "visible" from the source
	//if the new edge can be covered only after turn_over, the value is negative (-1.0)
	double L1 = compute_positive_intersection(begin,
											  pseudo_x,
											  pseudo_y,
										      sin_alpha,
										      cos_alpha);

	if(L1 < 0 || L1 >= L)
	{
		if(first_interval && turn_left)
		{
			p->start() = 0.0;
			p->stop() = L;
			p->d() = d + sqrt(pseudo_x*pseudo_x + pseudo_y*pseudo_y);
			p->pseudo_y() = 0.0;
			p->pseudo_x() = 0.0;
			return 1;
		}
		else
		{
			return 0;
		}
	}

	double L2 = compute_positive_intersection(end,
											  pseudo_x,
											  pseudo_y,
											  sin_alpha,
											  cos_alpha);

	if(L2 < 0 || L2 >= L)
	{
		p->start() = L1;
		p->stop() = L;
		p->d() = d;
		p->pseudo_x() = cos_alpha*pseudo_x + sin_alpha*pseudo_y;
		p->pseudo_y() = -sin_alpha*pseudo_x + cos_alpha*pseudo_y;

		return 1;
	}

	p->start() = L1;
	p->stop() = L2;
	p->d() = d;
	p->pseudo_x() = cos_alpha*pseudo_x + sin_alpha*pseudo_y;
	p->pseudo_y() = -sin_alpha*pseudo_x + cos_alpha*pseudo_y;
	assert(p->pseudo_y() <= 0.0);

	if(!(last_interval && turn_right))
	{
		return 1;
	}
	else
	{
		p = candidates + 1;

		p->start() = L2;
		p->stop() = L;
		double dx = pseudo_x - end;
		p->d() = d + sqrt(dx*dx + pseudo_y*pseudo_y);
		p->pseudo_x() = end*cos_alpha;
		p->pseudo_y() = -end*sin_alpha;

		return 2;
	}
}

inline void GeodesicAlgorithmExact::construct_propagated_intervals(bool invert,
																	edge_pointer edge,
																	face_pointer face,		//constructs iNew from the rest of the data
																	IntervalWithStop* candidates,
																	unsigned& num_candidates,
																	interval_pointer source_interval)	//up to two candidates
{
	double edge_length = edge->length();
	double local_epsilon = SMALLEST_INTERVAL_RATIO * edge_length;

		//kill very small intervals in order to avoid precision problems
	if(num_candidates == 2)
	{
		double start = std::min(candidates->start(), (candidates+1)->start());
		double stop = std::max(candidates->stop(), (candidates+1)->stop());
		if(candidates->stop()-candidates->start() < local_epsilon) // kill interval 0
		{
			*candidates = *(candidates+1);
			num_candidates = 1;
			candidates->start() = start;
			candidates->stop() = stop;
		}
		else if ((candidates+1)->stop() - (candidates+1)->start() < local_epsilon)
		{
			num_candidates = 1;
			candidates->start() = start;
			candidates->stop() = stop;
		}
	}

	IntervalWithStop* first;
	IntervalWithStop* second;
	if(num_candidates == 1)
	{
		first = candidates;
		second = candidates;
	}
	else
	{
		if(candidates->start() <= (candidates+1)->start())
		{
			first = candidates;
			second = candidates+1;
		}
		else
		{
			first = candidates+1;
			second = candidates;
		}
		assert(first->stop() == second->start());
	}

	if(first->start() < local_epsilon)
	{
		first->start() = 0.0;
	}
	if(edge_length - second->stop() < local_epsilon)
	{
		second->stop() = edge_length;
	}

		//invert intervals if necessary; fill missing data and set pointers correctly
	Interval::DirectionType direction = edge->adjacent_faces()[0]->id() == face->id() ?
										Interval::FROM_FACE_0 :
										Interval::FROM_FACE_1;

	if(!invert)					//in this case everything is straighforward, we do not have to invert the intervals
	{
		for(unsigned i=0; i<num_candidates; ++i)
		{
			IntervalWithStop* p = candidates + i;

			p->next() = (i == num_candidates - 1) ? NULL : candidates + i + 1;
			p->edge() = edge;
			p->direction() = direction;
			p->source_index() = source_interval->source_index();

			p->min() = 0.0;					//it will be changed later on

			assert(p->start() < p->stop());
		}
	}
	else				//now we have to invert the intervals
	{
		for(unsigned i=0; i<num_candidates; ++i)
		{
			IntervalWithStop* p = candidates + i;

			p->next() = (i == 0) ? NULL : candidates + i - 1;
			p->edge() = edge;
			p->direction() = direction;
			p->source_index() = source_interval->source_index();

			double length = edge_length;
			p->pseudo_x() = length - p->pseudo_x();

			double start = length - p->stop();
			p->stop() = length - p->start();
			p->start() = start;

			p->min() = 0;

			assert(p->start() < p->stop());
			assert(p->start() >= 0.0);
			assert(p->stop() <= edge->length());
		}
	}
}


inline unsigned GeodesicAlgorithmExact::best_source(SurfacePoint& point,			//quickly find what source this point belongs to and what is the distance to this source
													   double& best_source_distance)
{
	double best_interval_position;
	unsigned best_source_index;

	best_first_interval(point,
						best_source_distance,
						best_interval_position,
						best_source_index);

	return best_source_index;
}

inline interval_pointer GeodesicAlgorithmExact::best_first_interval(SurfacePoint& point,
															 double& best_total_distance,
															 double& best_interval_position,
															 unsigned& best_source_index)
{
	assert(point.type() != UNDEFINED_POINT);

	interval_pointer best_interval = NULL;
	best_total_distance = GEODESIC_INF;

	if(point.type() == EDGE)
	{
		edge_pointer e = static_cast<edge_pointer>(point.base_element());
		list_pointer list = interval_list(e);

		best_interval_position = point.distance(e->v0());
		best_interval = list->covering_interval(best_interval_position);
		if(best_interval)
		{
			//assert(best_interval && best_interval->d() < GEODESIC_INF);
			best_total_distance = best_interval->signal(best_interval_position);
			best_source_index = best_interval->source_index();
		}
	}
	else if(point.type() == FACE)
	{
		face_pointer f = static_cast<face_pointer>(point.base_element());
		for(unsigned i=0; i<3; ++i)
		{
			edge_pointer e = f->adjacent_edges()[i];
			list_pointer list = interval_list(e);

			double offset;
			double distance;
			interval_pointer interval;

			list->find_closest_point(&point,
									 offset,
									 distance,
									 interval);

			if(interval && distance < best_total_distance)
			{
				best_interval = interval;
				best_total_distance = distance;
				best_interval_position = offset;
				best_source_index = interval->source_index();
			}
		}

			//check for all sources that might be located inside this face
		SortedSources::sorted_iterator_pair local_sources = m_sources.sources(f);
		for(SortedSources::sorted_iterator it=local_sources.first; it != local_sources.second; ++it)
		{
			SurfacePointWithIndex* source = *it;
			double distance = point.distance(source);
			if(distance < best_total_distance)
			{
				best_interval = NULL;
				best_total_distance = distance;
				best_interval_position = 0.0;
				best_source_index = source->index();
			}
		}
	}
	else if(point.type() == VERTEX)
	{
		vertex_pointer v = static_cast<vertex_pointer>(point.base_element());
		for(unsigned i=0; i<v->adjacent_edges().size(); ++i)
		{
			edge_pointer e = v->adjacent_edges()[i];
			list_pointer list = interval_list(e);

			double position = e->v0()->id() == v->id() ? 0.0 : e->length();
			interval_pointer interval = list->covering_interval(position);
			if(interval)
			{
				double distance = interval->signal(position);

				if(distance < best_total_distance)
				{
					best_interval = interval;
					best_total_distance = distance;
					best_interval_position = position;
					best_source_index = interval->source_index();
				}
			}
		}
	}

	if(best_total_distance > m_propagation_distance_stopped)		//result is unreliable
	{
		best_total_distance = GEODESIC_INF;
		return NULL;
	}
	else
	{
		return best_interval;
	}
}

inline void GeodesicAlgorithmExact::trace_back(SurfacePoint& destination,		//trace back piecewise-linear path
										std::vector<SurfacePoint>& path)
{
	path.clear();
	double best_total_distance;
	double best_interval_position;
	unsigned source_index = std::numeric_limits<unsigned>::max();
	interval_pointer best_interval = best_first_interval(destination,
														 best_total_distance,
														 best_interval_position,
														 source_index);

	if(best_total_distance >= GEODESIC_INF/2.0)		//unable to find the right path
	{
		return;
	}

	path.push_back(destination);

	if(best_interval)	//if we did not hit the face source immediately
	{
		std::vector<edge_pointer> possible_edges;
		possible_edges.reserve(10);

		while(visible_from_source(path.back()) < 0)		//while this point is not in the direct visibility of some source (if we are inside the FACE, we obviously hit the source)
		{
			SurfacePoint& q = path.back();

			possible_traceback_edges(q, possible_edges);

			interval_pointer interval;
			double total_distance;
			double position;

			best_point_on_the_edge_set(q,
									   possible_edges,
									   interval,
									   total_distance,
									   position);

			//std::cout << total_distance + length(path) << std::endl;
			assert(total_distance<GEODESIC_INF);
			source_index = interval->source_index();

			edge_pointer e = interval->edge();
			double local_epsilon = SMALLEST_INTERVAL_RATIO*e->length();
			if(position < local_epsilon)
			{
				path.push_back(SurfacePoint(e->v0()));
			}
			else if(position > e->length()-local_epsilon)
			{
				path.push_back(SurfacePoint(e->v1()));
			}
			else
			{
				double normalized_position = position/e->length();
				path.push_back(SurfacePoint(e, normalized_position));
			}
		}
	}

	SurfacePoint& source = static_cast<SurfacePoint&>(m_sources[source_index]);
	if(path.back().distance(&source) > 0)
	{
		path.push_back(source);
	}
}

inline void GeodesicAlgorithmExact::print_statistics()
{
	GeodesicAlgorithmBase::print_statistics();

	unsigned interval_counter = 0;
	for(unsigned i=0; i<m_edge_interval_lists.size(); ++i)
	{
		interval_counter += m_edge_interval_lists[i].number_of_intervals();
	}
	double intervals_per_edge = (double)interval_counter/(double)m_edge_interval_lists.size();

	double memory = m_edge_interval_lists.size()*sizeof(IntervalList) +
					interval_counter*sizeof(Interval);

	std::cout << "uses about " << memory/1e6 << "Mb of memory" <<std::endl;
	std::cout << interval_counter << " total intervals, or "
			  << intervals_per_edge << " intervals per edge"
			  << std::endl;
	std::cout << "maximum interval queue size is " << m_queue_max_size << std::endl;
	std::cout << "number of interval propagations is " << m_iterations << std::endl;
}

}		//geodesic
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedVS,
  typename DerivedFS,
  typename DerivedVT,
  typename DerivedFT,
  typename DerivedD>
IGL_INLINE void igl::exact_geodesic(
  const Eigen::MatrixBase<DerivedV> &V,
  const Eigen::MatrixBase<DerivedF> &F,
  const Eigen::MatrixBase<DerivedVS> &VS,
  const Eigen::MatrixBase<DerivedFS> &FS,
  const Eigen::MatrixBase<DerivedVT> &VT,
  const Eigen::MatrixBase<DerivedFT> &FT,
  Eigen::PlainObjectBase<DerivedD> &D)
{
  assert((V.cols() == 3 || V.cols() == 2) && F.cols() == 3 && "Only support 2D/3D triangle mesh");
  std::vector<typename DerivedV::Scalar> points(V.rows() * 3);
  std::vector<typename DerivedF::Scalar> faces(F.rows() * F.cols());
  for (int i = 0; i < points.size(); i++)
  {
    // Append 0s for 2D input
    points[i] = ((i%3)<2 || V.cols()==3) ? V(i / 3, i % 3) : 0.0;
  }
  for (int i = 0; i < faces.size(); i++)
  {
    faces[i] = F(i / 3, i % 3);
  }

  igl::geodesic::Mesh mesh;
  mesh.initialize_mesh_data(points, faces);
  igl::geodesic::GeodesicAlgorithmExact exact_algorithm(&mesh);

  std::vector<igl::geodesic::SurfacePoint> source;
  source.reserve(VS.rows() + FS.rows());

  // Vertex sources
  for(int i = 0;i < VS.rows(); i++)
  {
    for(int j = 0;j < VS.cols(); j++)
    {
      source.emplace_back(&mesh.vertices()[VS(i, j)]);
    }
  }
  // Face Sources
  for(int i = 0;i < FS.rows(); i++)
  {
    for(int j = 0;j < FS.cols(); j++)
    {
      source.emplace_back(&mesh.faces()[FS(i, j)]);
    }
  }
  std::vector<igl::geodesic::SurfacePoint> target;
  target.reserve(VT.rows() + FT.rows());
  //Vertex targets
  for(int i = 0;i < VT.rows(); i++)
  {
    for(int j = 0;j < VT.cols(); j++)
    {
      target.emplace_back(&mesh.vertices()[VT(i, j)]);
    }
  }
  // Face targets
  for(int i = 0;i < FT.rows(); i++)
  {
    for(int j = 0;j < FT.cols(); j++)
    {
      target.emplace_back(&mesh.faces()[FT(i, j)]);
    }
  }

  exact_algorithm.propagate(source);
  std::vector<igl::geodesic::SurfacePoint> path;
  D.resize(target.size());
  for (int i = 0; i < target.size(); i++)
  {
    exact_algorithm.trace_back(target[i], path);
    D(i) = igl::geodesic::length(path);
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::exact_geodesic<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1>> &);
#endif
