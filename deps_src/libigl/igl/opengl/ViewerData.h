// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VIEWERDATA_H
#define IGL_VIEWERDATA_H

#include "../igl_inline.h"
#include "MeshGL.h"
#include <cassert>
#include <cstdint>
#include <Eigen/Core>
#include <memory>
#include <vector>

// Alec: This is a mesh class containing a variety of data types (normals,
// overlays, material colors, etc.)
//
namespace igl
{

// TODO: write documentation
namespace opengl
{

class ViewerData
{
public:
  ViewerData();

  // Empty all fields
  IGL_INLINE void clear();

  // Change the visualization mode, invalidating the cache if necessary
  IGL_INLINE void set_face_based(bool newvalue);

  // Helpers that can draw the most common meshes
  IGL_INLINE void set_mesh(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F);
  IGL_INLINE void set_vertices(const Eigen::MatrixXd& V);
  IGL_INLINE void set_normals(const Eigen::MatrixXd& N);

  // Set the color of the mesh
  //
  // Inputs:
  //   C  #V|#F|1 by 3 list of colors
  IGL_INLINE void set_colors(const Eigen::MatrixXd &C);
  // Set per-vertex UV coordinates
  //
  // Inputs:
  //   UV  #V by 2 list of UV coordinates (indexed by F)
  IGL_INLINE void set_uv(const Eigen::MatrixXd& UV);
  // Set per-corner UV coordinates
  //
  // Inputs:
  //   UV_V  #UV by 2 list of UV coordinates
  //   UV_F  #F by 3 list of UV indices into UV_V
  IGL_INLINE void set_uv(const Eigen::MatrixXd& UV_V, const Eigen::MatrixXi& UV_F);
  // Set the texture associated with the mesh.
  //
  // Inputs:
  //   R  width by height image matrix of red channel
  //   G  width by height image matrix of green channel
  //   B  width by height image matrix of blue channel
  //
  IGL_INLINE void set_texture(
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B);

  // Set the texture associated with the mesh.
  //
  // Inputs:
  //   R  width by height image matrix of red channel
  //   G  width by height image matrix of green channel
  //   B  width by height image matrix of blue channel
  //   A  width by height image matrix of alpha channel
  //
  IGL_INLINE void set_texture(
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A);

  // Sets points given a list of point vertices. In constrast to `set_points`
  // this will (purposefully) clober existing points.
  //
  // Inputs:
  //   P  #P by 3 list of vertex positions
  //   C  #P|1 by 3 color(s)
  IGL_INLINE void set_points(
    const Eigen::MatrixXd& P,
    const Eigen::MatrixXd& C);
  IGL_INLINE void add_points(const Eigen::MatrixXd& P,  const Eigen::MatrixXd& C);
  // Sets edges given a list of edge vertices and edge indices. In constrast
  // to `add_edges` this will (purposefully) clober existing edges.
  //
  // Inputs:
  //   P  #P by 3 list of vertex positions
  //   E  #E by 2 list of edge indices into P
  //   C  #E|1 by 3 color(s)
  IGL_INLINE void set_edges (const Eigen::MatrixXd& P, const Eigen::MatrixXi& E, const Eigen::MatrixXd& C);
  // Alec: This is very confusing. Why does add_edges have a different API from
  // set_edges? 
  IGL_INLINE void add_edges (const Eigen::MatrixXd& P1, const Eigen::MatrixXd& P2, const Eigen::MatrixXd& C);
  IGL_INLINE void add_label (const Eigen::VectorXd& P,  const std::string& str);

  // Computes the normals of the mesh
  IGL_INLINE void compute_normals();

  // Assigns uniform colors to all faces/vertices
  IGL_INLINE void uniform_colors(
    const Eigen::Vector3d& diffuse,
    const Eigen::Vector3d& ambient,
    const Eigen::Vector3d& specular);

  // Assigns uniform colors to all faces/vertices
  IGL_INLINE void uniform_colors(
    const Eigen::Vector4d& ambient,
    const Eigen::Vector4d& diffuse,
    const Eigen::Vector4d& specular);

  // Generates a default grid texture
  IGL_INLINE void grid_texture();

  Eigen::MatrixXd V; // Vertices of the current mesh (#V x 3)
  Eigen::MatrixXi F; // Faces of the mesh (#F x 3)

  // Per face attributes
  Eigen::MatrixXd F_normals; // One normal per face

  Eigen::MatrixXd F_material_ambient; // Per face ambient color
  Eigen::MatrixXd F_material_diffuse; // Per face diffuse color
  Eigen::MatrixXd F_material_specular; // Per face specular color

  // Per vertex attributes
  Eigen::MatrixXd V_normals; // One normal per vertex

  Eigen::MatrixXd V_material_ambient; // Per vertex ambient color
  Eigen::MatrixXd V_material_diffuse; // Per vertex diffuse color
  Eigen::MatrixXd V_material_specular; // Per vertex specular color

  // UV parametrization
  Eigen::MatrixXd V_uv; // UV vertices
  Eigen::MatrixXi F_uv; // optional faces for UVs

  // Texture
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_R;
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_G;
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_B;
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_A;

  // Overlays

  // Lines plotted over the scene
  // (Every row contains 9 doubles in the following format S_x, S_y, S_z, T_x, T_y, T_z, C_r, C_g, C_b),
  // with S and T the coordinates of the two vertices of the line in global coordinates, and C the color in floating point rgb format
  Eigen::MatrixXd lines;

  // Points plotted over the scene
  // (Every row contains 6 doubles in the following format P_x, P_y, P_z, C_r, C_g, C_b),
  // with P the position in global coordinates of the center of the point, and C the color in floating point rgb format
  Eigen::MatrixXd points;

  // Text labels plotted over the scene
  // Textp contains, in the i-th row, the position in global coordinates where the i-th label should be anchored
  // Texts contains in the i-th position the text of the i-th label
  Eigen::MatrixXd           labels_positions;
  std::vector<std::string>  labels_strings;

  // Marks dirty buffers that need to be uploaded to OpenGL
  uint32_t dirty;

  // Enable per-face or per-vertex properties
  bool face_based;

  // Visualization options
  bool show_overlay;
  bool show_overlay_depth;
  bool show_texture;
  bool show_faces;
  bool show_lines;
  bool show_vertid;
  bool show_faceid;
  bool invert_normals;

  // Point size / line width
  float point_size;
  float line_width;
  Eigen::Vector4f line_color;

  // Shape material
  float shininess;

  // Unique identifier
  int id;

  // OpenGL representation of the mesh
  igl::opengl::MeshGL meshgl;

  // Update contents from a 'Data' instance
  IGL_INLINE void updateGL(
    const igl::opengl::ViewerData& data,
    const bool invert_normals,
    igl::opengl::MeshGL& meshgl);
};

} // namespace opengl
} // namespace igl

////////////////////////////////////////////////////////////////////////////////

#include <igl/serialize.h>
namespace igl
{
  namespace serialization
  {
    inline void serialization(bool s, igl::opengl::ViewerData& obj, std::vector<char>& buffer)
    {
      SERIALIZE_MEMBER(V);
      SERIALIZE_MEMBER(F);
      SERIALIZE_MEMBER(F_normals);
      SERIALIZE_MEMBER(F_material_ambient);
      SERIALIZE_MEMBER(F_material_diffuse);
      SERIALIZE_MEMBER(F_material_specular);
      SERIALIZE_MEMBER(V_normals);
      SERIALIZE_MEMBER(V_material_ambient);
      SERIALIZE_MEMBER(V_material_diffuse);
      SERIALIZE_MEMBER(V_material_specular);
      SERIALIZE_MEMBER(V_uv);
      SERIALIZE_MEMBER(F_uv);
      SERIALIZE_MEMBER(texture_R);
      SERIALIZE_MEMBER(texture_G);
      SERIALIZE_MEMBER(texture_B);
      SERIALIZE_MEMBER(texture_A);
      SERIALIZE_MEMBER(lines);
      SERIALIZE_MEMBER(points);
      SERIALIZE_MEMBER(labels_positions);
      SERIALIZE_MEMBER(labels_strings);
      SERIALIZE_MEMBER(dirty);
      SERIALIZE_MEMBER(face_based);
      SERIALIZE_MEMBER(show_faces);
      SERIALIZE_MEMBER(show_lines);
      SERIALIZE_MEMBER(invert_normals);
      SERIALIZE_MEMBER(show_overlay);
      SERIALIZE_MEMBER(show_overlay_depth);
      SERIALIZE_MEMBER(show_vertid);
      SERIALIZE_MEMBER(show_faceid);
      SERIALIZE_MEMBER(show_texture);
      SERIALIZE_MEMBER(point_size);
      SERIALIZE_MEMBER(line_width);
      SERIALIZE_MEMBER(line_color);
      SERIALIZE_MEMBER(shininess);
      SERIALIZE_MEMBER(id);
    }
    template<>
    inline void serialize(const igl::opengl::ViewerData& obj, std::vector<char>& buffer)
    {
      serialization(true, const_cast<igl::opengl::ViewerData&>(obj), buffer);
    }
    template<>
    inline void deserialize(igl::opengl::ViewerData& obj, const std::vector<char>& buffer)
    {
      serialization(false, obj, const_cast<std::vector<char>&>(buffer));
      obj.dirty = igl::opengl::MeshGL::DIRTY_ALL;
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "ViewerData.cpp"
#endif

#endif
