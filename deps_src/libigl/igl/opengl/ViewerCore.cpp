// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "ViewerCore.h"
#include "ViewerData.h"
#include "gl.h"
#include "../quat_to_mat.h"
#include "../null.h"
#include "../snap_to_fixed_up.h"
#include "../look_at.h"
#include "../frustum.h"
#include "../ortho.h"
#include "../massmatrix.h"
#include "../barycenter.h"
#include "../PI.h"
#include "report_gl_error.h"
#include "read_pixels.h"
#include <Eigen/Geometry>
#include <iostream>

IGL_INLINE void igl::opengl::ViewerCore::align_camera_center(
  const Eigen::MatrixXd& V,
  const Eigen::MatrixXi& F)
{
  if(V.rows() == 0)
    return;

  get_scale_and_shift_to_fit_mesh(V,F,camera_base_zoom,camera_base_translation);
  // Rather than crash on empty mesh...
  if(V.size() > 0)
  {
    object_scale = (V.colwise().maxCoeff() - V.colwise().minCoeff()).norm();
  }
}

IGL_INLINE void igl::opengl::ViewerCore::get_scale_and_shift_to_fit_mesh(
  const Eigen::MatrixXd& V,
  const Eigen::MatrixXi& F,
  float& zoom,
  Eigen::Vector3f& shift)
{
  if (V.rows() == 0)
    return;

  Eigen::MatrixXd BC;
  if (F.rows() <= 1)
  {
    BC = V;
  } else
  {
    igl::barycenter(V,F,BC);
  }
  return get_scale_and_shift_to_fit_mesh(BC,zoom,shift);
}

IGL_INLINE void igl::opengl::ViewerCore::align_camera_center(
  const Eigen::MatrixXd& V)
{
  if(V.rows() == 0)
    return;

  get_scale_and_shift_to_fit_mesh(V,camera_base_zoom,camera_base_translation);
  // Rather than crash on empty mesh...
  if(V.size() > 0)
  {
    object_scale = (V.colwise().maxCoeff() - V.colwise().minCoeff()).norm();
  }
}

IGL_INLINE void igl::opengl::ViewerCore::get_scale_and_shift_to_fit_mesh(
  const Eigen::MatrixXd& V,
  float& zoom,
  Eigen::Vector3f& shift)
{
  if (V.rows() == 0)
    return;

  auto min_point = V.colwise().minCoeff();
  auto max_point = V.colwise().maxCoeff();
  auto centroid  = (0.5*(min_point + max_point)).eval();
  shift.setConstant(0);
  shift.head(centroid.size()) = -centroid.cast<float>();
  zoom = 2.0 / (max_point-min_point).array().abs().maxCoeff();
}


IGL_INLINE void igl::opengl::ViewerCore::clear_framebuffers()
{
  // The glScissor call ensures we only clear this core's buffers,
  // (in case the user wants different background colors in each viewport.)
  glScissor(viewport(0), viewport(1), viewport(2), viewport(3));
  glEnable(GL_SCISSOR_TEST);
  glClearColor(background_color[0],
               background_color[1],
               background_color[2],
               background_color[3]);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
}

IGL_INLINE void igl::opengl::ViewerCore::draw(
  ViewerData& data,
  bool update_matrices)
{
  using namespace std;
  using namespace Eigen;

  if (depth_test)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  /* Bind and potentially refresh mesh/line/point data */
  if (data.dirty)
  {
    data.updateGL(data, data.invert_normals, data.meshgl);
    data.dirty = MeshGL::DIRTY_NONE;
  }
  data.meshgl.bind_mesh();

  // Initialize uniform
  glViewport(viewport(0), viewport(1), viewport(2), viewport(3));

  if(update_matrices)
  {
    view = Eigen::Matrix4f::Identity();
    proj = Eigen::Matrix4f::Identity();
    norm = Eigen::Matrix4f::Identity();

    float width  = viewport(2);
    float height = viewport(3);

    // Set view
    look_at( camera_eye, camera_center, camera_up, view);
    view = view
      * (trackball_angle * Eigen::Scaling(camera_zoom * camera_base_zoom)
      * Eigen::Translation3f(camera_translation + camera_base_translation)).matrix();

    norm = view.inverse().transpose();

    // Set projection
    if (orthographic)
    {
      float length = (camera_eye - camera_center).norm();
      float h = tan(camera_view_angle/360.0 * igl::PI) * (length);
      ortho(-h*width/height, h*width/height, -h, h, camera_dnear, camera_dfar,proj);
    }
    else
    {
      float fH = tan(camera_view_angle / 360.0 * igl::PI) * camera_dnear;
      float fW = fH * (double)width/(double)height;
      frustum(-fW, fW, -fH, fH, camera_dnear, camera_dfar,proj);
    }
  }

  // Send transformations to the GPU
  GLint viewi  = glGetUniformLocation(data.meshgl.shader_mesh,"view");
  GLint proji  = glGetUniformLocation(data.meshgl.shader_mesh,"proj");
  GLint normi  = glGetUniformLocation(data.meshgl.shader_mesh,"normal_matrix");
  glUniformMatrix4fv(viewi, 1, GL_FALSE, view.data());
  glUniformMatrix4fv(proji, 1, GL_FALSE, proj.data());
  glUniformMatrix4fv(normi, 1, GL_FALSE, norm.data());

  // Light parameters
  GLint specular_exponenti    = glGetUniformLocation(data.meshgl.shader_mesh,"specular_exponent");
  GLint light_position_eyei = glGetUniformLocation(data.meshgl.shader_mesh,"light_position_eye");
  GLint lighting_factori      = glGetUniformLocation(data.meshgl.shader_mesh,"lighting_factor");
  GLint fixed_colori          = glGetUniformLocation(data.meshgl.shader_mesh,"fixed_color");
  GLint texture_factori       = glGetUniformLocation(data.meshgl.shader_mesh,"texture_factor");
  GLint matcap_factori        = glGetUniformLocation(data.meshgl.shader_mesh,"matcap_factor");
  GLint double_sidedi         = glGetUniformLocation(data.meshgl.shader_mesh,"double_sided");

  const bool eff_is_directional_light = is_directional_light || is_shadow_mapping;
  glUniform1f(specular_exponenti, data.shininess);
  if(eff_is_directional_light)
  {
    Eigen::Vector3f light_direction  = light_position.normalized();
    glUniform3fv(light_position_eyei, 1, light_direction.data());
  }else
  {
    glUniform3fv(light_position_eyei, 1, light_position.data());
  }
  if(is_shadow_mapping)
  {
    glUniformMatrix4fv(glGetUniformLocation(data.meshgl.shader_mesh,"shadow_view"), 1, GL_FALSE, shadow_view.data());
    glUniformMatrix4fv(glGetUniformLocation(data.meshgl.shader_mesh,"shadow_proj"), 1, GL_FALSE, shadow_proj.data());
    glActiveTexture(GL_TEXTURE0+1);
    glBindTexture(GL_TEXTURE_2D, shadow_depth_tex);
    {
      glUniform1i(glGetUniformLocation(data.meshgl.shader_mesh,"shadow_tex"), 1);
    }
  }
  glUniform1f(lighting_factori, lighting_factor); // enables lighting
  glUniform4f(fixed_colori, 0.0, 0.0, 0.0, 0.0);

  glUniform1i(glGetUniformLocation(data.meshgl.shader_mesh,"is_directional_light"),eff_is_directional_light);
  glUniform1i(glGetUniformLocation(data.meshgl.shader_mesh,"is_shadow_mapping"),is_shadow_mapping);
  glUniform1i(glGetUniformLocation(data.meshgl.shader_mesh,"shadow_pass"),false);

  if (data.V.rows()>0)
  {
    // Render fill
    if (is_set(data.show_faces))
    {
      // Texture
      glUniform1f(texture_factori, is_set(data.show_texture) ? 1.0f : 0.0f);
      glUniform1f(matcap_factori, is_set(data.use_matcap) ? 1.0f : 0.0f);
      glUniform1f(double_sidedi, data.double_sided ? 1.0f : 0.0f);
      data.meshgl.draw_mesh(true);
      glUniform1f(matcap_factori, 0.0f);
      glUniform1f(texture_factori, 0.0f);
    }

    // Render wireframe
    if (is_set(data.show_lines))
    {
      glLineWidth(data.line_width);
      glUniform4f(fixed_colori,
        data.line_color[0],
        data.line_color[1],
        data.line_color[2],
        data.line_color[3]);
      data.meshgl.draw_mesh(false);
      glUniform4f(fixed_colori, 0.0f, 0.0f, 0.0f, 0.0f);
    }
  }

  if (is_set(data.show_overlay))
  {
    if (is_set(data.show_overlay_depth))
      glEnable(GL_DEPTH_TEST);
    else
      glDisable(GL_DEPTH_TEST);

    if (data.lines.rows() > 0)
    {
      data.meshgl.bind_overlay_lines();
      viewi  = glGetUniformLocation(data.meshgl.shader_overlay_lines,"view");
      proji  = glGetUniformLocation(data.meshgl.shader_overlay_lines,"proj");

      glUniformMatrix4fv(viewi, 1, GL_FALSE, view.data());
      glUniformMatrix4fv(proji, 1, GL_FALSE, proj.data());
      // This must be enabled, otherwise glLineWidth has no effect
      glEnable(GL_LINE_SMOOTH);
      glLineWidth(data.line_width);

      data.meshgl.draw_overlay_lines();
    }

    if (data.points.rows() > 0)
    {
      data.meshgl.bind_overlay_points();
      viewi  = glGetUniformLocation(data.meshgl.shader_overlay_points,"view");
      proji  = glGetUniformLocation(data.meshgl.shader_overlay_points,"proj");

      glUniformMatrix4fv(viewi, 1, GL_FALSE, view.data());
      glUniformMatrix4fv(proji, 1, GL_FALSE, proj.data());
      glPointSize(data.point_size);
      data.meshgl.draw_overlay_points();
    }
    glEnable(GL_DEPTH_TEST);
  }

  if(is_set(data.show_vertex_labels)&&data.vertex_labels_positions.rows()>0) 
    draw_labels(data, data.meshgl.vertex_labels);
  if(is_set(data.show_face_labels)&&data.face_labels_positions.rows()>0) 
    draw_labels(data, data.meshgl.face_labels);
  if(is_set(data.show_custom_labels)&&data.labels_positions.rows()>0) 
    draw_labels(data, data.meshgl.custom_labels);
}

IGL_INLINE void igl::opengl::ViewerCore::initialize_shadow_pass()
{
  // attach buffers
  glBindFramebuffer(GL_FRAMEBUFFER,   shadow_depth_fbo);
  glBindRenderbuffer(GL_RENDERBUFFER, shadow_color_rbo);
  // clear buffer 
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  // In the libigl viewer setup, each mesh has its own shader program. This is
  // kind of funny because they should all be the same, just different uniform
  // values.
  glViewport(0,0,shadow_width,shadow_height);
  // Assumes light is directional
  assert(is_directional_light);
  Eigen::Vector3f camera_eye = light_position.normalized()*5;
  Eigen::Vector3f camera_up = [&camera_eye]()
    {
      Eigen::Matrix<float,3,2> T;
      igl::null(camera_eye.transpose().eval(),T);
      return T.col(0);
    }();
  Eigen::Vector3f camera_center = this->camera_center;
  // Same camera parameters except 2× field of view and reduced far plane
  float camera_view_angle =               2*this->camera_view_angle;
  float camera_dnear =                      this->camera_dnear;
  float camera_dfar =                       this->camera_dfar;
  Eigen::Quaternionf trackball_angle =      this->trackball_angle;
  float camera_zoom =                       this->camera_zoom;
  float camera_base_zoom =                  this->camera_base_zoom;
  Eigen::Vector3f camera_translation =      this->camera_translation;
  Eigen::Vector3f camera_base_translation = this->camera_base_translation;
  camera_dfar = exp2( 0.5 * ( log2(camera_dnear) + log2(camera_dfar)));
  igl::look_at( camera_eye, camera_center, camera_up, shadow_view);
  shadow_view = shadow_view
    * (trackball_angle * Eigen::Scaling(camera_zoom * camera_base_zoom)
    * Eigen::Translation3f(camera_translation + camera_base_translation)).matrix();

  float length = (camera_eye - camera_center).norm();
  float h = tan(camera_view_angle/360.0 * igl::PI) * (length);
  igl::ortho(-h*shadow_width/shadow_height, h*shadow_width/shadow_height, -h, h, camera_dnear, camera_dfar,shadow_proj);
}

IGL_INLINE void igl::opengl::ViewerCore::deinitialize_shadow_pass()
{
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

IGL_INLINE void igl::opengl::ViewerCore::draw_shadow_pass(
  ViewerData& data,
  bool /*update_matrices*/)
{
  if (data.dirty)
  {
    data.updateGL(data, data.invert_normals, data.meshgl);
    data.dirty = igl::opengl::MeshGL::DIRTY_NONE;
  }
  data.meshgl.bind_mesh();
  // Send transformations to the GPU as if rendering from shadow point of view
  GLint viewi  = glGetUniformLocation(data.meshgl.shader_mesh,"view");
  GLint proji  = glGetUniformLocation(data.meshgl.shader_mesh,"proj");
  glUniformMatrix4fv(viewi, 1, GL_FALSE, shadow_view.data());
  glUniformMatrix4fv(proji, 1, GL_FALSE, shadow_proj.data());
  glUniform1i(glGetUniformLocation(data.meshgl.shader_mesh,"shadow_pass"),true);
  data.meshgl.draw_mesh(true);
  glUniform1i(glGetUniformLocation(data.meshgl.shader_mesh,"shadow_pass"),false);

}

IGL_INLINE void igl::opengl::ViewerCore::draw_buffer(
  ViewerData& data,
  bool update_matrices,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A)
{
  assert(R.rows() == G.rows() && G.rows() == B.rows() && B.rows() == A.rows());
  assert(R.cols() == G.cols() && G.cols() == B.cols() && B.cols() == A.cols());

  unsigned width = R.rows();
  unsigned height = R.cols();
  if(width == 0 && height == 0)
  {
    width = viewport(2);
    height = viewport(3);
  }
  R.resize(width,height);
  G.resize(width,height);
  B.resize(width,height);
  A.resize(width,height);

  ////////////////////////////////////////////////////////////////////////
  // PREPARE width×height BUFFERS does *not* depend on `data`
  //   framebuffer
  //   textureColorBufferMultiSampled
  //   rbo
  //   intermediateFBO
  //   screenTexture
  //
  ////////////////////////////////////////////////////////////////////////
  // https://learnopengl.com/Advanced-OpenGL/Anti-Aliasing

  // Create an initial multisampled framebuffer
  unsigned int framebuffer;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  // create a multisampled color attachment texture
  unsigned int textureColorBufferMultiSampled;
  glGenTextures(1, &textureColorBufferMultiSampled);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textureColorBufferMultiSampled);
  glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA, width, height, GL_TRUE);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, textureColorBufferMultiSampled, 0);
  // create a (also multisampled) renderbuffer object for depth and stencil attachments
  unsigned int rbo;
  glGenRenderbuffers(1, &rbo);
  glBindRenderbuffer(GL_RENDERBUFFER, rbo);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, width, height);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // configure second post-processing framebuffer
  unsigned int intermediateFBO;
  glGenFramebuffers(1, &intermediateFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, intermediateFBO);
  // create a color attachment texture
  unsigned int screenTexture;
  glGenTextures(1, &screenTexture);
  glBindTexture(GL_TEXTURE_2D, screenTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screenTexture, 0);	// we only need a color buffer
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

  // Clear the buffer
  glClearColor(background_color(0), background_color(1), background_color(2), 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  // Save old viewport
  Eigen::Vector4f viewport_ori = viewport;
  viewport << 0,0,width,height;
  // Draw
  draw(data,update_matrices);
  // Restore viewport
  viewport = viewport_ori;

  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, intermediateFBO);
  glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  glBindFramebuffer(GL_FRAMEBUFFER, intermediateFBO);
  // Copy back in the given Eigen matrices
  GLubyte* pixels = (GLubyte*)calloc(width*height*4,sizeof(GLubyte));
  glReadPixels(0, 0,width, height,GL_RGBA, GL_UNSIGNED_BYTE, pixels);

  // Clean up
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteTextures(1, &screenTexture);
  glDeleteTextures(1, &textureColorBufferMultiSampled);
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteFramebuffers(1, &intermediateFBO);
  glDeleteRenderbuffers(1, &rbo);

  int count = 0;
  for (unsigned j=0; j<height; ++j)
  {
    for (unsigned i=0; i<width; ++i)
    {
      R(i,j) = pixels[count*4+0];
      G(i,j) = pixels[count*4+1];
      B(i,j) = pixels[count*4+2];
      A(i,j) = pixels[count*4+3];
      ++count;
    }
  }
  // Clean up
  free(pixels);
}

// Define uniforms for text labels
IGL_INLINE void igl::opengl::ViewerCore::draw_labels(
  ViewerData& data,
  const igl::opengl::MeshGL::TextGL& labels
){
  glDisable(GL_LINE_SMOOTH); // Clear settings if overlay is activated
  data.meshgl.bind_labels(labels);
  GLint viewi = glGetUniformLocation(data.meshgl.shader_text,"view");
  GLint proji = glGetUniformLocation(data.meshgl.shader_text,"proj");
  glUniformMatrix4fv(viewi, 1, GL_FALSE, view.data());
  glUniformMatrix4fv(proji, 1, GL_FALSE, proj.data());
  // Parameters for mapping characters from font atlass
  float width  = viewport(2);
  float height = viewport(3);
  float text_shift_scale_factor = orthographic ? 0.01 : 0.03;
  float render_scale = (orthographic ? 0.6 : 1.7) * data.label_size;
  glUniform1f(glGetUniformLocation(data.meshgl.shader_text, "TextShiftFactor"), text_shift_scale_factor);
  glUniform3f(glGetUniformLocation(data.meshgl.shader_text, "TextColor"), data.label_color(0), data.label_color(1), data.label_color(2));
  glUniform2f(glGetUniformLocation(data.meshgl.shader_text, "CellSize"), 1.0f / 16, (300.0f / 384) / 6);
  glUniform2f(glGetUniformLocation(data.meshgl.shader_text, "CellOffset"), 0.5 / 256.0, 0.5 / 256.0);
  glUniform2f(glGetUniformLocation(data.meshgl.shader_text, "RenderSize"), 
                                    render_scale * 0.75 * 16 / (width), 
                                    render_scale * 0.75 * 33.33 / (height));
  glUniform2f(glGetUniformLocation(data.meshgl.shader_text, "RenderOrigin"), -2, 2);
  data.meshgl.draw_labels(labels);
  glEnable(GL_DEPTH_TEST);
}

IGL_INLINE void igl::opengl::ViewerCore::set_rotation_type(
  const igl::opengl::ViewerCore::RotationType & value)
{
  using namespace Eigen;
  using namespace std;
  const RotationType old_rotation_type = rotation_type;
  rotation_type = value;
  if(rotation_type == ROTATION_TYPE_TWO_AXIS_VALUATOR_FIXED_UP &&
    old_rotation_type != ROTATION_TYPE_TWO_AXIS_VALUATOR_FIXED_UP)
  {
    snap_to_fixed_up(Quaternionf(trackball_angle),trackball_angle);
  }
}

IGL_INLINE void igl::opengl::ViewerCore::set(unsigned int &property_mask, bool value) const
{
  if (!value)
    unset(property_mask);
  else
    property_mask |= id;
}

IGL_INLINE void igl::opengl::ViewerCore::unset(unsigned int &property_mask) const
{
  property_mask &= ~id;
}

IGL_INLINE void igl::opengl::ViewerCore::toggle(unsigned int &property_mask) const
{
  property_mask ^= id;
}

IGL_INLINE bool igl::opengl::ViewerCore::is_set(unsigned int property_mask) const
{
  return (property_mask & id);
}

IGL_INLINE igl::opengl::ViewerCore::ViewerCore()
{
  // Default colors
  background_color << 0.3f, 0.3f, 0.5f, 1.0f;

  // Default lights settings
  light_position << 0.0f, 0.3f, 0.0f;
  is_directional_light = false;
  is_shadow_mapping = false;
  shadow_width =  2056;
  shadow_height = 2056;

  lighting_factor = 1.0f; //on

  // Default trackball
  trackball_angle = Eigen::Quaternionf::Identity();
  rotation_type = ViewerCore::ROTATION_TYPE_TRACKBALL;
  set_rotation_type(ViewerCore::ROTATION_TYPE_TWO_AXIS_VALUATOR_FIXED_UP);

  // Camera parameters
  camera_base_zoom = 1.0f;
  camera_zoom = 1.0f;
  orthographic = false;
  camera_view_angle = 45.0;
  camera_dnear = 1.0;
  camera_dfar = 100.0;
  camera_base_translation << 0, 0, 0;
  camera_translation << 0, 0, 0;
  camera_eye << 0, 0, 5;
  camera_center << 0, 0, 0;
  camera_up << 0, 1, 0;

  depth_test = true;

  is_animating = false;
  animation_max_fps = 30.;

  viewport.setZero();
}

IGL_INLINE void igl::opengl::ViewerCore::init()
{
  delete_shadow_buffers();
  generate_shadow_buffers();
}

IGL_INLINE void igl::opengl::ViewerCore::shut()
{
  delete_shadow_buffers();
}

IGL_INLINE void igl::opengl::ViewerCore::delete_shadow_buffers()
{
  glDeleteTextures(1,&shadow_depth_tex);
  glDeleteFramebuffers(1,&shadow_depth_fbo);
  glDeleteRenderbuffers(1,&shadow_color_rbo);
}

IGL_INLINE void igl::opengl::ViewerCore::generate_shadow_buffers()
{
  // Create a texture for writing the shadow map depth values into
  {
    glDeleteTextures(1,&shadow_depth_tex);
    glGenTextures(1, &shadow_depth_tex);
    glBindTexture(GL_TEXTURE_2D, shadow_depth_tex);
    // Should this be using double/float precision?
    glTexImage2D(
      GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT,
      shadow_width,
      shadow_height,
      0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, Eigen::Vector4f(1,1,1,1).data() );
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  // Generate a framebuffer with depth attached to this texture and color
  // attached to a render buffer object
  glGenFramebuffers(1, &shadow_depth_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, shadow_depth_fbo);
  // Attach depth texture
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
      shadow_depth_tex,0);
  // Generate a render buffer to write colors into. Low precision we don't
  // care about them. Is there a way to not write/compute them at? Probably
  // just need to change fragment shader.
  glGenRenderbuffers(1,&shadow_color_rbo);
  glBindRenderbuffer(GL_RENDERBUFFER,shadow_color_rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, shadow_width, shadow_height);
  // Attach color buffer
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_RENDERBUFFER, shadow_color_rbo);
  //Does the GPU support current FBO configuration?
  GLenum status;
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  switch(status)
  {
    case GL_FRAMEBUFFER_COMPLETE:
      break;
    default:
      printf("[ViewerCore] Error: We failed to set up a good FBO: %d\n",status);
      assert(false);
  }
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

