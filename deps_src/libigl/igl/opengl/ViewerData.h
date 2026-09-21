// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VIEWERDATA_H
#define IGL_VIEWERDATA_H

#include "MeshGL.h"
#include "../igl_inline.h"
#include "../colormap.h"
#include <cassert>
#include <cstdint>
#include <Eigen/Core>
#include <memory>
#include <vector>

namespace igl
{

namespace opengl
{

class ViewerCore;

/// Object being drawn (i.e., mesh and its accessories) by the ViewerCore
///
/// \warning Eigen data members (such as Eigen::Vector4f) should explicitly
/// disable alignment (e.g. use `Eigen::Matrix<float, 4, 1, Eigen::DontAlign>`),
/// in order to avoid alignment issues further down the line (esp. if the
/// structure are stored in a std::vector).
///
/// See this thread for a more detailed discussion:
/// https://github.com/libigl/libigl/pull/1029
///
class ViewerData
{
public:
  ViewerData();

  /// Empty all fields
  IGL_INLINE void clear();

  /// Change whether drawing per-vertex or per-face; invalidating cache if
  /// necessary
  ///
  /// @param[in] newvalue whether face based 
  IGL_INLINE void set_face_based(bool newvalue);

  /// Set the current mesh. Call this once at the beginning and whenever
  /// **both** the vertices and faces are changing.
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3/4 list of mesh faces (triangles/tets)
  IGL_INLINE void set_mesh(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F);
  /// Set just the vertices. Call this if _just_ the vertices are changing. You
  /// may need to call compute_normals to update the lighting correctly. 
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  IGL_INLINE void set_vertices(const Eigen::MatrixXd& V);
  /// Set the normals of a mesh. 
  ///
  /// @param[in] N #V|#F|3#F by 3 list of mesh normals
  ///
  /// \see compute_normals
  IGL_INLINE void set_normals(const Eigen::MatrixXd& N);
  /// Set whether this object is visible
  /// @param[in] value  true iff this object is visible
  /// @param[in] core_id  Index of the core to set (default is 0)
  IGL_INLINE void set_visible(bool value, unsigned int core_id = 1);

  /// Set the diffuse color of the mesh. The ambient color will be set to 0.1*C
  /// and the specular color will be set to 0.3+0.1*(C-0.3).
  ///
  /// @param[in] C  #V|#F|1 by 3 list of diffuse colors
  IGL_INLINE void set_colors(const Eigen::MatrixXd &C);

  /// Set per-vertex UV coordinates
  ///
  /// @param[in] UV  #V by 2 list of UV coordinates (indexed by F)
  IGL_INLINE void set_uv(const Eigen::MatrixXd& UV);

  /// Set per-corner UV coordinates
  ///
  /// @param[in] UV_V  #UV by 2 list of UV coordinates
  /// @param[in] UV_F  #F by 3 list of UV indices into UV_V
  IGL_INLINE void set_uv(const Eigen::MatrixXd& UV_V, const Eigen::MatrixXi& UV_F);

  /// Set the texture associated with the mesh.
  ///
  /// @param[in] R  width by height image matrix of red channel
  /// @param[in] G  width by height image matrix of green channel
  /// @param[in] B  width by height image matrix of blue channel
  /// @param[in] A  width by height image matrix of alpha channel
  ///
  IGL_INLINE void set_texture(
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A);
  /// \overload
  IGL_INLINE void set_texture(
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B);

  /// Set pseudo-color-able scalar data associated with the mesh.
  ///
  /// @param[in] caxis_min  caxis minimum bound
  /// @param[in] caxis_max  caxis maximum bound
  /// @param[in] D  #V|#F by 1 list of scalar values
  /// @param[in] cmap colormap type
  /// @param[in] num_steps number of intervals to discretize the colormap
  IGL_INLINE void set_data(
    const Eigen::VectorXd & D,
    double caxis_min,
    double caxis_max,
    igl::ColorMapType cmap = igl::COLOR_MAP_TYPE_VIRIDIS,
    int num_steps = 21);

  /// \overload
  /// \brief Use min(D) and max(D) to set caxis.
  IGL_INLINE void set_data(const Eigen::VectorXd & D,
    igl::ColorMapType cmap = igl::COLOR_MAP_TYPE_VIRIDIS,
    int num_steps = 21);

  /// Not to be confused with set_colors, this creates a _texture_ that will be
  /// referenced to pseudocolor according to the scalar field passed to set_data.
  ///
  /// @param[in] CM  #CM by 3 list of colors
  IGL_INLINE void set_colormap(const Eigen::MatrixXd & CM);

  /// Sets points given a list of point vertices. In constrast to `add_points`
  /// this will (purposefully) clober existing points.
  ///
  /// @param[in] P  #P by 3 list of vertex positions
  /// @param[in] C  #P|1 by 3 color(s)
  IGL_INLINE void set_points(
    const Eigen::MatrixXd& P,
    const Eigen::MatrixXd& C);

  /// Add points given a list of point vertices.
  ///
  /// @param[in] P  #P by 3 list of vertex positions
  /// @param[in] C  #P|1 by 3 color(s)
  IGL_INLINE void add_points(const Eigen::MatrixXd& P,  const Eigen::MatrixXd& C);

  /// Clear the point data
  IGL_INLINE void clear_points();

  /// Sets edges given a list of edge vertices and edge indices. In constrast
  /// to `add_edges` this will (purposefully) clober existing edges.
  ///
  /// @param[in] P  #P by 3 list of vertex positions
  /// @param[in] E  #E by 2 list of edge indices into P
  /// @param[in] C  #E|1 by 3 color(s)
  IGL_INLINE void set_edges (const Eigen::MatrixXd& P, const Eigen::MatrixXi& E, const Eigen::MatrixXd& C);
  /// Add edges given a list of edge start and end positions and colors
  ///
  /// @param[in] P1  #P by 3 list of edge start positions
  /// @param[in] P2  #P by 3 list of edge end positions
  /// @param[in] C  #P|1 by 3 color(s)
  /// \note This is very confusing. Why does add_edges have a different API from
  /// set_edges?
  IGL_INLINE void add_edges (const Eigen::MatrixXd& P1, const Eigen::MatrixXd& P2, const Eigen::MatrixXd& C);
  /// Sets edges given a list of points and eminating vectors
  ///
  /// @param[in] P  #P by 3 list of vertex positions
  /// @param[in] V  #P by 3 list of eminating vectors
  /// @param[in] C  #P|1 by 3 color(s)
  IGL_INLINE void set_edges_from_vector_field(
    const Eigen::MatrixXd& P, 
    const Eigen::MatrixXd& V, 
    const Eigen::MatrixXd& C);

  /// Clear the edge data
  IGL_INLINE void clear_edges();

  /// Sets / Adds text labels at the given positions in 3D.
  /// \param[in] P  #P by 3 list of vertex positions
  /// \param[in] str  #P list of text labels
  /// \note This requires the ImGui viewer plugin to display text labels.
  IGL_INLINE void set_labels (const Eigen::MatrixXd& P,  const std::vector<std::string>& str);
  /// Sets / Adds text labels at the given positions in 3D.
  /// @param[in] P 3D position of the label
  /// @param[in] str text label
  IGL_INLINE void add_label (const Eigen::VectorXd& P,  const std::string& str);

  /// Clear the label data
  IGL_INLINE void clear_labels ();

  /// Computes the normals of the mesh
  IGL_INLINE void compute_normals();

  /// Assigns uniform colors to all faces/vertices
  /// \param[in] ambient  ambient color
  /// \param[in] diffuse  diffuse color
  /// \param[in] specular specular color
  IGL_INLINE void uniform_colors(
    const Eigen::Vector3d& ambient,
    const Eigen::Vector3d& diffuse,
    const Eigen::Vector3d& specular);
  /// \overload
  IGL_INLINE void uniform_colors(
    const Eigen::Vector4d& ambient,
    const Eigen::Vector4d& diffuse,
    const Eigen::Vector4d& specular);

  /// Generate a normal image matcap
  IGL_INLINE void normal_matcap();

  /// Generates a default grid texture (without uvs)
  IGL_INLINE void grid_texture();

  /// Copy visualization options from one viewport to another
  /// \param[in] from  source viewport
  /// \param[in] to  destination viewport
  IGL_INLINE void copy_options(const ViewerCore &from, const ViewerCore &to);

  /// Vertices of the current mesh (#V x 3)
  Eigen::MatrixXd V;
  /// Faces of the mesh (#F x 3)
  Eigen::MatrixXi F;

  // Per face attributes
  /// One normal per face
  Eigen::MatrixXd F_normals;

  /// Per face ambient color
  Eigen::MatrixXd F_material_ambient;
  /// Per face diffuse color
  Eigen::MatrixXd F_material_diffuse;
  /// Per face specular color
  Eigen::MatrixXd F_material_specular;

  // Per vertex attributes
  /// One normal per vertex
  Eigen::MatrixXd V_normals;

  /// Per vertex ambient color
  Eigen::MatrixXd V_material_ambient;
  /// Per vertex diffuse color
  Eigen::MatrixXd V_material_diffuse;
  /// Per vertex specular color
  Eigen::MatrixXd V_material_specular;

  // UV parametrization
  /// UV vertices
  Eigen::MatrixXd V_uv;
  /// optional faces for UVs
  Eigen::MatrixXi F_uv;

  // Texture
  /// Texture red colors
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_R;
  /// Texture green colors
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_G;
  /// Texture blue colors
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_B;
  /// Texture alpha values
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> texture_A;

  // Overlays

  /// Lines plotted over the scene
  /// (Every row contains 9 doubles in the following format S_x, S_y, S_z, T_x, T_y, T_z, C_r, C_g, C_b),
  // with S and T the coordinates of the two vertices of the line in global coordinates, and C the color in floating point rgb format
  Eigen::MatrixXd lines;

  /// Points plotted over the scene
  /// (Every row contains 6 doubles in the following format P_x, P_y, P_z, C_r, C_g, C_b),
  /// with P the position in global coordinates of the center of the point, and C the color in floating point rgb format
  Eigen::MatrixXd points;

  // Text labels plotted over the scene
  // Textp contains, in the i-th row, the position in global coordinates where the i-th label should be anchored
  // Texts contains in the i-th position the text of the i-th label
  /// Text positions of vertices
  Eigen::MatrixXd           vertex_labels_positions;
  /// Text positions of faces
  Eigen::MatrixXd           face_labels_positions;
  /// Text positions of labels
  Eigen::MatrixXd           labels_positions;
  /// Text strings of labels at vertices
  std::vector<std::string>  vertex_labels_strings;
  /// Text strings of labels at faces
  std::vector<std::string>  face_labels_strings;
  /// Text strings of labels
  std::vector<std::string>  labels_strings;

  /// Marks dirty buffers that need to be uploaded to OpenGL
  uint32_t dirty;

  /// Enable per-face or per-vertex properties
  bool face_based;

  /// Enable double-sided lighting on faces
  bool double_sided;

  /// Invert mesh normals
  bool invert_normals;

  /// Visualization options
  /// Each option is a binary mask specifying on which viewport each option is set.
  /// When using a single viewport, standard boolean can still be used for simplicity.
  unsigned int is_visible;
  unsigned int show_custom_labels;
  unsigned int show_face_labels;
  unsigned int show_faces;
  unsigned int show_lines;
  unsigned int show_overlay;
  unsigned int show_overlay_depth;
  unsigned int show_texture;
  unsigned int show_vertex_labels;
  unsigned int use_matcap;

  /// Point size / line width
  float point_size;
  /// line_width is NOT SUPPORTED on Mac OS and Windows
  float line_width;
  /// Size of lables
  float label_size;
  /// Color of lines
  Eigen::Matrix<float, 4, 1, Eigen::DontAlign> line_color;
  /// Color of labels
  Eigen::Matrix<float, 4, 1, Eigen::DontAlign> label_color;

  /// Shape material shininess
  /// \deprecated unused?
  float shininess;

  /// Unique identifier
  int id;

  /// OpenGL representation of the mesh
  igl::opengl::MeshGL meshgl;

  /// Update contents from a 'Data' instance
  ///
  /// @param[in,out] GL_labels  labels to be updated
  /// @param[in] positions  positions of the labels
  /// @param[in] strings  strings of the labels
  IGL_INLINE void update_labels(
    igl::opengl::MeshGL::TextGL& GL_labels,
    const Eigen::MatrixXd& positions,
    const std::vector<std::string>& strings
  );
  /// Update the meshgl object
  /// \param[in] data  data to be used for the update
  /// \param[in] invert_normals  whether to invert normals
  /// \param[in,out] meshgl  mesh to be updated
  IGL_INLINE void updateGL(
    const igl::opengl::ViewerData& data,
    const bool invert_normals,
    igl::opengl::MeshGL& meshgl);
  };

} // namespace opengl
} // namespace igl

////////////////////////////////////////////////////////////////////////////////

#include "../serialize.h"
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
      SERIALIZE_MEMBER(show_vertex_labels);
      SERIALIZE_MEMBER(show_face_labels);
      SERIALIZE_MEMBER(show_custom_labels);
      SERIALIZE_MEMBER(show_texture);
      SERIALIZE_MEMBER(double_sided);
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
