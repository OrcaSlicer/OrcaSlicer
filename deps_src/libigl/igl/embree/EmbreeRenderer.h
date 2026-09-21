// This file is part of libigl, a simple c++ geometry processing library.
//
//
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com>
//               2013 Alec Jacobson <alecjacobson@gmail.com>
//               2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
//

#ifndef IGL_EMBREE_EMBREE_RENDERER_H
#define IGL_EMBREE_EMBREE_RENDERER_H

#include "../colormap.h"

#include <Eigen/Geometry>
#include <Eigen/Core>
#include <Eigen/Geometry>

#include <embree4/rtcore.h>
#include <embree4/rtcore_ray.h>
#include <iostream>
#include <vector>

#include "EmbreeDevice.h"


namespace igl
{
  namespace embree
  {
    /// @private
    /// embree-based mesh renderer
    class EmbreeRenderer
    {
    public:
      typedef Eigen::RowVector3f Vec3f;

      struct Hit
      {
        int id;    // primitive id
        int gid;   // geometry id
        float u,v; // barycentric coordinates
        float t;   // distance = direction*t to intersection
        Vec3f N;    // element normal
      };

    public:
      typedef Eigen::Matrix<float,Eigen::Dynamic,3> PointMatrixType;
      typedef Eigen::Matrix<float,Eigen::Dynamic,3> ColorMatrixType;
      typedef Eigen::Matrix<int,  Eigen::Dynamic,3> FaceMatrixType;
      typedef Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic> PixelMatrixType;

    public:
      EmbreeRenderer();
    private:
      // Copying and assignment are not allowed.
      EmbreeRenderer(const EmbreeRenderer & that);
      EmbreeRenderer & operator=(const EmbreeRenderer &);
    public:
      virtual ~EmbreeRenderer();

      // Specify mesh, this call reinitializes embree structures
      // Inputs:
      //   V  #V x dim matrix of vertex coordinates
      //   F  #F x simplex_size  matrix of indices of simplex corners into V
      //   is_static - optimize for static thene (HQ rendering)
      template <typename DerivedV, typename DerivedF>
      void set_mesh(const Eigen::MatrixBase<DerivedV> & V,
                    const Eigen::MatrixBase<DerivedF> & F,
                    bool is_static=true);

      // Specify per-vertex or per-face color
      // Inputs:
      //   C  #V x 3 matrix of vertex colors
      //    or #F x 3 matrix of face colors
      //    or 1 x 3 matrix of uniform color
      template <typename DerivedC>
      void set_colors(const Eigen::MatrixBase<DerivedC> & C);

      // Use min(D) and max(D) to set caxis.
      template <typename DerivedD>
      void set_data(const Eigen::MatrixBase<DerivedD> & D,
                    igl::ColorMapType cmap = igl::COLOR_MAP_TYPE_VIRIDIS);

      // Specify per-vertex or per-face scalar field
      //   that will be converted to color using jet color map
      // Inputs:
      //   caxis_min  caxis minimum bound
      //   caxis_max  caxis maximum bound
      //   D  #V by 1 list of scalar values
      //   cmap colormap type
      //   num_steps number of intervals to discretize the colormap
      template <typename DerivedD, typename T>
      void set_data(
        const Eigen::MatrixBase<DerivedD> & D,
        T caxis_min,
        T caxis_max,
        igl::ColorMapType cmap = igl::COLOR_MAP_TYPE_VIRIDIS);

      // Specify mesh rotation
      // Inputs:
      //   r  3 x 3 rotaton matrix
      template <typename Derivedr>
      void set_rot(const Eigen::MatrixBase<Derivedr> &r);

      // Specify mesh magnification
      // Inputs:
      //   z  magnification ratio
      template <typename T>
      void set_zoom(T z);

      // Specify mesh translation
      // Inputs:
      //   tr  translation vector
      template <typename Derivedtr>
      void set_translation(const Eigen::MatrixBase<Derivedtr> &tr);

      // Specify that color is face based
      // Inputs:
      //    f - face or vertex colours
      void set_face_based(bool f);

      // Use orthographic projection
      // Inputs:
      //    f - orthographic or perspective projection
      void set_orthographic(bool f );


      // Render both sides of triangles
      // Inputs:
      //    f - double sided
      void set_double_sided(bool f);

      // render full buffer
      // Outputs:
      //   all outputs should have the same size (size of the output picture)
      //     area outside of the visible object will have zero alpha component (transparant)
      //   R - red channel
      //   G - green channel
      //   B - blue channel
      //   A - alpha channel
      void render_buffer(PixelMatrixType &R,
                         PixelMatrixType &G,
                         PixelMatrixType &B,
                         PixelMatrixType &A);

      // Given a ray find the first hit
      //
      // Inputs:
      //   origin     3d origin point of ray
      //   direction  3d (not necessarily normalized) direction vector of ray
      //   tnear      start of ray segment
      //   tfar       end of ray segment
      //   mask      a 32 bit mask to identify active geometries.
      // Output:
      //   hit        information about hit
      // Returns true if and only if there was a hit
      bool intersect_ray(
        const Eigen::RowVector3f& origin,
        const Eigen::RowVector3f& direction,
        Hit & hit,
        float tnear = 0,
        float tfar = std::numeric_limits<float>::infinity(),
        int mask = 0xFFFFFFFF) const;

    private:

      // Initialize with a given mesh.
      //
      // Inputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of Oriented triangles
      //   isStatic  scene is optimized for static geometry
      // Side effects:
      //   The first time this is ever called the embree engine is initialized.
      void init(
        const PointMatrixType& V,
        const FaceMatrixType& F,
        bool isStatic = false);

      // Initialize embree with a given mesh.
      //
      // Inputs:
      //   V  vector of #V by 3 list of vertex positions for each geometry
      //   F  vector of #F by 3 list of Oriented triangles for each geometry
      //   masks  a 32 bit mask to identify active geometries.
      //   isStatic  scene is optimized for static geometry
      // Side effects:
      //   The first time this is ever called the embree engine is initialized.
      void init(
        const std::vector<const PointMatrixType*>& V,
        const std::vector<const FaceMatrixType*>& F,
        const std::vector<int>& masks,
        bool isStatic = false);


      // Deinitialize embree datasctructures for current mesh.  Also called on
      // destruction: no need to call if you just want to init() once and
      // destroy.
      void deinit();
      // initialize view parameters
      void init_view();

      // scene data
      PointMatrixType V; // vertices
      FaceMatrixType  F; // faces
      ColorMatrixType C; // colours

      Eigen::RowVector3f uC; // uniform color

      bool face_based;
      bool uniform_color;
      bool double_sided ;

      // Camera parameters
      float camera_base_zoom;
      float camera_zoom;

      Eigen::Vector3f camera_base_translation;
      Eigen::Vector3f camera_translation;
      Eigen::Vector3f camera_eye;
      Eigen::Vector3f camera_up;
      Eigen::Vector3f camera_center;
      float camera_view_angle;
      float camera_dnear;
      float camera_dfar;

      // projection matrixes
      Eigen::Matrix4f view;
      Eigen::Matrix4f proj;
      Eigen::Matrix4f norm;

      Eigen::Matrix3f rot_matrix;

      bool orthographic;

      // embree data
      RTCScene scene;
      unsigned geomID;
      bool initialized;

      RTCDevice device;

      void create_ray(
        RTCRayHit& ray,
        const Eigen::RowVector3f& origin,
        const Eigen::RowVector3f& direction,
        float tnear,
        float tfar,
        int mask) const;

    };
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "EmbreeRenderer.cpp"
#endif
#endif //IGL_EMBREE_EMBREE_RENDERER_H
