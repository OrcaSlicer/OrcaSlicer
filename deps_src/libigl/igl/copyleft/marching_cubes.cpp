/*===========================================================================*\
 *                                                                           *
 *                                IsoEx                                      *
 *        Copyright (C) 2002 by Computer Graphics Group, RWTH Aachen         *
 *                         www.rwth-graphics.de                              *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *                                                                           *
 *                                License                                    *
 *                                                                           *
 *  This library is free software; you can redistribute it and/or modify it  *
 *  under the terms of the GNU Library General Public License as published   *
 *  by the Free Software Foundation, version 2.                              *
 *                                                                           *
 *  This library is distributed in the hope that it will be useful, but      *
 *  WITHOUT ANY WARRANTY; without even the implied warranty of               *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU        *
 *  Library General Public License for more details.                         *
 *                                                                           *
 *  You should have received a copy of the GNU Library General Public        *
 *  License along with this library; if not, write to the Free Software      *
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.                *
 *                                                                           *
 \*===========================================================================*/

#include "marching_cubes.h"
#include "marching_cubes_tables.h"

#include <unordered_map>


extern const int edgeTable[256];
extern const int triTable[256][2][17];
extern const int polyTable[8][16];


template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedIndices, typename DerivedFaces>
class MarchingCubes
{
  struct EdgeKey
  {
    EdgeKey(unsigned i0, unsigned i1) : i0_(i0), i1_(i1) {}

    bool operator==(const EdgeKey& _rhs) const
    {
      return i0_ == _rhs.i0_ && i1_ == _rhs.i1_;
    }

    unsigned i0_, i1_;
  };

  struct EdgeHash
  {
    std::size_t operator()(const EdgeKey& key) const {
      std::size_t seed = 0;
      seed ^= key.i0_ + 0x9e3779b9 + (seed<<6) + (seed>>2); // Copied from boost::hash_combine
      seed ^= key.i1_ + 0x9e3779b9 + (seed<<6) + (seed>>2);
      return std::hash<std::size_t>()(seed);
    }
  };

  typedef std::unordered_map<EdgeKey, unsigned, EdgeHash> MyMap;
  typedef typename MyMap::const_iterator                  MyMapIterator;

public:
  // Dense index grid version
  MarchingCubes(const Eigen::MatrixBase<DerivedValues> &values,
                const Eigen::MatrixBase<DerivedPoints> &points,
                const unsigned x_res,
                const unsigned y_res,
                const unsigned z_res,
                const double isovalue,
                Eigen::PlainObjectBase<DerivedVertices> &vertices,
                Eigen::PlainObjectBase<DerivedFaces> &faces)
  {
    assert(values.cols() == 1);
    assert(points.cols() == 3);

    if(x_res <2 || y_res<2 ||z_res<2)
      return;
    faces.resize(10000,3);
    int num_faces = 0;

    vertices.resize(10000,3);
    int num_vertices = 0;

    unsigned n_cubes  = (x_res-1) * (y_res-1) * (z_res-1);
    assert(unsigned(points.rows()) == x_res * y_res * z_res);

    unsigned int         offsets_[8];
    offsets_[0] = 0;
    offsets_[1] = 1;
    offsets_[2] = 1 + x_res;
    offsets_[3] =     x_res;
    offsets_[4] =             x_res*y_res;
    offsets_[5] = 1         + x_res*y_res;
    offsets_[6] = 1 + x_res + x_res*y_res;
    offsets_[7] =     x_res + x_res*y_res;

    for (unsigned cube_it =0 ; cube_it < n_cubes; ++cube_it)
    {
      unsigned         corner[8];
      // get point indices of corner vertices
      for (int i=0; i<8; ++i)
      {
        // get cube coordinates
        unsigned int _idx = cube_it;
        unsigned int X(x_res-1), Y(y_res-1);
        unsigned int x = _idx % X;  _idx /= X;
        unsigned int y = _idx % Y;  _idx /= Y;
        unsigned int z = _idx;
        // transform to point coordinates
        _idx = x + y*x_res + z*x_res*y_res;
        // add offset
        corner[i] = _idx + offsets_[i];
      }
      add_cube(values,points,isovalue,corner,vertices,num_vertices,faces,num_faces,edge2vertex);
    }
    vertices.conservativeResize(num_vertices, Eigen::NoChange);
    faces.conservativeResize(num_faces, Eigen::NoChange);
  }

  // Sparse index grid version
  MarchingCubes(const Eigen::MatrixBase<DerivedValues> &values,
                const Eigen::MatrixBase<DerivedPoints> &points,
                const Eigen::MatrixBase<DerivedIndices> &cubes,
                const double isovalue,
                Eigen::PlainObjectBase<DerivedVertices> &vertices,
                Eigen::PlainObjectBase<DerivedFaces> &faces)
  {
    assert(values.cols() == 1);
    assert(points.cols() == 3);
    assert(cubes.cols() == 8);

    if(cubes.rows() == 0)
    {
      return;
    }

    faces.resize(10000,3);
    int num_faces = 0;

    vertices.resize(10000,3);
    int num_vertices = 0;


    unsigned n_cubes  = cubes.rows();

    for (unsigned cube_it =0 ; cube_it < n_cubes; ++cube_it)
    {
      typedef Eigen::Matrix<typename DerivedIndices::Scalar, 1, 8> CubeIndexVector;

      CubeIndexVector  corner = cubes.row(cube_it);
      add_cube(values,points,isovalue,corner.data(),vertices,num_vertices,faces,num_faces,edge2vertex);
    }
    vertices.conservativeResize(num_vertices, Eigen::NoChange);
    faces.conservativeResize(num_faces, Eigen::NoChange);
  }

  // Dense index grid function version
  template <typename DerivedValue, typename DerivedPoint>
  MarchingCubes(
    const std::function< DerivedValue(const DerivedPoint & ) > & value_fun,
    const Eigen::MatrixBase<DerivedPoints> &points,
    const unsigned x_res,
    const unsigned y_res,
    const unsigned z_res,
    const double isovalue,
    Eigen::PlainObjectBase<DerivedVertices> &vertices,
    Eigen::PlainObjectBase<DerivedFaces> &faces)
  {
    assert(points.cols() == 3);

    if(x_res <2 || y_res<2 ||z_res<2)
      return;
    faces.resize(10000,3);
    int num_faces = 0;

    vertices.resize(10000,3);
    int num_vertices = 0;

    unsigned n_cubes  = (x_res-1) * (y_res-1) * (z_res-1);
    assert(unsigned(points.rows()) == x_res * y_res * z_res);

    unsigned int         offsets_[8];
    offsets_[0] = 0;
    offsets_[1] = 1;
    offsets_[2] = 1 + x_res;
    offsets_[3] =     x_res;
    offsets_[4] =             x_res*y_res;
    offsets_[5] = 1         + x_res*y_res;
    offsets_[6] = 1 + x_res + x_res*y_res;
    offsets_[7] =     x_res + x_res*y_res;

    for (unsigned cube_it =0 ; cube_it < n_cubes; ++cube_it)
    {
      unsigned         corner[8];
      // get point indices of corner vertices
      for (int i=0; i<8; ++i)
      {
        // get cube coordinates
        unsigned int _idx = cube_it;
        unsigned int X(x_res-1), Y(y_res-1);
        unsigned int x = _idx % X;  _idx /= X;
        unsigned int y = _idx % Y;  _idx /= Y;
        unsigned int z = _idx;
        // transform to point coordinates
        _idx = x + y*x_res + z*x_res*y_res;
        // add offset
        corner[i] = _idx + offsets_[i];
      }
      add_cube(value_fun,points,isovalue,corner,vertices,num_vertices,faces,num_faces,edge2vertex);
    }
    vertices.conservativeResize(num_vertices, Eigen::NoChange);
    faces.conservativeResize(num_faces, Eigen::NoChange);
  }

  template <typename CubeIndexType>
  static void add_cube(
      const Eigen::MatrixBase<DerivedValues> &values,
      const Eigen::MatrixBase<DerivedPoints> &points,
      const double isovalue,
      const CubeIndexType corner[],
      Eigen::PlainObjectBase<DerivedVertices> &vertices,
      int &num_vertices,
      Eigen::PlainObjectBase<DerivedFaces> &faces,
      int & num_faces,
      MyMap &edge2vertex)
  {
    typedef typename DerivedFaces::Scalar SampleScalar;
    SampleScalar     samples[12];
    unsigned char    cubetype(0);

    // determine cube type
    for (int i=0; i<8; ++i)
    {
      if (values(corner[i]) > isovalue)
      {
        cubetype |= (1<<i);
      }
    }

    // trivial reject ?
    if (cubetype == 0 || cubetype == 255)
    {
      return;
    }

    // compute samples on cube's edges
    if (edgeTable[cubetype]&1)
      samples[0]  = add_vertex(values, points, isovalue, corner[0], corner[1], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&2)
      samples[1]  = add_vertex(values, points, isovalue, corner[1], corner[2], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&4)
      samples[2]  = add_vertex(values, points, isovalue, corner[3], corner[2], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&8)
      samples[3]  = add_vertex(values, points, isovalue, corner[0], corner[3], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&16)
      samples[4]  = add_vertex(values, points, isovalue, corner[4], corner[5], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&32)
      samples[5]  = add_vertex(values, points, isovalue, corner[5], corner[6], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&64)
      samples[6]  = add_vertex(values, points, isovalue, corner[7], corner[6], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&128)
      samples[7]  = add_vertex(values, points, isovalue, corner[4], corner[7], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&256)
      samples[8]  = add_vertex(values, points, isovalue, corner[0], corner[4], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&512)
      samples[9]  = add_vertex(values, points, isovalue, corner[1], corner[5], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&1024)
      samples[10] = add_vertex(values, points, isovalue, corner[2], corner[6], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&2048)
      samples[11] = add_vertex(values, points, isovalue, corner[3], corner[7], vertices, num_vertices, edge2vertex);

    // connect samples by triangles
    for (int i=0; triTable[cubetype][0][i] != -1; i+=3 )
    {
      num_faces++;
      if (num_faces > faces.rows())
      {
        faces.conservativeResize(faces.rows()+10000, Eigen::NoChange);
      }
      faces.row(num_faces-1) <<
        samples[triTable[cubetype][0][i  ]],
        samples[triTable[cubetype][0][i+1]],
        samples[triTable[cubetype][0][i+2]];
    }
  };

  template <typename DerivedValue, typename DerivedPoint, typename CubeIndexType>
  static void add_cube(
      const std::function< DerivedValue(const DerivedPoint & ) > & value_fun,
      const Eigen::MatrixBase<DerivedPoints> &points,
      const double isovalue,
      const CubeIndexType corner[],
      Eigen::PlainObjectBase<DerivedVertices> &vertices,
      int &num_vertices,
      Eigen::PlainObjectBase<DerivedFaces> &faces,
      int & num_faces,
      MyMap &edge2vertex)
  {
    typedef typename DerivedFaces::Scalar SampleScalar;
    SampleScalar     samples[12];
    unsigned char    cubetype(0);

    // determine cube type
    for (int i=0; i<8; ++i)
    {
      if (value_fun(points.row(corner[i])) > isovalue)
      {
        cubetype |= (1<<i);
      }
    }

    // trivial reject ?
    if (cubetype == 0 || cubetype == 255)
    {
      return;
    }

    // compute samples on cube's edges
    if (edgeTable[cubetype]&1)
      samples[0]  = add_vertex(value_fun, points, isovalue, corner[0], corner[1], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&2)
      samples[1]  = add_vertex(value_fun, points, isovalue, corner[1], corner[2], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&4)
      samples[2]  = add_vertex(value_fun, points, isovalue, corner[3], corner[2], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&8)
      samples[3]  = add_vertex(value_fun, points, isovalue, corner[0], corner[3], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&16)
      samples[4]  = add_vertex(value_fun, points, isovalue, corner[4], corner[5], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&32)
      samples[5]  = add_vertex(value_fun, points, isovalue, corner[5], corner[6], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&64)
      samples[6]  = add_vertex(value_fun, points, isovalue, corner[7], corner[6], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&128)
      samples[7]  = add_vertex(value_fun, points, isovalue, corner[4], corner[7], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&256)
      samples[8]  = add_vertex(value_fun, points, isovalue, corner[0], corner[4], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&512)
      samples[9]  = add_vertex(value_fun, points, isovalue, corner[1], corner[5], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&1024)
      samples[10] = add_vertex(value_fun, points, isovalue, corner[2], corner[6], vertices, num_vertices, edge2vertex);
    if (edgeTable[cubetype]&2048)
      samples[11] = add_vertex(value_fun, points, isovalue, corner[3], corner[7], vertices, num_vertices, edge2vertex);

    // connect samples by triangles
    for (int i=0; triTable[cubetype][0][i] != -1; i+=3 )
    {
      num_faces++;
      if (num_faces > faces.rows())
      {
        faces.conservativeResize(faces.rows()+10000, Eigen::NoChange);
      }
      faces.row(num_faces-1) <<
        samples[triTable[cubetype][0][i  ]],
        samples[triTable[cubetype][0][i+1]],
        samples[triTable[cubetype][0][i+2]];
    }
  };

  static typename DerivedFaces::Scalar add_vertex(
    const Eigen::MatrixBase<DerivedValues> &values,
    const Eigen::MatrixBase<DerivedPoints> &points,
    const double isovalue, 
    unsigned int i0,
    unsigned int i1,
    Eigen::PlainObjectBase<DerivedVertices> &vertices,
    int &num_vertices,
    MyMap &edge2vertex)
  {
    // find vertex if it has been computed already
    MyMapIterator it = edge2vertex.find(EdgeKey(i0, i1));
    if (it != edge2vertex.end())
    {
      return it->second;
    }

    // generate new vertex
    const Eigen::Matrix<typename DerivedPoints::Scalar, 1, 3> & p0 = points.row(i0);
    const Eigen::Matrix<typename DerivedPoints::Scalar, 1, 3> & p1 = points.row(i1);
    typename DerivedValues::Scalar s0 = fabs(values(i0)-isovalue);
    typename DerivedValues::Scalar s1 = fabs(values(i1)-isovalue);
    typename DerivedValues::Scalar t  = s0 / (s0+s1);
    num_vertices++;
    if (num_vertices > vertices.rows())
    {
      vertices.conservativeResize(vertices.rows()+10000, Eigen::NoChange);
    }
    //
    // Linear interpolation based on linearly interpolating values
    vertices.row(num_vertices-1)  = ((1.0f-t)*p0 + t*p1).template cast<typename DerivedVertices::Scalar>();
    edge2vertex[EdgeKey(i0, i1)] = num_vertices-1;
    return num_vertices-1;
  }

  template <typename DerivedValue, typename DerivedPoint>
  static typename DerivedFaces::Scalar add_vertex(
    const std::function< DerivedValue(const DerivedPoint & ) > & value_fun,
    const Eigen::MatrixBase<DerivedPoints> &points,
    const double isovalue, 
    unsigned int i0,
    unsigned int i1,
    Eigen::PlainObjectBase<DerivedVertices> &vertices,
    int &num_vertices,
    MyMap &edge2vertex)
  {
    // find vertex if it has been computed already
    MyMapIterator it = edge2vertex.find(EdgeKey(i0, i1));
    if (it != edge2vertex.end())
    {
      return it->second;
    }
    num_vertices++;
    if (num_vertices > vertices.rows())
    {
      vertices.conservativeResize(vertices.rows()+10000, Eigen::NoChange);
    }

    // generate new vertex
    typedef Eigen::Matrix<typename DerivedPoints::Scalar, 1, 3> RowVector3S;
    const RowVector3S & p0 = points.row(i0);
    const RowVector3S & p1 = points.row(i1);
    // Linear interpolation based on linearly interpolating values
    //typename DerivedValues::Scalar s0 = fabs(value_fun(points.row(i0))-isovalue);
    //typename DerivedValues::Scalar s1 = fabs(value_fun(points.row(i1))-isovalue);
    //typename DerivedValues::Scalar t  = s0 / (s0+s1);
    const DerivedValue v1 = value_fun(p1);
    double t0 = (v1>isovalue)?0:1;
    double t1 = (v1>isovalue)?1:0;
    double t;
    for(int j = 0;j<10;j++)
    {
      t = 0.5*(t0+t1);
      const double val = value_fun( ((1.0f-t)*p0 + t*p1) );
      if( val > isovalue )
      {
        t1 = t;
      }else
      {
        t0 = t;
      }
    }
    vertices.row(num_vertices-1)  = ((1.0f-t)*p0 + t*p1).template cast<typename DerivedVertices::Scalar>();

    edge2vertex[EdgeKey(i0, i1)] = num_vertices-1;
    return num_vertices-1;
  }

  // maps an edge to the sample vertex generated on it
  MyMap  edge2vertex;
};


template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedFaces>
IGL_INLINE void igl::copyleft::marching_cubes(
  const Eigen::MatrixBase<DerivedValues> &values,
  const Eigen::MatrixBase<DerivedPoints> &points,
  const unsigned x_res,
  const unsigned y_res,
  const unsigned z_res,
  const double isovalue,
  Eigen::PlainObjectBase<DerivedVertices> &vertices,
  Eigen::PlainObjectBase<DerivedFaces> &faces)
{
  typedef Eigen::MatrixXi Shim; /* DerivedIndices shim type is unused in this instantiation*/
  MarchingCubes<DerivedValues, DerivedPoints, DerivedVertices, Shim, DerivedFaces>
          mc(values, points, x_res, y_res, z_res, isovalue, vertices, faces);
}

template <
  typename DerivedValue, 
  typename DerivedPoint,
  typename DerivedPoints, 
  typename DerivedVertices, 
  typename DerivedFaces>
IGL_INLINE void igl::copyleft::marching_cubes(
    const std::function< DerivedValue(const DerivedPoint & ) > & value_fun,
    const Eigen::MatrixBase<DerivedPoints> &points,
    const unsigned x_res,
    const unsigned y_res,
    const unsigned z_res,
    const double isovalue, 
    Eigen::PlainObjectBase<DerivedVertices> &vertices,
    Eigen::PlainObjectBase<DerivedFaces> &faces)
{
  MarchingCubes<
    Eigen::Matrix<DerivedValue,Eigen::Dynamic,1>, /* unnecessary */
    DerivedPoints, DerivedVertices, 
    Eigen::Matrix<int,Eigen::Dynamic,1>, /* unnecessary */
    DerivedFaces> mc(value_fun, points, x_res, y_res, z_res, isovalue, vertices, faces);
}

template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedFaces>
IGL_INLINE void igl::copyleft::marching_cubes(
  const Eigen::MatrixBase<DerivedValues> &values,
  const Eigen::MatrixBase<DerivedPoints> &points,
  const unsigned x_res,
  const unsigned y_res,
  const unsigned z_res,
  Eigen::PlainObjectBase<DerivedVertices> &vertices,
  Eigen::PlainObjectBase<DerivedFaces> &faces)
{
  typedef Eigen::MatrixXi Shim; /* DerivedIndices shim type is unused in this instantiation*/
  MarchingCubes<DerivedValues, DerivedPoints, DerivedVertices, Shim, DerivedFaces>
          mc(values, points, x_res, y_res, z_res, 0.0 /*isovalue*/, vertices, faces);
}

template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedIndices, typename DerivedFaces>
IGL_INLINE void igl::copyleft::marching_cubes(
  const Eigen::MatrixBase<DerivedValues>& values,
  const Eigen::MatrixBase<DerivedPoints>& points,
  const Eigen::MatrixBase<DerivedIndices>& indices,
  const double isovalue,
  Eigen::PlainObjectBase<DerivedVertices>& vertices,
  Eigen::PlainObjectBase<DerivedFaces> &faces)
{
  MarchingCubes<DerivedValues, DerivedPoints, DerivedVertices, DerivedIndices, DerivedFaces> mc(values, points, indices, isovalue, vertices, faces);
}

template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedIndices, typename DerivedFaces>
IGL_INLINE void igl::copyleft::marching_cubes(
  const Eigen::MatrixBase<DerivedValues> &values,
  const Eigen::MatrixBase<DerivedPoints> &points,
  const Eigen::MatrixBase<DerivedIndices> & indices,
  Eigen::PlainObjectBase<DerivedVertices> &vertices,
  Eigen::PlainObjectBase<DerivedFaces> &faces)
{
  MarchingCubes<DerivedValues, DerivedPoints, DerivedVertices, DerivedIndices, DerivedFaces> mc(values, points, indices, 0.0 /*isovalue*/, vertices, faces);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, unsigned int, unsigned int, unsigned int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<float, -1, 1, 0, -1, 1>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, unsigned int, unsigned int, unsigned int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<float, -1, 1, 0, -1, 1>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, unsigned int, unsigned int, unsigned int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<float, -1, 1, 0, -1, 1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, unsigned int, unsigned int, unsigned int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, unsigned int, unsigned int, unsigned int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, unsigned int, unsigned int, unsigned int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, const Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, const Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, const Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, const Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, const Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, const Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, const Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, const Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 8, 0, -1, 8>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, const Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, const Eigen::MatrixBase<Eigen::Matrix<int, -1, 8, 0, -1, 8> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>,Eigen::Matrix<double, -1, -1, 0, -1, -1>,Eigen::Matrix<double, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, 3, 0, -1, 3> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&,const Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&,const Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&,Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&,Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>,Eigen::Matrix<double, -1, -1, 0, -1, -1>,Eigen::Matrix<double, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, 8, 0, -1, 8>,Eigen::Matrix<int, -1, 3, 0, -1, 3> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&,const Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&,const Eigen::MatrixBase<Eigen::Matrix<int, -1, 8, 0, -1, 8> >&,Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&,Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>,Eigen::Matrix<double, -1, 3, 0, -1, 3>,Eigen::Matrix<double, -1, 3, 0, -1, 3>,Eigen::Matrix<int, -1, 8, 0, -1, 8>,Eigen::Matrix<int, -1, 3, 0, -1, 3> >(const Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&,const Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&,const Eigen::MatrixBase<Eigen::Matrix<int, -1, 8, 0, -1, 8> >&,Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&,Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::marching_cubes<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, unsigned int, unsigned int, unsigned int, double, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
