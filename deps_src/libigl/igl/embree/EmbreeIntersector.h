// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//               2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
// igl function interface for Embree2.2
//
// Necessary changes to switch from previous Embree versions:
// * Use igl:Hit instead of embree:Hit (where id0 -> id)
// * For Embree2.2
// * Uncomment #define __USE_RAY_MASK__ in platform.h to enable masking

#ifndef IGL_EMBREE_EMBREE_INTERSECTOR_H
#define IGL_EMBREE_EMBREE_INTERSECTOR_H

#include "../Hit.h"
#include <Eigen/Geometry>
#include <Eigen/Core>
#include <Eigen/Geometry>

#include "embree2/rtcore.h"
#include "embree2/rtcore_ray.h"
#include <iostream>
#include <vector>

namespace igl
{
  namespace embree
  {
    class EmbreeIntersector
    {
    public:
      // Initialize embree engine. This will be called on instance `init()`
      // calls. If already inited then this function does nothing: it is harmless
      // to call more than once.
      static inline void global_init();
    private:
      // Deinitialize the embree engine.
      static inline void global_deinit();
    public:
      typedef Eigen::Matrix<float,Eigen::Dynamic,3> PointMatrixType;
      typedef Eigen::Matrix<int,Eigen::Dynamic,3> FaceMatrixType;
    public:
      inline EmbreeIntersector();
    private:
      // Copying and assignment are not allowed.
      inline EmbreeIntersector(const EmbreeIntersector & that);
      inline EmbreeIntersector & operator=(const EmbreeIntersector &);
    public:
      virtual inline ~EmbreeIntersector();

      // Initialize with a given mesh.
      //
      // Inputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of Oriented triangles
      //   isStatic  scene is optimized for static geometry
      // Side effects:
      //   The first time this is ever called the embree engine is initialized.
      inline void init(
        const PointMatrixType& V,
        const FaceMatrixType& F,
        bool isStatic = false);

      // Initialize with a given mesh.
      //
      // Inputs:
      //   V  vector of #V by 3 list of vertex positions for each geometry
      //   F  vector of #F by 3 list of Oriented triangles for each geometry
      //   masks  a 32 bit mask to identify active geometries.
      //   isStatic  scene is optimized for static geometry
      // Side effects:
      //   The first time this is ever called the embree engine is initialized.
      inline void init(
        const std::vector<const PointMatrixType*>& V,
        const std::vector<const FaceMatrixType*>& F,
        const std::vector<int>& masks,
        bool isStatic = false);

      // Deinitialize embree datasctructures for current mesh.  Also called on
      // destruction: no need to call if you just want to init() once and
      // destroy.
      inline void deinit();

      // Given a ray find the first hit
      //
      // Inputs:
      //   origin     3d origin point of ray
      //   direction  3d (not necessarily normalized) direction vector of ray
      //   tnear      start of ray segment
      //   tfar       end of ray segment
      //   masks      a 32 bit mask to identify active geometries.
      // Output:
      //   hit        information about hit
      // Returns true if and only if there was a hit
      inline bool intersectRay(
        const Eigen::RowVector3f& origin,
        const Eigen::RowVector3f& direction,
        Hit& hit,
        float tnear = 0,
        float tfar = std::numeric_limits<float>::infinity(),
        int mask = 0xFFFFFFFF) const;

      // Given a ray find the first hit
      // This is a conservative hit test where multiple rays within a small radius
      // will be tested and only the closesest hit is returned.
      //
      // Inputs:
      //   origin     3d origin point of ray
      //   direction  3d (not necessarily normalized) direction vector of ray
      //   tnear      start of ray segment
      //   tfar       end of ray segment
      //   masks      a 32 bit mask to identify active geometries.
      //   geoId      id of geometry mask (default std::numeric_limits<float>::infinity() if no: no masking)
      //   closestHit true for gets closest hit, false for furthest hit
      // Output:
      //   hit        information about hit
      // Returns true if and only if there was a hit
      inline bool intersectBeam(
        const Eigen::RowVector3f& origin,
        const Eigen::RowVector3f& direction,
        Hit& hit,
        float tnear = 0,
        float tfar = std::numeric_limits<float>::infinity(),
        int mask = 0xFFFFFFFF,
        int geoId = -1,
        bool closestHit = true,
        unsigned int samples = 4) const;

      // Given a ray find all hits in order
      //
      // Inputs:
      //   origin     3d origin point of ray
      //   direction  3d (not necessarily normalized) direction vector of ray
      //   tnear      start of ray segment
      //   tfar       end of ray segment
      //   masks      a 32 bit mask to identify active geometries.
      // Output:
      //   hit        information about hit
      //   num_rays   number of rays shot (at least one)
      // Returns true if and only if there was a hit
      inline bool intersectRay(
        const Eigen::RowVector3f& origin,
        const Eigen::RowVector3f& direction,
        std::vector<Hit > &hits,
        int& num_rays,
        float tnear = 0,
        float tfar = std::numeric_limits<float>::infinity(),
        int mask = 0xFFFFFFFF) const;

      // Given a ray find the first hit
      //
      // Inputs:
      //   a    3d first end point of segment
      //   ab   3d vector from a to other endpoint b
      // Output:
      //   hit  information about hit
      // Returns true if and only if there was a hit
      inline bool intersectSegment(
        const Eigen::RowVector3f& a,
        const Eigen::RowVector3f& ab,
        Hit &hit,
        int mask = 0xFFFFFFFF) const;

    private:

      struct Vertex   {float x,y,z,a;};
      struct Triangle {int v0, v1, v2;};

      RTCScene scene;
      unsigned geomID;
      Vertex* vertices;
      Triangle* triangles;
      bool initialized;

      inline void createRay(
        RTCRay& ray,
        const Eigen::RowVector3f& origin,
        const Eigen::RowVector3f& direction,
        float tnear,
        float tfar,
        int mask) const;
    };
  }
}

// Implementation
#include <igl/EPS.h>
// This unfortunately cannot be a static field of EmbreeIntersector because it
// would depend on the template and then we might end up with initializing
// embree twice. If only there was a way to ask embree if it's already
// initialized...
namespace igl
{
  namespace embree
  {
    // Keeps track of whether the **Global** Embree intersector has been
    // initialized. This should never been done at the global scope.
    static bool EmbreeIntersector_inited = false;
  }
}

inline void igl::embree::EmbreeIntersector::global_init()
{
  if(!EmbreeIntersector_inited)
  {
    rtcInit();
    if(rtcGetError() != RTC_NO_ERROR)
      std::cerr << "Embree: An error occurred while initializing embree core!" << std::endl;
#ifdef IGL_VERBOSE
    else
      std::cerr << "Embree: core initialized." << std::endl;
#endif
    EmbreeIntersector_inited = true;
  }
}

inline void igl::embree::EmbreeIntersector::global_deinit()
{
  EmbreeIntersector_inited = false;
  rtcExit();
}

inline igl::embree::EmbreeIntersector::EmbreeIntersector()
  :
  //scene(NULL),
  geomID(0),
  vertices(NULL),
  triangles(NULL),
  initialized(false)
{
}

inline igl::embree::EmbreeIntersector::EmbreeIntersector(
  const EmbreeIntersector &)
  :// To make -Weffc++ happy
  //scene(NULL),
  geomID(0),
  vertices(NULL),
  triangles(NULL),
  initialized(false)
{
  assert(false && "Embree: Copying EmbreeIntersector is not allowed");
}

inline igl::embree::EmbreeIntersector & igl::embree::EmbreeIntersector::operator=(
  const EmbreeIntersector &)
{
  assert(false && "Embree: Assigning an EmbreeIntersector is not allowed");
  return *this;
}


inline void igl::embree::EmbreeIntersector::init(
  const PointMatrixType& V,
  const FaceMatrixType& F,
  bool isStatic)
{
  std::vector<const PointMatrixType*> Vtemp;
  std::vector<const FaceMatrixType*> Ftemp;
  std::vector<int> masks;
  Vtemp.push_back(&V);
  Ftemp.push_back(&F);
  masks.push_back(0xFFFFFFFF);
  init(Vtemp,Ftemp,masks,isStatic);
}

inline void igl::embree::EmbreeIntersector::init(
  const std::vector<const PointMatrixType*>& V,
  const std::vector<const FaceMatrixType*>& F,
  const std::vector<int>& masks,
  bool isStatic)
{

  if(initialized)
    deinit();

  using namespace std;
  global_init();

  if(V.size() == 0 || F.size() == 0)
  {
    std::cerr << "Embree: No geometry specified!";
    return;
  }

  // create a scene
  RTCSceneFlags flags = RTC_SCENE_ROBUST | RTC_SCENE_HIGH_QUALITY;
  if(isStatic)
    flags = flags | RTC_SCENE_STATIC;
  scene = rtcNewScene(flags,RTC_INTERSECT1);

  for(int g=0;g<(int)V.size();g++)
  {
    // create triangle mesh geometry in that scene
    geomID = rtcNewTriangleMesh(scene,RTC_GEOMETRY_STATIC,F[g]->rows(),V[g]->rows(),1);

    // fill vertex buffer
    vertices = (Vertex*)rtcMapBuffer(scene,geomID,RTC_VERTEX_BUFFER);
    for(int i=0;i<(int)V[g]->rows();i++)
    {
      vertices[i].x = (float)V[g]->coeff(i,0);
      vertices[i].y = (float)V[g]->coeff(i,1);
      vertices[i].z = (float)V[g]->coeff(i,2);
    }
    rtcUnmapBuffer(scene,geomID,RTC_VERTEX_BUFFER);

    // fill triangle buffer
    triangles = (Triangle*) rtcMapBuffer(scene,geomID,RTC_INDEX_BUFFER);
    for(int i=0;i<(int)F[g]->rows();i++)
    {
      triangles[i].v0 = (int)F[g]->coeff(i,0);
      triangles[i].v1 = (int)F[g]->coeff(i,1);
      triangles[i].v2 = (int)F[g]->coeff(i,2);
    }
    rtcUnmapBuffer(scene,geomID,RTC_INDEX_BUFFER);

    rtcSetMask(scene,geomID,masks[g]);
  }

  rtcCommit(scene);

  if(rtcGetError() != RTC_NO_ERROR)
      std::cerr << "Embree: An error occurred while initializing the provided geometry!" << endl;
#ifdef IGL_VERBOSE
  else
    std::cerr << "Embree: geometry added." << endl;
#endif

  initialized = true;
}

igl::embree::EmbreeIntersector
::~EmbreeIntersector()
{
  if(initialized)
    deinit();
}

void igl::embree::EmbreeIntersector::deinit()
{
  if(EmbreeIntersector_inited && scene)
  {
    rtcDeleteScene(scene);

    if(rtcGetError() != RTC_NO_ERROR)
    {
        std::cerr << "Embree: An error occurred while resetting!" << std::endl;
    }
#ifdef IGL_VERBOSE
    else
    {
      std::cerr << "Embree: geometry removed." << std::endl;
    }
#endif
  }
}

inline bool igl::embree::EmbreeIntersector::intersectRay(
  const Eigen::RowVector3f& origin,
  const Eigen::RowVector3f& direction,
  Hit& hit,
  float tnear,
  float tfar,
  int mask) const
{
  RTCRay ray;
  createRay(ray, origin,direction,tnear,tfar,mask);

  // shot ray
  rtcIntersect(scene,ray);
#ifdef IGL_VERBOSE
  if(rtcGetError() != RTC_NO_ERROR)
      std::cerr << "Embree: An error occurred while resetting!" << std::endl;
#endif

  if((unsigned)ray.geomID != RTC_INVALID_GEOMETRY_ID)
  {
    hit.id = ray.primID;
    hit.gid = ray.geomID;
    hit.u = ray.u;
    hit.v = ray.v;
    hit.t = ray.tfar;
    return true;
  }

  return false;
}

inline bool igl::embree::EmbreeIntersector::intersectBeam(
      const Eigen::RowVector3f& origin,
      const Eigen::RowVector3f& direction,
      Hit& hit,
      float tnear,
      float tfar,
      int mask,
      int geoId,
      bool closestHit,
	  unsigned int samples) const
{
  bool hasHit = false;
  Hit bestHit;

  if(closestHit)
    bestHit.t = std::numeric_limits<float>::max();
  else
    bestHit.t = 0;

  if((intersectRay(origin,direction,hit,tnear,tfar,mask) && (hit.gid == geoId || geoId == -1)))
  {
    bestHit = hit;
    hasHit = true;
  }

  // sample points around actual ray (conservative hitcheck)
  const float eps= 1e-5;

  Eigen::RowVector3f up(0,1,0);
  Eigen::RowVector3f offset = direction.cross(up).normalized();

  Eigen::Matrix3f rot = Eigen::AngleAxis<float>(2*3.14159265358979/samples,direction).toRotationMatrix();

  for(int r=0;r<(int)samples;r++)
  {
    if(intersectRay(origin+offset*eps,direction,hit,tnear,tfar,mask) && 
        ((closestHit && (hit.t < bestHit.t)) || 
           (!closestHit && (hit.t > bestHit.t)))  &&
        (hit.gid == geoId || geoId == -1))
    {
      bestHit = hit;
      hasHit = true;
    }
    offset = rot*offset.transpose();
  }

  hit = bestHit;
  return hasHit;
}

inline bool
igl::embree::EmbreeIntersector
::intersectRay(
  const Eigen::RowVector3f& origin,
  const Eigen::RowVector3f& direction,
  std::vector<Hit > &hits,
  int& num_rays,
  float tnear,
  float tfar,
  int mask) const
{
  using namespace std;
  num_rays = 0;
  hits.clear();
  int last_id0 = -1;
  double self_hits = 0;
  // This epsilon is directly correleated to the number of missed hits, smaller
  // means more accurate and slower
  //const double eps = DOUBLE_EPS;
  const double eps = FLOAT_EPS;
  double min_t = tnear;
  bool large_hits_warned = false;
  RTCRay ray;
  createRay(ray,origin,direction,tnear,tfar,mask);

  while(true)
  {
    ray.tnear = min_t;
    ray.tfar = tfar;
    ray.geomID = RTC_INVALID_GEOMETRY_ID;
    ray.primID = RTC_INVALID_GEOMETRY_ID;
    ray.instID = RTC_INVALID_GEOMETRY_ID;
    num_rays++;
    rtcIntersect(scene,ray);
    if((unsigned)ray.geomID != RTC_INVALID_GEOMETRY_ID)
    {
      // Hit self again, progressively advance
      if(ray.primID == last_id0 || ray.tfar <= min_t)
      {
        // push min_t a bit more
        //double t_push = pow(2.0,self_hits-4)*(hit.t<eps?eps:hit.t);
        double t_push = pow(2.0,self_hits)*eps;
        #ifdef IGL_VERBOSE
        std::cerr<<"  t_push: "<<t_push<<endl;
        #endif
        //o = o+t_push*d;
        min_t += t_push;
        self_hits++;
      }
      else
      {
        Hit hit;
        hit.id = ray.primID;
        hit.gid = ray.geomID;
        hit.u = ray.u;
        hit.v = ray.v;
        hit.t = ray.tfar;
        hits.push_back(hit);
#ifdef IGL_VERBOSE
        std::cerr<<"  t: "<<hit.t<<endl;
#endif
        // Instead of moving origin, just change min_t. That way calculations
        // all use exactly same origin values
        min_t = ray.tfar;

        // reset t_scale
        self_hits = 0;
      }
      last_id0 = ray.primID;
    }
    else
      break; // no more hits

    if(hits.size()>1000 && !large_hits_warned)
    {
      std::cout<<"Warning: Large number of hits..."<<endl;
      std::cout<<"[ ";
      for(vector<Hit>::iterator hit = hits.begin(); hit != hits.end();hit++)
      {
        std::cout<<(hit->id+1)<<" ";
      }

      std::cout.precision(std::numeric_limits< double >::digits10);
      std::cout<<"[ ";

      for(vector<Hit>::iterator hit = hits.begin(); hit != hits.end(); hit++)
      {
        std::cout<<(hit->t)<<endl;;
      }

      std::cout<<"]"<<endl;
      large_hits_warned = true;

      return hits.empty();
    }
  }

  return hits.empty();
}

inline bool
igl::embree::EmbreeIntersector
::intersectSegment(const Eigen::RowVector3f& a, const Eigen::RowVector3f& ab, Hit &hit, int mask) const
{
  RTCRay ray;
  createRay(ray,a,ab,0,1.0,mask);

  rtcIntersect(scene,ray);

  if((unsigned)ray.geomID != RTC_INVALID_GEOMETRY_ID)
  {
    hit.id = ray.primID;
    hit.gid = ray.geomID;
    hit.u = ray.u;
    hit.v = ray.v;
    hit.t = ray.tfar;
    return true;
  }

  return false;
}

inline void
igl::embree::EmbreeIntersector
::createRay(RTCRay& ray, const Eigen::RowVector3f& origin, const Eigen::RowVector3f& direction, float tnear, float tfar, int mask) const
{
  ray.org[0] = origin[0];
  ray.org[1] = origin[1];
  ray.org[2] = origin[2];
  ray.dir[0] = direction[0];
  ray.dir[1] = direction[1];
  ray.dir[2] = direction[2];
  ray.tnear = tnear;
  ray.tfar = tfar;
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.instID = RTC_INVALID_GEOMETRY_ID;
  ray.mask = mask;
  ray.time = 0.0f;
}

#endif //EMBREE_INTERSECTOR_H
