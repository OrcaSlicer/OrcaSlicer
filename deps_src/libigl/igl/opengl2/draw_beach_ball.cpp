// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "draw_beach_ball.h"
#include "gl.h"

// I'm not sure why windows would need it this way:
// http://lists.cairographics.org/archives/cairo/2008-January/012722.html
#ifdef _MSC_VER
#define SAFE_INLINE __inline
#else
#define SAFE_INLINE inline
#endif

#include <vector>
#include <cmath>
#include <iostream>

// Most of this implementation comes from the AntTweakBar source code:
// TwMgr.cpp, TwMgr.h, TwColor.h, TwColor.cpp, TwOpenGL.h and TwOpenGL.cpp

////////////////////////////////////////////////////////////////////////////
// Begin Copied Straight from AntTweakBar
////////////////////////////////////////////////////////////////////////////
enum EArrowParts     { ARROW_CONE, ARROW_CONE_CAP, ARROW_CYL, ARROW_CYL_CAP };

template <typename T> SAFE_INLINE const T& TClamp(const T& X, const T& Limit1, const T& Limit2)
{
    if( Limit1<Limit2 )
        return (X<=Limit1) ? Limit1 : ( (X>=Limit2) ? Limit2 : X );
    else
        return (X<=Limit2) ? Limit2 : ( (X>=Limit1) ? Limit1 : X );
}

typedef unsigned int color32;
static SAFE_INLINE color32 Color32FromARGBi(int A, int R, int G, int B)
{
    return (((color32)TClamp(A, 0, 255))<<24) | (((color32)TClamp(R, 0, 255))<<16) | (((color32)TClamp(G, 0, 255))<<8) | ((color32)TClamp(B, 0, 255));
}

static SAFE_INLINE color32 Color32FromARGBf(float A, float R, float G, float B)
{
    return (((color32)TClamp(A*256.0f, 0.0f, 255.0f))<<24) | (((color32)TClamp(R*256.0f, 0.0f, 255.0f))<<16) | (((color32)TClamp(G*256.0f, 0.0f, 255.0f))<<8) | ((color32)TClamp(B*256.0f, 0.0f, 255.0f));
}

static SAFE_INLINE void Color32ToARGBi(color32 Color, int *A, int *R, int *G, int *B)
{
    if(A) *A = (Color>>24)&0xff;
    if(R) *R = (Color>>16)&0xff;
    if(G) *G = (Color>>8)&0xff;
    if(B) *B = Color&0xff;
}

static SAFE_INLINE void Color32ToARGBf(color32 Color, float *A, float *R, float *G, float *B)
{
    if(A) *A = (1.0f/255.0f)*float((Color>>24)&0xff);
    if(R) *R = (1.0f/255.0f)*float((Color>>16)&0xff);
    if(G) *G = (1.0f/255.0f)*float((Color>>8)&0xff);
    if(B) *B = (1.0f/255.0f)*float(Color&0xff);
}

static color32 ColorBlend(color32 Color1, color32 Color2, float S)
{
    float a1, r1, g1, b1, a2, r2, g2, b2;
    Color32ToARGBf(Color1, &a1, &r1, &g1, &b1);
    Color32ToARGBf(Color2, &a2, &r2, &g2, &b2);
    float t = 1.0f-S;
    return Color32FromARGBf(t*a1+S*a2, t*r1+S*r2, t*g1+S*g2, t*b1+S*b2);
}

static std::vector<float>   s_SphTri;
static std::vector<color32> s_SphCol;
static void CreateSphere()
{
    const int SUBDIV = 7;
    s_SphTri.clear();
    s_SphCol.clear();

    const float A[8*3] = { 1,0,0, 0,0,-1, -1,0,0, 0,0,1,   0,0,1,  1,0,0,  0,0,-1, -1,0,0 };
    const float B[8*3] = { 0,1,0, 0,1,0,  0,1,0,  0,1,0,   0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0 };
    const float C[8*3] = { 0,0,1, 1,0,0,  0,0,-1, -1,0,0,  1,0,0,  0,0,-1, -1,0,0, 0,0,1  };
    //const color32 COL_A[8] = { 0xffff8080, 0xff000080, 0xff800000, 0xff8080ff,  0xff8080ff, 0xffff8080, 0xff000080, 0xff800000 };
    //const color32 COL_B[8] = { 0xff80ff80, 0xff80ff80, 0xff80ff80, 0xff80ff80,  0xff008000, 0xff008000, 0xff008000, 0xff008000 };
    //const color32 COL_C[8] = { 0xff8080ff, 0xffff8080, 0xff000080, 0xff800000,  0xffff8080, 0xff000080, 0xff800000, 0xff8080ff };
    const color32 COL_A[8] = { 0xffffffff, 0xffffff40, 0xff40ff40, 0xff40ffff,  0xffff40ff, 0xffff4040, 0xff404040, 0xff4040ff };
    const color32 COL_B[8] = { 0xffffffff, 0xffffff40, 0xff40ff40, 0xff40ffff,  0xffff40ff, 0xffff4040, 0xff404040, 0xff4040ff };
    const color32 COL_C[8] = { 0xffffffff, 0xffffff40, 0xff40ff40, 0xff40ffff,  0xffff40ff, 0xffff4040, 0xff404040, 0xff4040ff };

    int i, j, k, l;
    float xa, ya, za, xb, yb, zb, xc, yc, zc, x, y, z, norm, u[3], v[3];
    color32 col;
    for( i=0; i<8; ++i )
    {
        xa = A[3*i+0]; ya = A[3*i+1]; za = A[3*i+2];
        xb = B[3*i+0]; yb = B[3*i+1]; zb = B[3*i+2];
        xc = C[3*i+0]; yc = C[3*i+1]; zc = C[3*i+2];
        for( j=0; j<=SUBDIV; ++j )
            for( k=0; k<=2*(SUBDIV-j); ++k )
            {
                if( k%2==0 )
                {
                    u[0] = ((float)j)/(SUBDIV+1);
                    v[0] = ((float)(k/2))/(SUBDIV+1);
                    u[1] = ((float)(j+1))/(SUBDIV+1);
                    v[1] = ((float)(k/2))/(SUBDIV+1);
                    u[2] = ((float)j)/(SUBDIV+1);
                    v[2] = ((float)(k/2+1))/(SUBDIV+1);
                }
                else
                {
                    u[0] = ((float)j)/(SUBDIV+1);
                    v[0] = ((float)(k/2+1))/(SUBDIV+1);
                    u[1] = ((float)(j+1))/(SUBDIV+1);
                    v[1] = ((float)(k/2))/(SUBDIV+1);
                    u[2] = ((float)(j+1))/(SUBDIV+1);
                    v[2] = ((float)(k/2+1))/(SUBDIV+1);
                }

                for( l=0; l<3; ++l )
                {
                    x = (1.0f-u[l]-v[l])*xa + u[l]*xb + v[l]*xc;
                    y = (1.0f-u[l]-v[l])*ya + u[l]*yb + v[l]*yc;
                    z = (1.0f-u[l]-v[l])*za + u[l]*zb + v[l]*zc;
                    norm = sqrtf(x*x+y*y+z*z);
                    x /= norm; y /= norm; z /= norm;
                    s_SphTri.push_back(x); s_SphTri.push_back(y); s_SphTri.push_back(z);
static const float  FLOAT_EPS     = 1.0e-7f;
                    if( u[l]+v[l]>FLOAT_EPS )
                        col = ColorBlend(COL_A[i], ColorBlend(COL_B[i], COL_C[i], v[l]/(u[l]+v[l])), u[l]+v[l]);
                    else
                        col = COL_A[i];
                    //if( (j==0 && k==0) || (j==0 && k==2*SUBDIV) || (j==SUBDIV && k==0) )
                    //  col = 0xffff0000;
                    s_SphCol.push_back(col);
                }
            }
    }
    //s_SphTriProj.clear();
    //s_SphTriProj.resize(2*s_SphCol.size(), 0);
    //s_SphColLight.clear();
    //s_SphColLight.resize(s_SphCol.size(), 0);
}

static std::vector<float> s_ArrowTri[4];
static std::vector<float> s_ArrowNorm[4];
static void CreateArrow()
{
    const int   SUBDIV  = 15;
    const float CYL_RADIUS  = 0.08f;
    const float CONE_RADIUS = 0.16f;
    const float CONE_LENGTH = 0.25f;
    const float ARROW_BGN = -1.1f;
    const float ARROW_END = 1.15f;
    int i;
    for(i=0; i<4; ++i)
    {
        s_ArrowTri[i].clear();
        s_ArrowNorm[i].clear();
    }
    
    float x0, x1, y0, y1, z0, z1, a0, a1, nx, nn;
    for(i=0; i<SUBDIV; ++i)
    {
static const float  FLOAT_PI      = 3.14159265358979323846f;
        a0 = 2.0f*FLOAT_PI*(float(i))/SUBDIV;
        a1 = 2.0f*FLOAT_PI*(float(i+1))/SUBDIV;
        x0 = ARROW_BGN;
        x1 = ARROW_END-CONE_LENGTH;
        y0 = cosf(a0);
        z0 = sinf(a0);
        y1 = cosf(a1);
        z1 = sinf(a1);
        s_ArrowTri[ARROW_CYL].push_back(x1); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*y0); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*z0);
        s_ArrowTri[ARROW_CYL].push_back(x0); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*y0); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*z0);
        s_ArrowTri[ARROW_CYL].push_back(x0); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*y1); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*z1);
        s_ArrowTri[ARROW_CYL].push_back(x1); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*y0); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*z0);
        s_ArrowTri[ARROW_CYL].push_back(x0); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*y1); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*z1);
        s_ArrowTri[ARROW_CYL].push_back(x1); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*y1); s_ArrowTri[ARROW_CYL].push_back(CYL_RADIUS*z1);
        s_ArrowNorm[ARROW_CYL].push_back(0); s_ArrowNorm[ARROW_CYL].push_back(y0); s_ArrowNorm[ARROW_CYL].push_back(z0);
        s_ArrowNorm[ARROW_CYL].push_back(0); s_ArrowNorm[ARROW_CYL].push_back(y0); s_ArrowNorm[ARROW_CYL].push_back(z0);
        s_ArrowNorm[ARROW_CYL].push_back(0); s_ArrowNorm[ARROW_CYL].push_back(y1); s_ArrowNorm[ARROW_CYL].push_back(z1);
        s_ArrowNorm[ARROW_CYL].push_back(0); s_ArrowNorm[ARROW_CYL].push_back(y0); s_ArrowNorm[ARROW_CYL].push_back(z0);
        s_ArrowNorm[ARROW_CYL].push_back(0); s_ArrowNorm[ARROW_CYL].push_back(y1); s_ArrowNorm[ARROW_CYL].push_back(z1);
        s_ArrowNorm[ARROW_CYL].push_back(0); s_ArrowNorm[ARROW_CYL].push_back(y1); s_ArrowNorm[ARROW_CYL].push_back(z1);
        s_ArrowTri[ARROW_CYL_CAP].push_back(x0); s_ArrowTri[ARROW_CYL_CAP].push_back(0); s_ArrowTri[ARROW_CYL_CAP].push_back(0);
        s_ArrowTri[ARROW_CYL_CAP].push_back(x0); s_ArrowTri[ARROW_CYL_CAP].push_back(CYL_RADIUS*y1); s_ArrowTri[ARROW_CYL_CAP].push_back(CYL_RADIUS*z1);
        s_ArrowTri[ARROW_CYL_CAP].push_back(x0); s_ArrowTri[ARROW_CYL_CAP].push_back(CYL_RADIUS*y0); s_ArrowTri[ARROW_CYL_CAP].push_back(CYL_RADIUS*z0);
        s_ArrowNorm[ARROW_CYL_CAP].push_back(-1); s_ArrowNorm[ARROW_CYL_CAP].push_back(0); s_ArrowNorm[ARROW_CYL_CAP].push_back(0);
        s_ArrowNorm[ARROW_CYL_CAP].push_back(-1); s_ArrowNorm[ARROW_CYL_CAP].push_back(0); s_ArrowNorm[ARROW_CYL_CAP].push_back(0);
        s_ArrowNorm[ARROW_CYL_CAP].push_back(-1); s_ArrowNorm[ARROW_CYL_CAP].push_back(0); s_ArrowNorm[ARROW_CYL_CAP].push_back(0);
        x0 = ARROW_END-CONE_LENGTH;
        x1 = ARROW_END;
        nx = CONE_RADIUS/(x1-x0);
        nn = 1.0f/sqrtf(nx*nx+1);
        s_ArrowTri[ARROW_CONE].push_back(x1); s_ArrowTri[ARROW_CONE].push_back(0); s_ArrowTri[ARROW_CONE].push_back(0);
        s_ArrowTri[ARROW_CONE].push_back(x0); s_ArrowTri[ARROW_CONE].push_back(CONE_RADIUS*y0); s_ArrowTri[ARROW_CONE].push_back(CONE_RADIUS*z0);
        s_ArrowTri[ARROW_CONE].push_back(x0); s_ArrowTri[ARROW_CONE].push_back(CONE_RADIUS*y1); s_ArrowTri[ARROW_CONE].push_back(CONE_RADIUS*z1);
        s_ArrowTri[ARROW_CONE].push_back(x1); s_ArrowTri[ARROW_CONE].push_back(0); s_ArrowTri[ARROW_CONE].push_back(0);
        s_ArrowTri[ARROW_CONE].push_back(x0); s_ArrowTri[ARROW_CONE].push_back(CONE_RADIUS*y1); s_ArrowTri[ARROW_CONE].push_back(CONE_RADIUS*z1);
        s_ArrowTri[ARROW_CONE].push_back(x1); s_ArrowTri[ARROW_CONE].push_back(0); s_ArrowTri[ARROW_CONE].push_back(0);
        s_ArrowNorm[ARROW_CONE].push_back(nn*nx); s_ArrowNorm[ARROW_CONE].push_back(nn*y0); s_ArrowNorm[ARROW_CONE].push_back(nn*z0);
        s_ArrowNorm[ARROW_CONE].push_back(nn*nx); s_ArrowNorm[ARROW_CONE].push_back(nn*y0); s_ArrowNorm[ARROW_CONE].push_back(nn*z0);
        s_ArrowNorm[ARROW_CONE].push_back(nn*nx); s_ArrowNorm[ARROW_CONE].push_back(nn*y1); s_ArrowNorm[ARROW_CONE].push_back(nn*z1);
        s_ArrowNorm[ARROW_CONE].push_back(nn*nx); s_ArrowNorm[ARROW_CONE].push_back(nn*y0); s_ArrowNorm[ARROW_CONE].push_back(nn*z0);
        s_ArrowNorm[ARROW_CONE].push_back(nn*nx); s_ArrowNorm[ARROW_CONE].push_back(nn*y1); s_ArrowNorm[ARROW_CONE].push_back(nn*z1);
        s_ArrowNorm[ARROW_CONE].push_back(nn*nx); s_ArrowNorm[ARROW_CONE].push_back(nn*y1); s_ArrowNorm[ARROW_CONE].push_back(nn*z1);
        s_ArrowTri[ARROW_CONE_CAP].push_back(x0); s_ArrowTri[ARROW_CONE_CAP].push_back(0); s_ArrowTri[ARROW_CONE_CAP].push_back(0);
        s_ArrowTri[ARROW_CONE_CAP].push_back(x0); s_ArrowTri[ARROW_CONE_CAP].push_back(CONE_RADIUS*y1); s_ArrowTri[ARROW_CONE_CAP].push_back(CONE_RADIUS*z1);
        s_ArrowTri[ARROW_CONE_CAP].push_back(x0); s_ArrowTri[ARROW_CONE_CAP].push_back(CONE_RADIUS*y0); s_ArrowTri[ARROW_CONE_CAP].push_back(CONE_RADIUS*z0);
        s_ArrowNorm[ARROW_CONE_CAP].push_back(-1); s_ArrowNorm[ARROW_CONE_CAP].push_back(0); s_ArrowNorm[ARROW_CONE_CAP].push_back(0);
        s_ArrowNorm[ARROW_CONE_CAP].push_back(-1); s_ArrowNorm[ARROW_CONE_CAP].push_back(0); s_ArrowNorm[ARROW_CONE_CAP].push_back(0);
        s_ArrowNorm[ARROW_CONE_CAP].push_back(-1); s_ArrowNorm[ARROW_CONE_CAP].push_back(0); s_ArrowNorm[ARROW_CONE_CAP].push_back(0);
    }

    //for(i=0; i<4; ++i)
    //{
    //    s_ArrowTriProj[i].clear();
    //    s_ArrowTriProj[i].resize(2*(s_ArrowTri[i].size()/3), 0);
    //    s_ArrowColLight[i].clear();
    //    s_ArrowColLight[i].resize(s_ArrowTri[i].size()/3, 0);
    //}
}

////////////////////////////////////////////////////////////////////////////
// End Copied Straight from AntTweakBar
////////////////////////////////////////////////////////////////////////////

IGL_INLINE void igl::opengl2::draw_beach_ball()
{
  using namespace std;

  CreateSphere();
  // Keep track of opengl settings
  int cm;
  glGetIntegerv(GL_COLOR_MATERIAL,&cm);
  // Draw triangles
  glEnable(GL_COLOR_MATERIAL);
  glColorMaterial(GL_FRONT_AND_BACK,GL_DIFFUSE);
  float mat_ambient[4] = {0.1,0.1,0.1,1.0};
  float mat_specular[4] = {0.0,0.0,0.0,1.0};
  float mat_shininess = 1;
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT,  mat_ambient);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
  glMaterialf( GL_FRONT_AND_BACK, GL_SHININESS, mat_shininess);

  glPushMatrix();
  glScalef(0.7,0.7,0.7);
  glEnable(GL_NORMALIZE);
  glBegin(GL_TRIANGLES);
  for(int i = 0;i<(int)s_SphCol.size();i++)
  {
    glNormal3fv(&s_SphTri[i*3]);
    glColor4ub(GLubyte(s_SphCol[i]>>16), GLubyte(s_SphCol[i]>>8), GLubyte(s_SphCol[i]), GLubyte(s_SphCol[i]>>24));
    glVertex3fv(&s_SphTri[i*3]);
  }
  glEnd();
  glPopMatrix();

  CreateArrow();
  for(int k = 0;k<3;k++)
  {
    glPushMatrix();
    glColor3f(k==0,k==1,k==2);
    glRotatef((k==2?-1.0:1.0)*90,k==0,k==2,k==1);
    glBegin(GL_TRIANGLES);
    for(int j = 0;j<4;j++)
    {
      for(int i = 0;i<(int)s_ArrowTri[j].size();i+=3)
      {
        glNormal3fv(&s_ArrowNorm[j][i]);
        glVertex3fv(&s_ArrowTri[j][i]);
      }
    }
    glEnd();
    glPopMatrix();
  }

  (cm ? glEnable(GL_COLOR_MATERIAL):glDisable(GL_COLOR_MATERIAL));
}
