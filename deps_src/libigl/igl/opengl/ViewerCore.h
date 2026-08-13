// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_VIEWERCORE_H
#define IGL_OPENGL_VIEWERCORE_H

#include "MeshGL.h"

#include "../igl_inline.h"
#include <Eigen/Geometry>
#include <Eigen/Core>

namespace igl
{
namespace opengl
{

// Forward declaration
class ViewerData;

/// Basic class of the 3D mesh viewer
class ViewerCore
{
public:
  using GLuint = MeshGL::GLuint;
  IGL_INLINE ViewerCore();

  /// Initialization
  IGL_INLINE void init();

  /// Shutdown
  IGL_INLINE void shut();

  /// Serialization code
  IGL_INLINE void InitSerialization();

  /// Adjust the view to see the entire model
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  IGL_INLINE void align_camera_center(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& F);
  /// \overload
  IGL_INLINE void align_camera_center(
    const Eigen::MatrixXd& V);

  /// Determines how much to zoom and shift such that the mesh fills the unit
  /// box (centered at the origin)
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[out] zoom  zoom factor
  /// @param[out] shift  3d shift
  IGL_INLINE void get_scale_and_shift_to_fit_mesh(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& F,
    float & zoom,
    Eigen::Vector3f& shift);
  /// \overload
  IGL_INLINE void get_scale_and_shift_to_fit_mesh(
    const Eigen::MatrixXd& V,
    float & zoom,
    Eigen::Vector3f& shift);

  /// Clear the frame buffers
  IGL_INLINE void clear_framebuffers();

  /// Draw everything
  ///
  /// \note data cannot be const because it is being set to "clean"
  ///
  /// @param[in] data  which ViewerData to draw
  /// @param[in] update_matrices  whether to update view, proj, and norm
  ///   matrices in shaders
  IGL_INLINE void draw(ViewerData& data, bool update_matrices = true);
  /// initialize shadow pass
  IGL_INLINE void initialize_shadow_pass();
  /// deinitialize shadow pass
  IGL_INLINE void deinitialize_shadow_pass();
  /// Draw everything to shadow map
  /// @param[in] data  which ViewerData to draw
  /// @param[in] update_matrices  whether to update view, proj, and norm
  IGL_INLINE void draw_shadow_pass(ViewerData& data, bool update_matrices = true);
  /// Render given ViewerData to a buffer. The width and height are determined by
  /// non-zeros dimensions of R (and G,B,A should match) or – if both are zero —
  /// are set to this core's viewport sizes.
  ///
  /// @param[in] data  which ViewerData to draw
  /// @param[in] update_matrices  whether to update view, proj, and norm matrices in
  ///     shaders
  /// @param[out] R  width by height red pixel color values
  /// @param[out] G  width by height green pixel color values
  /// @param[out] B  width by height blue pixel color values
  /// @param[out] A  width by height alpha pixel color values
  ///
  IGL_INLINE void draw_buffer(
    ViewerData& data,
    bool update_matrices,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A);
  /// Draw the text lables
  /// @param[in] data  which ViewerData to draw
  /// @param[in] labels  text labels to draw
  IGL_INLINE void draw_labels(
    ViewerData& data,
    const igl::opengl::MeshGL::TextGL& labels
  );

  /// Type of user interface for changing the view rotation based on the mouse
  /// draggin.
  enum RotationType
  {
    /// Typical trackball rotation (like Meshlab)
    ROTATION_TYPE_TRACKBALL = 0,
    /// Fixed up rotation (like Blender, Maya, etc.)
    ROTATION_TYPE_TWO_AXIS_VALUATOR_FIXED_UP = 1,
    /// No rotation suitable for 2D 
    ROTATION_TYPE_NO_ROTATION = 2,
    /// Total number of rotation types
    NUM_ROTATION_TYPES = 3
  };
  /// Set the current rotation type
  /// @param[in] value  the new rotation type
  IGL_INLINE void set_rotation_type(const RotationType & value);

  /// Set a ViewerData visualization option for this viewport
  /// @param[in] property_mask  a bit mask of visualization option
  /// @param[in] value  whether to set or unset the property
  IGL_INLINE void set(unsigned int &property_mask, bool value = true) const;

  /// Unset a ViewerData visualization option for this viewport
  /// @param[in] property_mask  a bit mask of visualization option
  IGL_INLINE void unset(unsigned int &property_mask) const;

  /// Toggle a ViewerData visualization option for this viewport
  /// @param[in] property_mask  a bit mask of visualization option
  IGL_INLINE void toggle(unsigned int &property_mask) const;

  /// Check whether a ViewerData visualization option is set for this viewport
  /// @param[in] property_mask  a bit mask of visualization option
  /// @returns whether the property is set
  IGL_INLINE bool is_set(unsigned int property_mask) const;

  /// delete the shadow buffers
  IGL_INLINE void delete_shadow_buffers();
  /// generate the shadow buffers
  IGL_INLINE void generate_shadow_buffers();

  // ------------------- Properties

  /// Unique identifier
  unsigned int id = 1u;

  /// Background color as RGBA
  Eigen::Vector4f background_color;

  /// Light position (or direction to light)
  Eigen::Vector3f light_position;
  /// Whether to treat `light_position` as a point or direction
  bool is_directional_light;
  /// Whether shadow mapping is on
  bool is_shadow_mapping;
  /// Width of the shadow map
  GLuint shadow_width;
  /// Height of the shadow map
  GLuint shadow_height;
  /// Shadow map depth texture
  GLuint shadow_depth_tex;
  /// Shadow map depth framebuffer object
  GLuint shadow_depth_fbo;
  /// Shadow map color render buffer object
  GLuint shadow_color_rbo;
  /// Factor of lighting (0: no lighting, 1: full lighting)
  float lighting_factor;

  /// Type of rotation interaction
  RotationType rotation_type;
  /// View rotation as quaternion
  Eigen::Quaternionf trackball_angle;

  /// Base zoom of camera
  float camera_base_zoom;
  /// Current zoom of camera
  float camera_zoom;
  /// Whether camera is orthographic (or perspective)
  bool orthographic;
  /// Base translation of camera
  Eigen::Vector3f camera_base_translation;
  /// Current translation of camera
  Eigen::Vector3f camera_translation;
  /// Current "eye" / origin position of camera
  Eigen::Vector3f camera_eye;
  /// Current "up" vector of camera
  Eigen::Vector3f camera_up;
  /// Current "look at" position of camera
  Eigen::Vector3f camera_center;
  /// Current view angle of camera
  float camera_view_angle;
  /// Near plane of camera
  float camera_dnear;
  /// Far plane of camera
  float camera_dfar;

  /// Whether testing for depth is enabled
  bool depth_test;

  /// Whether "animating" (continuous drawing) is enabled
  bool is_animating;
  /// Max fps of animation loop (e.g. 30fps or 60fps)
  double animation_max_fps;

  /// Caches the two-norm between the min/max point of the bounding box
  float object_scale;

  /// Viewport size
  Eigen::Vector4f viewport;

  /// OpenGL view transformation matrix on last render pass
  Eigen::Matrix4f view;
  /// OpenGL proj transformation matrix on last render pass
  Eigen::Matrix4f proj;
  /// OpenGL norm transformation matrix on last render pass
  Eigen::Matrix4f norm;
  /// OpenGL shadow_view transformation matrix on last render pass
  Eigen::Matrix4f shadow_view;
  /// OpenGL shadow_proj transformation matrix on last render pass
  Eigen::Matrix4f shadow_proj;
  public:
      EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

}
}

#include "../serialize.h"
namespace igl {
  namespace serialization {

    inline void serialization(bool s, igl::opengl::ViewerCore& obj, std::vector<char>& buffer)
    {

      SERIALIZE_MEMBER(background_color);

      SERIALIZE_MEMBER(light_position);
      SERIALIZE_MEMBER(lighting_factor);

      SERIALIZE_MEMBER(trackball_angle);
      SERIALIZE_MEMBER(rotation_type);

      SERIALIZE_MEMBER(camera_base_zoom);
      SERIALIZE_MEMBER(camera_zoom);
      SERIALIZE_MEMBER(orthographic);
      SERIALIZE_MEMBER(camera_base_translation);
      SERIALIZE_MEMBER(camera_translation);
      SERIALIZE_MEMBER(camera_view_angle);
      SERIALIZE_MEMBER(camera_dnear);
      SERIALIZE_MEMBER(camera_dfar);
      SERIALIZE_MEMBER(camera_eye);
      SERIALIZE_MEMBER(camera_center);
      SERIALIZE_MEMBER(camera_up);

      SERIALIZE_MEMBER(depth_test);
      SERIALIZE_MEMBER(is_animating);
      SERIALIZE_MEMBER(animation_max_fps);

      SERIALIZE_MEMBER(object_scale);

      SERIALIZE_MEMBER(viewport);
      SERIALIZE_MEMBER(view);
      SERIALIZE_MEMBER(proj);
      SERIALIZE_MEMBER(norm);
    }

    template<>
    inline void serialize(const igl::opengl::ViewerCore& obj, std::vector<char>& buffer)
    {
      serialization(true, const_cast<igl::opengl::ViewerCore&>(obj), buffer);
    }

    template<>
    inline void deserialize(igl::opengl::ViewerCore& obj, const std::vector<char>& buffer)
    {
      serialization(false, obj, const_cast<std::vector<char>&>(buffer));
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "ViewerCore.cpp"
#endif

#endif
