#include "readPLY.h"
#include <string>
#include <set>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <Eigen/Core>

#include "tinyply.h"
#include "read_file_binary.h"
#include "FileMemoryStream.h"


namespace igl
{

template <typename T, typename Derived>
IGL_INLINE bool _tinyply_buffer_to_matrix(
  tinyply::PlyData & D,
  Eigen::PlainObjectBase<Derived> & M,
  size_t rows,
  size_t cols )
{
  Eigen::Map< Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> >
    _map( reinterpret_cast<T *>( D.buffer.get()), rows, cols );

  M = _map.template cast<typename Derived::Scalar>();
  return true;
}



template <typename Derived>
IGL_INLINE bool tinyply_buffer_to_matrix(
  tinyply::PlyData & D,
  Eigen::PlainObjectBase<Derived> & M,
  size_t rows,
  size_t cols )
{
  switch(D.t)
  {
    case tinyply::Type::INT8 :
      return   _tinyply_buffer_to_matrix<std::int8_t,Derived>(D, M,rows,cols);
    case tinyply::Type::UINT8 :
      return   _tinyply_buffer_to_matrix<std::uint8_t,Derived>(D, M,rows,cols);
    case tinyply::Type::INT16 :
      return   _tinyply_buffer_to_matrix<std::int16_t,Derived>(D, M,rows,cols);
    case tinyply::Type::UINT16 :
      return   _tinyply_buffer_to_matrix<std::uint16_t,Derived>(D, M,rows,cols);
    case tinyply::Type::INT32 :
      return   _tinyply_buffer_to_matrix<std::int32_t,Derived>(D, M,rows,cols);
    case tinyply::Type::UINT32 :
      return   _tinyply_buffer_to_matrix<std::uint32_t,Derived>(D, M,rows,cols);
    case tinyply::Type::FLOAT32 :
      return   _tinyply_buffer_to_matrix<float,Derived>(D, M,rows,cols);
    case tinyply::Type::FLOAT64 :
      return   _tinyply_buffer_to_matrix<double,Derived>(D, M,rows,cols);
    default:
      return false;
  }
}


template <typename T, typename Derived>
IGL_INLINE bool _tinyply_tristrips_to_trifaces(
  tinyply::PlyData & D,
  Eigen::PlainObjectBase<Derived> & M,
  size_t el,
  size_t el_len )
{

  Eigen::Map< Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> >
     _map( reinterpret_cast<T *>( D.buffer.get()), el, el_len );

  // to make it more interesting, triangles in triangle strip can be separated by negative index elements
  // 1. count all triangles
  size_t triangles=0;

  // TODO: it's possible to optimize this , i suppose
  for(size_t i=0; i<el; i++)
    for(size_t j=0; j<(el_len-2); j++)
    {
      if(_map(i,j)>=0 && _map(i,j+1)>=0 && _map(i,j+2)>=0)
        triangles++;
    }

  // 2. convert triangles to faces, skipping over the negative indeces, indicating separate strips
  M.resize(triangles, 3);
  size_t k=0;
  for(size_t i=0; i<el; i++)
  {
    int flip=0;
    for(size_t j=0; j<(el_len-2); j++)
    {
      if(_map(i,j)>=0 && _map(i,j+1)>=0 && _map(i,j+2)>=0)
      {
        // consequtive faces on the same strip have to be flip-flopped, to preserve orientation
        M( k,0 ) = static_cast<typename Derived::Scalar>( _map(i, j ) );
        M( k,1 ) = static_cast<typename Derived::Scalar>( _map(i, j+1+flip ) );
        M( k,2 ) = static_cast<typename Derived::Scalar>( _map(i, j+1+(flip^1) ) );
        k++;
        flip ^= 1;
      } else {
        // reset flip on new strip start
        flip = 0;
      }
    }
  }
  assert(k==triangles);
  return true;
}

template <typename Derived>
IGL_INLINE bool tinyply_tristrips_to_faces(
  tinyply::PlyData & D,
  Eigen::PlainObjectBase<Derived> & M,
  size_t el,
  size_t el_len )
{
  switch(D.t)
  {
    case tinyply::Type::INT8 :
      return   _tinyply_tristrips_to_trifaces<std::int8_t,Derived>(D, M,el,el_len);
    case tinyply::Type::UINT8 :
      return   _tinyply_tristrips_to_trifaces<std::uint8_t,Derived>(D, M,el,el_len);
    case tinyply::Type::INT16 :
      return   _tinyply_tristrips_to_trifaces<std::int16_t,Derived>(D, M,el,el_len);
    case tinyply::Type::UINT16 :
      return   _tinyply_tristrips_to_trifaces<std::uint16_t,Derived>(D, M,el,el_len);
    case tinyply::Type::INT32 :
      return   _tinyply_tristrips_to_trifaces<std::int32_t,Derived>(D, M,el,el_len);
    case tinyply::Type::UINT32 :
      return   _tinyply_tristrips_to_trifaces<std::uint32_t,Derived>(D, M,el,el_len);
    case tinyply::Type::FLOAT32 :
      return   _tinyply_tristrips_to_trifaces<float,Derived>(D, M,el,el_len);
    case tinyply::Type::FLOAT64 :
      return   _tinyply_tristrips_to_trifaces<double,Derived>(D, M,el,el_len);
    default:
      return false;
  }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV,
  typename DerivedVD,
  typename DerivedFD,
  typename DerivedED
  >
IGL_INLINE bool readPLY(
  FILE *fp,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV,

  Eigen::PlainObjectBase<DerivedVD> & VD,
  std::vector<std::string> & Vheader,
  Eigen::PlainObjectBase<DerivedFD> & FD,
  std::vector<std::string> & Fheader,
  Eigen::PlainObjectBase<DerivedED> & ED,
  std::vector<std::string> & Eheader,
  std::vector<std::string> & comments
  )
{
  // buffer the whole file in memory
  // then read from memory buffer
  try
  {
    std::vector<std::uint8_t> fileBufferBytes;
    // read_file_binary will call fclose
    read_file_binary(fp,fileBufferBytes);
    FileMemoryStream stream((char*)fileBufferBytes.data(), fileBufferBytes.size());
    return readPLY(stream,V,F,E,N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
  }
  catch(const std::exception& e)
  {
    std::cerr << "ReadPLY error: " << e.what() << std::endl;
  }
  fclose(fp);
  return false;
}



template <
  typename DerivedV,
  typename DerivedF
  >
IGL_INLINE bool readPLY(
  FILE *fp,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F
  )
{
  Eigen::MatrixXd N,UV,VD,FD,ED;
  Eigen::MatrixXi E;
  std::vector<std::string> Vheader,Eheader,Fheader,comments;
  return readPLY(fp,V,F,E,N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE
  >
IGL_INLINE bool readPLY(
  FILE *fp,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E
  )
{
  Eigen::MatrixXd N,UV,VD,FD,ED;
  std::vector<std::string> Vheader,Eheader,Fheader,comments;
  return readPLY(fp,V,F,E,N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}


template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV,
  typename DerivedVD,
  typename DerivedFD,
  typename DerivedED
  >
IGL_INLINE bool readPLY(
  std::istream & ply_stream,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV,

  Eigen::PlainObjectBase<DerivedVD> & VD,
  std::vector<std::string> & Vheader,
  Eigen::PlainObjectBase<DerivedFD> & FD,
  std::vector<std::string> & Fheader,
  Eigen::PlainObjectBase<DerivedED> & ED,
  std::vector<std::string> & Eheader,
  std::vector<std::string> & comments
  )
{
  tinyply::PlyFile file;
  file.parse_header(ply_stream);

  std::set<std::string> vertex_std{ "x","y","z", "nx","ny","nz",  "u","v",  "texture_u", "texture_v", "s", "t"};
  std::set<std::string> face_std  { "vertex_index", "vertex_indices"};
  std::set<std::string> edge_std  { "vertex1", "vertex2"}; //non-standard edge indexes

  // Tinyply treats parsed data as untyped byte buffers.
  std::shared_ptr<tinyply::PlyData> vertices, normals, faces, texcoords, edges;

  // Some ply files contain tristrips instead of faces
  std::shared_ptr<tinyply::PlyData> tristrips;

  std::shared_ptr<tinyply::PlyData> _vertex_data;
  std::vector<std::string> _vertex_header;

  std::shared_ptr<tinyply::PlyData> _face_data;
  std::vector<std::string> _face_header;

  std::shared_ptr<tinyply::PlyData> _edge_data;
  std::vector<std::string> _edge_header;

  for (auto c : file.get_comments())
      comments.push_back(c);

  for (auto e : file.get_elements())
  {
      if(e.name == "vertex" ) // found a vertex
      {
        for (auto p : e.properties)
        {
          if(vertex_std.find(p.name) == vertex_std.end())
          {
              _vertex_header.push_back(p.name);
          }
        }
      }
      else if(e.name == "face" ) // found face
      {
        for (auto p : e.properties)
        {
          if(face_std.find(p.name) == face_std.end())
          {
              _face_header.push_back(p.name);
          }
        }
      }
      else if(e.name == "edge" ) // found edge
      {
        for (auto p : e.properties)
        {
          if(edge_std.find(p.name) == edge_std.end())
          {
              _edge_header.push_back(p.name);
          }
        }
      }
      // skip the unknown entries
  }

  // The header information can be used to programmatically extract properties on elements
  // known to exist in the header prior to reading the data. For brevity of this sample, properties
  // like vertex position are hard-coded:
  try {
    vertices = file.request_properties_from_element("vertex", { "x", "y", "z" });
  }
  catch (const std::exception & ) { }

  try {
    normals = file.request_properties_from_element("vertex", { "nx", "ny", "nz" });
  }
  catch (const std::exception & ) { }

  //Try texture coordinates with several names
  try {
    //texture_u texture_v are the names used by meshlab to store textures
    texcoords = file.request_properties_from_element("vertex", { "texture_u", "texture_v" });
  }
  catch (const std::exception & ) { }
  if (!texcoords)
  {
      try {
        //u v are the naive names
        texcoords = file.request_properties_from_element("vertex", { "u", "v" });
      }
      catch (const std::exception & ) { }

  }
  if (!texcoords)
  {
      try {
        //s t were the names used by blender and the previous libigl PLY reader.
        texcoords = file.request_properties_from_element("vertex", { "s", "t" });
      }
      catch (const std::exception & ) { }

  }

  // Providing a list size hint (the last argument) is a 2x performance improvement. If you have
  // arbitrary ply files, it is best to leave this 0.
  try {
    faces = file.request_properties_from_element( "face", { "vertex_indices" }, 0);
  }
  catch (const std::exception & ) { }

  if (!faces)
  {
      try {
        // alternative name of the elements
        faces = file.request_properties_from_element( "face", { "vertex_index" },0);
      }
      catch (const std::exception & ) { }
  }

  if (!faces)
  {
      try {
        // try using tristrips
        tristrips = file.request_properties_from_element( "tristrips", { "vertex_indices" }, 0);
      }
      catch (const std::exception & ) { }

      if (!tristrips)
      {
          try {
            // alternative name of the elements
            tristrips = file.request_properties_from_element( "tristrips", { "vertex_index" }, 0);
          }
          catch (const std::exception & ) { }
      }
  }


  try {
    edges = file.request_properties_from_element("edge", { "vertex1", "vertex2" });
  }
  catch (const std::exception & ) { }

  if(! _vertex_header.empty())
    _vertex_data = file.request_properties_from_element( "vertex", _vertex_header);
  if(! _face_header.empty())
    _face_data = file.request_properties_from_element( "face", _face_header);
  if(! _edge_header.empty())
    _edge_data = file.request_properties_from_element( "edge", _edge_header);

  // Parse the geometry data
  file.read(ply_stream);

  if (!vertices || !tinyply_buffer_to_matrix(*vertices,V,vertices->count,3) ) {
    // Don't do this because V might have non-trivial compile-time size V.resize(0,0);
  }

  if (!normals || !tinyply_buffer_to_matrix(*normals,N,normals->count,3) ) {
    // Don't do this (see above) N.resize(0,0);
  }

  if (!texcoords || !tinyply_buffer_to_matrix(*texcoords,UV,texcoords->count,2) ) {
    // Don't do this (see above) UV.resize(0,0);
  }

  //HACK: Unfortunately, tinyply doesn't store list size as a separate variable
  if (!faces || !tinyply_buffer_to_matrix(*faces, F, faces->count, faces->count==0?0:faces->buffer.size_bytes()/(tinyply::PropertyTable[faces->t].stride*faces->count) )) {

    if(tristrips) { // need to convert to faces
      // code based on blender importer for ply
      // converting triangle strips into triangles
      // tinyply supports tristrips of the same length only
      size_t el_count = tristrips->buffer.size_bytes()/(tinyply::PropertyTable[tristrips->t].stride*tristrips->count);

      // all strips should have tristrips->count elements
      if(!tinyply_tristrips_to_faces(*tristrips, F , tristrips->count, el_count))
      {
        // Don't do this (see above) F.resize(0,0);
      }

    } else {
      // Don't do this (see above) F.resize(0,0);
    }
  }

  if(!edges || !tinyply_buffer_to_matrix(*edges,E, edges->count,2)) {
    // Don't do this (see above) E.resize(0,0);
  }

  /// convert vertex data:
  Vheader=_vertex_header;
  if(_vertex_header.empty())
  {
    // Don't do this (see above) VD.resize(0,0);
  }
  else
  {
    VD.resize(vertices->count,_vertex_header.size());
    tinyply_buffer_to_matrix(*_vertex_data, VD, vertices->count, _vertex_header.size());
  }

  /// convert face data:
  Fheader=_face_header;
  if(_face_header.empty())
  {
    // Don't do this (see above) FD.resize(0,0);
  }
  else
  {
    FD.resize(faces->count, _face_header.size());
    tinyply_buffer_to_matrix(*_face_data, FD, faces->count, _face_header.size());
  }

  /// convert edge data:
  Eheader=_edge_header;
  if(_edge_header.empty())
  {
    // Don't do this (see above) ED.resize(0,0);
  }
  else
  {
    ED.resize(_edge_data->count, _edge_header.size());
    tinyply_buffer_to_matrix(*_edge_data, ED, _edge_data->count, _edge_header.size());
  }
  return true;
}


template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV,
  typename DerivedVD,
  typename DerivedFD,
  typename DerivedED
  >
IGL_INLINE bool readPLY(
  const std::string& ply_file,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV,

  Eigen::PlainObjectBase<DerivedVD> & VD,
  std::vector<std::string> & VDheader,

  Eigen::PlainObjectBase<DerivedFD> & FD,
  std::vector<std::string> & FDheader,

  Eigen::PlainObjectBase<DerivedED> & ED,
  std::vector<std::string> & EDheader,
  std::vector<std::string> & comments
  )
{

  std::ifstream ply_stream(ply_file, std::ios::binary);
  if (ply_stream.fail())
  {
      std::cerr << "ReadPLY: Error opening file " << ply_file << std::endl;
      return false;
  }
  try
  {
    return readPLY(ply_stream, V, F, E, N, UV, VD, VDheader, FD,FDheader, ED, EDheader, comments );
  } catch (const std::exception& e) {
    std::cerr << "ReadPLY error: " << ply_file << e.what() << std::endl;
  }
  return false;
}


template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV,
  typename DerivedD
  >
IGL_INLINE bool readPLY(
  const std::string & filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV,
  Eigen::PlainObjectBase<DerivedD> & VD,
  std::vector<std::string> & Vheader
  )
{
  Eigen::MatrixXd FD,ED;
  std::vector<std::string> Fheader,Eheader;
  std::vector<std::string> comments;
  return readPLY(filename,V,F,E,N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV
  >
IGL_INLINE bool readPLY(
  const std::string & filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV
  )
{
  Eigen::MatrixXd VD,FD,ED;
  std::vector<std::string> Vheader,Fheader,Eheader;
  std::vector<std::string> comments;
  return readPLY(filename,V,F,E, N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedN,
  typename DerivedUV
  >
IGL_INLINE bool readPLY(
  const std::string & filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV
  )
{
  Eigen::MatrixXd VD,FD,ED;
  Eigen::MatrixXi E;
  std::vector<std::string> Vheader,Fheader,Eheader;
  std::vector<std::string> comments;
  return readPLY(filename,V,F,E, N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}

template <
  typename DerivedV,
  typename DerivedF
  >
IGL_INLINE bool readPLY(
  const std::string & filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F
  )
{
  Eigen::MatrixXd N,UV;
  Eigen::MatrixXd VD,FD,ED;
  Eigen::MatrixXi E;

  std::vector<std::string> Vheader,Fheader,Eheader;
  std::vector<std::string> comments;
  return readPLY(filename,V,F,E,N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE
  >
IGL_INLINE bool readPLY(
  const std::string & filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E
  )
{
  Eigen::MatrixXd N,UV;
  Eigen::MatrixXd VD,FD,ED;

  std::vector<std::string> Vheader,Fheader,Eheader;
  std::vector<std::string> comments;
  return readPLY(filename,V,F,E,N,UV,VD,Vheader,FD,Fheader,ED,Eheader,comments);
}


} //igl namespace

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::readPLY<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readPLY<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readPLY<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template bool igl::readPLY<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template bool igl::readPLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readPLY<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readPLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readPLY<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template bool igl::readPLY<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&);
template bool igl::readPLY<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template bool igl::readPLY<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);

template bool igl::readPLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&);
template bool igl::readPLY<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > >&);
#endif
