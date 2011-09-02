// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "lens_flare.h"

#include "../C_STR.h"
#include "unproject.h"
#include "project.h"
#include "shine_textures.h"
#include "flare_textures.h"

#include <iostream>
#include <stdint.h>

// http://www.opengl.org/archives/resources/features/KilgardTechniques/LensFlare/glflare.c

IGL_INLINE void igl::opengl2::lens_flare_load_textures(
  std::vector<GLuint> & shine_id,
  std::vector<GLuint> & flare_id)
{

  const auto setup_texture =[](
    const uint8_t * texture,
    const int width,
    const int height,
    GLuint texobj,
    GLenum minFilter, GLenum maxFilter)
  {
    glBindTexture(GL_TEXTURE_2D, texobj);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, maxFilter);
    glTexImage2D(GL_TEXTURE_2D, 0, 1, width, height, 0,
      GL_LUMINANCE, GL_UNSIGNED_BYTE, texture);
  };

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  shine_id.resize(10);
  glGenTextures(10,&shine_id[0]);
  for (int i = 0; i < (int)shine_id.size(); i++) {
    setup_texture(
      SHINE_TEXTURES[i],
      SHINE_TEXTURE_WIDTHS[i],
      SHINE_TEXTURE_HEIGHTS[i],
      shine_id[i], GL_LINEAR, GL_LINEAR);
  }
  flare_id.resize(6);
  glGenTextures(6,&flare_id[0]);
  for (int i = 0; i < (int)flare_id.size(); i++) {
    setup_texture(
      FLARE_TEXTURES[i],
      FLARE_TEXTURE_WIDTHS[i],
      FLARE_TEXTURE_HEIGHTS[i],
      flare_id[i], GL_LINEAR, GL_LINEAR);
  }
}

IGL_INLINE void igl::opengl2::lens_flare_create(
  const float * A,
  const float * B,
  const float * C,
  std::vector<igl::opengl2::Flare> & flares)
{
  using namespace std;
  flares.resize(12);
  /* Shines */
  flares[0] = Flare(-1, 1.0f, 0.1f, C, 1.0);
  flares[1] = Flare(-1, 1.0f, 0.15f, B, 1.0);
  flares[2] = Flare(-1, 1.0f, 0.35f, A, 1.0);

  /* Flares */
  flares[3] =  Flare(2, 1.3f, 0.04f, A, 0.6);
  flares[4] =  Flare(3, 1.0f, 0.1f, A, 0.4);
  flares[5] =  Flare(1, 0.5f, 0.2f, A, 0.3);
  flares[6] =  Flare(3, 0.2f, 0.05f, A, 0.3);
  flares[7] =  Flare(0, 0.0f, 0.04f, A, 0.3);
  flares[8] =  Flare(5, -0.25f, 0.07f, A, 0.5);
  flares[9] =  Flare(5, -0.4f, 0.02f, A, 0.6);
  flares[10] = Flare(5, -0.6f, 0.04f, A, 0.4);
  flares[11] = Flare(5, -1.0f, 0.03f, A, 0.2);
}

IGL_INLINE void igl::opengl2::lens_flare_draw(
  const std::vector<igl::opengl2::Flare> & flares,
  const std::vector<GLuint> & shine_ids,
  const std::vector<GLuint> & flare_ids,
  const Eigen::Vector3f & light,
  const float near_clip,
  int & shine_tic)
{
  bool ot2 = glIsEnabled(GL_TEXTURE_2D);
  bool ob = glIsEnabled(GL_BLEND);
  bool odt = glIsEnabled(GL_DEPTH_TEST);
  bool ocm = glIsEnabled(GL_COLOR_MATERIAL);
  bool ol = glIsEnabled(GL_LIGHTING);
  int obsa,obda,odf,odwm;
  glGetIntegerv(GL_BLEND_SRC_ALPHA,&obsa);
  glGetIntegerv(GL_BLEND_DST_ALPHA,&obda);
  glGetIntegerv(GL_DEPTH_FUNC,&odf);
  glGetIntegerv(GL_DEPTH_WRITEMASK,&odwm);

  glDisable(GL_COLOR_MATERIAL);
  glEnable(GL_DEPTH_TEST);
  //glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_TEXTURE_2D);
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);

  using namespace Eigen;
  using namespace std;

  //// view_dir  direction from eye to position it is looking at
  //const Vector3f view_dir =  (at - from).normalized();

  //// near_clip  distance from eye to near clipping plane along view_dir
  //// center   position on near clipping plane along viewdir from eye
  //const Vector3f center =  from + near_clip*view_dir;

  Vector3f plight = project(light);
  // Orthogonal vectors to view direction at light
  Vector3f psx = plight;
  psx(0) += 1;
  Vector3f psy = plight;
  psy(1) += 1;

  // axis toward center
  int vp[4];
  glGetIntegerv(GL_VIEWPORT,vp);
  Vector3f center = unproject(Vector3f(0.5*vp[2],0.5*vp[3],plight[2]-1e-3));
  //Vector3f center(0,0,1);
  Vector3f axis = light-center;
  //glLineWidth(4.);
  //glColor3f(1,0,0);
  //glBegin(GL_LINES);
  //glVertex3fv(center.data());
  //glVertex3fv(light.data());
  //glEnd();

  const Vector3f SX = unproject(psx).normalized();
  const Vector3f SY = unproject(psy).normalized();

  for(int i = 0; i < (int)flares.size(); i++)
  {
    const Vector3f sx = flares[i].scale * SX;
    const Vector3f sy = flares[i].scale * SY;
    glColor3fv(flares[i].color);
    if (flares[i].type < 0) {
      glBindTexture(GL_TEXTURE_2D, shine_ids[shine_tic]);
      shine_tic = (shine_tic + 1) % shine_ids.size();
    } else 
    {
      glBindTexture(GL_TEXTURE_2D, flare_ids[flares[i].type]);
    }

    /* position = center + flare[i].loc * axis */
    const Vector3f position = center + flares[i].loc * axis;
    Vector3f tmp;

    glBegin(GL_QUADS);
    glTexCoord2f(0.0, 0.0);
    tmp =  position +  sx;
    tmp =  tmp +  sy;
    glVertex3fv(tmp.data());

    glTexCoord2f(1.0, 0.0);
    tmp =  position -  sx;
    tmp =  tmp +  sy;
    glVertex3fv(tmp.data());

    glTexCoord2f(1.0, 1.0);
    tmp =  position -  sx;
    tmp =  tmp -  sy;
    glVertex3fv(tmp.data());

    glTexCoord2f(0.0, 1.0);
    tmp =  position +  sx;
    tmp =  tmp -  sy;
    glVertex3fv(tmp.data());
    glEnd();
  }
  ot2?glEnable(GL_TEXTURE_2D):glDisable(GL_TEXTURE_2D);
  ob?glEnable(GL_BLEND):glDisable(GL_BLEND);
  odt?glEnable(GL_DEPTH_TEST):glDisable(GL_DEPTH_TEST);
  ocm?glEnable(GL_COLOR_MATERIAL):glDisable(GL_COLOR_MATERIAL);
  ol?glEnable(GL_LIGHTING):glDisable(GL_LIGHTING);
  glBlendFunc(obsa,obda);
  glDepthFunc(odf);
  glDepthMask(odwm);
}
