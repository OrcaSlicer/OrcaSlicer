
#include "EmbreeIntersector.h"

// Implementation
#include "../EPS.h"

IGL_INLINE igl::embree::EmbreeIntersector::EmbreeIntersector()
  :
  //scene(NULL),
  geomID(0),
  vertices(NULL),
  triangles(NULL),
  initialized(false),
  device(igl::embree::EmbreeDevice::get_device())
{
}

IGL_INLINE igl::embree::EmbreeIntersector::EmbreeIntersector(
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

IGL_INLINE igl::embree::EmbreeIntersector & igl::embree::EmbreeIntersector::operator=(
  const EmbreeIntersector &)
{
  assert(false && "Embree: Assigning an EmbreeIntersector is not allowed");
  return *this;
}


IGL_INLINE void igl::embree::EmbreeIntersector::init(
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

IGL_INLINE void igl::embree::EmbreeIntersector::init(
  const std::vector<const PointMatrixType*>& V,
  const std::vector<const FaceMatrixType*>& F,
  const std::vector<int>& masks,
  bool isStatic)
{

  if(initialized)
    deinit();

  using namespace std;

  if(V.size() == 0 || F.size() == 0)
  {
    std::cerr << "Embree: No geometry specified!";
    return;
  }

  RTCBuildQuality buildQuality = isStatic ? RTC_BUILD_QUALITY_HIGH : RTC_BUILD_QUALITY_MEDIUM;

  // create a scene
  scene = rtcNewScene(device);
  rtcSetSceneFlags(scene, RTC_SCENE_FLAG_ROBUST);
  rtcSetSceneBuildQuality(scene, buildQuality);

  for(int g=0;g<(int)V.size();g++)
  {
    // create triangle mesh geometry in that scene
    RTCGeometry geom_0 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
    rtcSetGeometryBuildQuality(geom_0,buildQuality);
    rtcSetGeometryTimeStepCount(geom_0,1);
    geomID = rtcAttachGeometry(scene,geom_0);
    rtcReleaseGeometry(geom_0);

    // fill vertex buffer
    vertices = (Vertex*)rtcSetNewGeometryBuffer(geom_0,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,4*sizeof(float),V[g]->rows());
    for(int i=0;i<(int)V[g]->rows();i++)
    {
      vertices[i].x = (float)V[g]->coeff(i,0);
      vertices[i].y = (float)V[g]->coeff(i,1);
      vertices[i].z = (float)V[g]->coeff(i,2);
    }


    // fill triangle buffer
    triangles = (Triangle*) rtcSetNewGeometryBuffer(geom_0,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,3*sizeof(int),F[g]->rows());
    for(int i=0;i<(int)F[g]->rows();i++)
    {
      triangles[i].v0 = (int)F[g]->coeff(i,0);
      triangles[i].v1 = (int)F[g]->coeff(i,1);
      triangles[i].v2 = (int)F[g]->coeff(i,2);
    }


    rtcSetGeometryMask(geom_0,masks[g]);
    rtcCommitGeometry(geom_0);
  }

  rtcCommitScene(scene);

  if(rtcGetDeviceError (device) != RTC_ERROR_NONE)
      std::cerr << "Embree: An error occurred while initializing the provided geometry!" << endl;
#ifdef IGL_VERBOSE
  else
    std::cerr << "Embree: geometry added." << endl;
#endif

  initialized = true;
}

IGL_INLINE igl::embree::EmbreeIntersector
::~EmbreeIntersector()
{
  if(initialized)
    deinit();
  igl::embree::EmbreeDevice::release_device();
}

IGL_INLINE void igl::embree::EmbreeIntersector::deinit()
{
  if(device && scene)
  {
    rtcReleaseScene(scene);

    if(rtcGetDeviceError (device) != RTC_ERROR_NONE)
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

IGL_INLINE bool igl::embree::EmbreeIntersector::intersectRay(
  const Eigen::RowVector3f& origin,
  const Eigen::RowVector3f& direction,
  Hit<float> & hit,
  float tnear,
  float tfar,
  int mask) const
{
  RTCRayHit ray; // EMBREE_FIXME: use RTCRay for occlusion rays
  ray.ray.flags = 0;
  createRay(ray, origin,direction,tnear,tfar,mask);

  // shot ray
  {
    rtcIntersect1(scene,&ray);
    ray.hit.Ng_x = -ray.hit.Ng_x; // EMBREE_FIXME: only correct for triangles,quads, and subdivision surfaces
    ray.hit.Ng_y = -ray.hit.Ng_y;
    ray.hit.Ng_z = -ray.hit.Ng_z;
  }
#ifdef IGL_VERBOSE
  if(rtcGetDeviceError (device) != RTC_ERROR_NONE)
      std::cerr << "Embree: An error occurred while resetting!" << std::endl;
#endif

  if((unsigned)ray.hit.geomID != RTC_INVALID_GEOMETRY_ID)
  {
    hit.id = ray.hit.primID;
    hit.gid = ray.hit.geomID;
    hit.u = ray.hit.u;
    hit.v = ray.hit.v;
    hit.t = ray.ray.tfar;
    return true;
  }

  return false;
}

IGL_INLINE bool igl::embree::EmbreeIntersector::intersectBeam(
      const Eigen::RowVector3f& origin,
      const Eigen::RowVector3f& direction,
      Hit<float> & hit,
      float tnear,
      float tfar,
      int mask,
      int geoId,
      bool closestHit,
	  unsigned int samples) const
{
  bool hasHit = false;
  Hit<float> bestHit;

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
  if (direction.cross(up).norm() < eps) up = Eigen::RowVector3f(1,0,0);
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

IGL_INLINE bool
igl::embree::EmbreeIntersector
::intersectRay(
  const Eigen::RowVector3f& origin,
  const Eigen::RowVector3f& direction,
  std::vector<Hit<float> > &hits,
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
  RTCRayHit ray; // EMBREE_FIXME: use RTCRay for occlusion rays
  ray.ray.flags = 0;
  createRay(ray,origin,direction,tnear,tfar,mask);

  while(true)
  {
    ray.ray.tnear = min_t;
    ray.ray.tfar = tfar;
    ray.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    ray.hit.primID = RTC_INVALID_GEOMETRY_ID;
    ray.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    num_rays++;
    {
      rtcIntersect1(scene,&ray);
      ray.hit.Ng_x = -ray.hit.Ng_x; // EMBREE_FIXME: only correct for triangles,quads, and subdivision surfaces
      ray.hit.Ng_y = -ray.hit.Ng_y;
      ray.hit.Ng_z = -ray.hit.Ng_z;
    }
    if((unsigned)ray.hit.geomID != RTC_INVALID_GEOMETRY_ID)
    {
      // Hit self again, progressively advance
      if(ray.hit.primID == last_id0 || ray.ray.tfar <= min_t)
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
        Hit<float> hit;
        hit.id = ray.hit.primID;
        hit.gid = ray.hit.geomID;
        hit.u = ray.hit.u;
        hit.v = ray.hit.v;
        hit.t = ray.ray.tfar;
        hits.push_back(hit);
#ifdef IGL_VERBOSE
        std::cerr<<"  t: "<<hit.t<<endl;
#endif
        // Instead of moving origin, just change min_t. That way calculations
        // all use exactly same origin values
        min_t = ray.ray.tfar;

        // reset t_scale
        self_hits = 0;
      }
      last_id0 = ray.hit.primID;
    }
    else
      break; // no more hits

    if(hits.size()>1000 && !large_hits_warned)
    {
      std::cout<<"Warning: Large number of hits..."<<endl;
      std::cout<<"[ ";
      for(vector<Hit<float>>::iterator hit = hits.begin(); hit != hits.end();hit++)
      {
        std::cout<<(hit->id+1)<<" ";
      }

      std::cout.precision(std::numeric_limits< double >::digits10);
      std::cout<<"[ ";

      for(vector<Hit<float>>::iterator hit = hits.begin(); hit != hits.end(); hit++)
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

IGL_INLINE bool
igl::embree::EmbreeIntersector
::intersectSegment(const Eigen::RowVector3f& a, const Eigen::RowVector3f& ab, Hit<float> &hit, int mask) const
{
  RTCRayHit ray; // EMBREE_FIXME: use RTCRay for occlusion rays
  ray.ray.flags = 0;
  createRay(ray,a,ab,0,1.0,mask);

  {
    rtcIntersect1(scene,&ray);
    ray.hit.Ng_x = -ray.hit.Ng_x; // EMBREE_FIXME: only correct for triangles,quads, and subdivision surfaces
    ray.hit.Ng_y = -ray.hit.Ng_y;
    ray.hit.Ng_z = -ray.hit.Ng_z;
  }

  if((unsigned)ray.hit.geomID != RTC_INVALID_GEOMETRY_ID)
  {
    hit.id = ray.hit.primID;
    hit.gid = ray.hit.geomID;
    hit.u = ray.hit.u;
    hit.v = ray.hit.v;
    hit.t = ray.ray.tfar;
    return true;
  }

  return false;
}

IGL_INLINE void
igl::embree::EmbreeIntersector
::createRay(RTCRayHit& ray, const Eigen::RowVector3f& origin, const Eigen::RowVector3f& direction, float tnear, float tfar, int mask) const
{
  ray.ray.org_x = origin[0];
  ray.ray.org_y = origin[1];
  ray.ray.org_z = origin[2];
  ray.ray.dir_x = direction[0];
  ray.ray.dir_y = direction[1];
  ray.ray.dir_z = direction[2];
  ray.ray.tnear = tnear;
  ray.ray.tfar = tfar;
  ray.ray.id = RTC_INVALID_GEOMETRY_ID;
  ray.ray.mask = mask;
  ray.ray.time = 0.0f;

  ray.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  ray.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  ray.hit.primID = RTC_INVALID_GEOMETRY_ID;
}
