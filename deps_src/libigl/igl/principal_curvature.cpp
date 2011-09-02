// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "principal_curvature.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <list>
#include <cmath>
#include <limits>

#include <Eigen/SparseCholesky>

// Lib IGL includes
#include <igl/adjacency_list.h>
#include <igl/per_face_normals.h>
#include <igl/per_vertex_normals.h>
#include <igl/avg_edge_length.h>
#include <igl/vertex_triangle_adjacency.h>

typedef enum
{
  SPHERE_SEARCH,
  K_RING_SEARCH
} searchType;

typedef enum
{
  AVERAGE,
  PROJ_PLANE
} normalType;

class CurvatureCalculator
{
public:
  /* Row number i represents the i-th vertex, whose columns are:
   curv[i][0] : K2
   curv[i][1] : K1
   curvDir[i][0] : PD1
   curvDir[i][1] : PD2
   */
  std::vector< std::vector<double> > curv;
  std::vector< std::vector<Eigen::Vector3d> > curvDir;
  bool curvatureComputed;
  class Quadric
  {
  public:

    IGL_INLINE Quadric ()
    {
      a() = b() = c() = d() = e() = 1.0;
    }

    IGL_INLINE Quadric(double av, double bv, double cv, double dv, double ev)
    {
      a() = av;
      b() = bv;
      c() = cv;
      d() = dv;
      e() = ev;
    }

    IGL_INLINE double& a() { return data[0];}
    IGL_INLINE double& b() { return data[1];}
    IGL_INLINE double& c() { return data[2];}
    IGL_INLINE double& d() { return data[3];}
    IGL_INLINE double& e() { return data[4];}

    double data[5];

    IGL_INLINE double evaluate(double u, double v)
    {
      return a()*u*u + b()*u*v + c()*v*v + d()*u + e()*v;
    }

    IGL_INLINE double du(double u, double v)
    {
      return 2.0*a()*u + b()*v + d();
    }

    IGL_INLINE double dv(double u, double v)
    {
      return 2.0*c()*v + b()*u + e();
    }

    IGL_INLINE double duv(double u, double v)
    {
      return b();
    }

    IGL_INLINE double duu(double u, double v)
    {
      return 2.0*a();
    }

    IGL_INLINE double dvv(double u, double v)
    {
      return 2.0*c();
    }


    IGL_INLINE static Quadric fit(const std::vector<Eigen::Vector3d> &VV)
    {
      assert(VV.size() >= 5);
      if (VV.size() < 5)
      {
        std::cerr << "ASSERT FAILED! fit function requires at least 5 points: Only " << VV.size() << " were given." << std::endl;
        exit(0);
      }

      Eigen::MatrixXd A(VV.size(),5);
      Eigen::MatrixXd b(VV.size(),1);
      Eigen::MatrixXd sol(5,1);

      for(unsigned int c=0; c < VV.size(); ++c)
      {
        double u = VV[c][0];
        double v = VV[c][1];
        double n = VV[c][2];

        A(c,0) = u*u;
        A(c,1) = u*v;
        A(c,2) = v*v;
        A(c,3) = u;
        A(c,4) = v;

        b(c) = n;
      }

      sol=A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b);

      return Quadric(sol(0),sol(1),sol(2),sol(3),sol(4));
    }
  };

public:

  Eigen::MatrixXd vertices;
  // Face list of current mesh    (#F x 3) or (#F x 4)
  // The i-th row contains the indices of the vertices that forms the i-th face in ccw order
  Eigen::MatrixXi faces;

  std::vector<std::vector<int> > vertex_to_vertices;
  std::vector<std::vector<int> > vertex_to_faces;
  std::vector<std::vector<int> > vertex_to_faces_index;
  Eigen::MatrixXd face_normals;
  Eigen::MatrixXd vertex_normals;

  /* Size of the neighborhood */
  double sphereRadius;
  int kRing;

  bool localMode; /* Use local mode */
  bool projectionPlaneCheck; /* Check collected vertices on tangent plane */
  bool montecarlo;
  unsigned int montecarloN;

  searchType st; /* Use either a sphere search or a k-ring search */
  normalType nt;

  double lastRadius;
  double scaledRadius;
  std::string lastMeshName;

  /* Benchmark related variables */
  bool expStep; /* True if we want the radius to increase exponentially */
  int step;  /* If expStep==false, by how much rhe radius increases on every step */
  int maxSize; /* The maximum limit of the radius in the benchmark */

  IGL_INLINE CurvatureCalculator();
  IGL_INLINE void init(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F);

  IGL_INLINE void finalEigenStuff(int, const std::vector<Eigen::Vector3d>&, Quadric&);
  IGL_INLINE void fitQuadric(const Eigen::Vector3d&, const std::vector<Eigen::Vector3d>& ref, const std::vector<int>& , Quadric *);
  IGL_INLINE void applyProjOnPlane(const Eigen::Vector3d&, const std::vector<int>&, std::vector<int>&);
  IGL_INLINE void getSphere(const int, const double, std::vector<int>&, int min);
  IGL_INLINE void getKRing(const int, const double,std::vector<int>&);
  IGL_INLINE Eigen::Vector3d project(const Eigen::Vector3d&, const Eigen::Vector3d&, const Eigen::Vector3d&);
  IGL_INLINE void computeReferenceFrame(int, const Eigen::Vector3d&, std::vector<Eigen::Vector3d>&);
  IGL_INLINE void getAverageNormal(int, const std::vector<int>&, Eigen::Vector3d&);
  IGL_INLINE void getProjPlane(int, const std::vector<int>&, Eigen::Vector3d&);
  IGL_INLINE void applyMontecarlo(const std::vector<int>&,std::vector<int>*);
  IGL_INLINE void computeCurvature();
  IGL_INLINE void printCurvature(const std::string& outpath);
  IGL_INLINE double getAverageEdge();

  IGL_INLINE static int rotateForward (double *v0, double *v1, double *v2)
  {
    double t;

    if (std::abs(*v2) >= std::abs(*v1) && std::abs(*v2) >= std::abs(*v0))
      return 0;

    t = *v0;
    *v0 = *v2;
    *v2 = *v1;
    *v1 = t;

    return 1 + rotateForward (v0, v1, v2);
  }

  IGL_INLINE static void rotateBackward (int nr, double *v0, double *v1, double *v2)
  {
    double t;

    if (nr == 0)
      return;

    t = *v2;
    *v2 = *v0;
    *v0 = *v1;
    *v1 = t;

    rotateBackward (nr - 1, v0, v1, v2);
  }

  IGL_INLINE static Eigen::Vector3d chooseMax (Eigen::Vector3d n, Eigen::Vector3d abc, double ab)
  {
    int max_i;
    double max_sp;
    Eigen::Vector3d nt[8];

    n.normalize ();
    abc.normalize ();

    max_sp = - std::numeric_limits<double>::max();

    for (int i = 0; i < 4; ++i)
    {
      nt[i] = n;
      if (ab > 0)
      {
        switch (i)
        {
          case 0:
            break;

          case 1:
            nt[i][2] = -n[2];
            break;

          case 2:
            nt[i][0] = -n[0];
            nt[i][1] = -n[1];
            break;

          case 3:
            nt[i][0] = -n[0];
            nt[i][1] = -n[1];
            nt[i][2] = -n[2];
            break;
        }
      }
      else
      {
        switch (i)
        {
          case 0:
            nt[i][0] = -n[0];
            break;

          case 1:
            nt[i][1] = -n[1];
            break;

          case 2:
            nt[i][0] = -n[0];
            nt[i][2] = -n[2];
            break;

          case 3:
            nt[i][1] = -n[1];
            nt[i][2] = -n[2];
            break;
        }
      }

      if (nt[i].dot(abc) > max_sp)
      {
        max_sp = nt[i].dot(abc);
        max_i = i;
      }
    }
    return nt[max_i];
  }

};

class comparer
{
public:
  IGL_INLINE bool operator() (const std::pair<int, double>& lhs, const std::pair<int, double>&rhs) const
  {
    return lhs.second>rhs.second;
  }
};

IGL_INLINE CurvatureCalculator::CurvatureCalculator()
{
  this->localMode=true;
  this->projectionPlaneCheck=true;
  this->sphereRadius=5;
  this->st=SPHERE_SEARCH;
  this->nt=AVERAGE;
  this->montecarlo=false;
  this->montecarloN=0;
  this->kRing=3;
  this->curvatureComputed=false;
  this->expStep=true;
}

IGL_INLINE void CurvatureCalculator::init(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F)
{
  // Normalize vertices
  vertices = V;

//  vertices = vertices.array() - vertices.minCoeff();
//  vertices = vertices.array() / vertices.maxCoeff();
//  vertices = vertices.array() * (1.0/igl::avg_edge_length(V,F));

  faces = F;
  igl::adjacency_list(F, vertex_to_vertices);
  igl::vertex_triangle_adjacency(V, F, vertex_to_faces, vertex_to_faces_index);
  igl::per_face_normals(V, F, face_normals);
  igl::per_vertex_normals(V, F, face_normals, vertex_normals);
}

IGL_INLINE void CurvatureCalculator::fitQuadric(const Eigen::Vector3d& v, const std::vector<Eigen::Vector3d>& ref, const std::vector<int>& vv, Quadric *q)
{
  std::vector<Eigen::Vector3d> points;
  points.reserve (vv.size());

  for (unsigned int i = 0; i < vv.size(); ++i) {

    Eigen::Vector3d  cp = vertices.row(vv[i]);

    // vtang non e` il v tangente!!!
    Eigen::Vector3d  vTang = cp - v;

    double x = vTang.dot(ref[0]);
    double y = vTang.dot(ref[1]);
    double z = vTang.dot(ref[2]);
    points.push_back(Eigen::Vector3d (x,y,z));
  }
  if (points.size() < 5)
  {
    std::cerr << "ASSERT FAILED! fit function requires at least 5 points: Only " << points.size() << " were given." << std::endl;
    *q = Quadric(0,0,0,0,0);
  }
  else
  {
    *q = Quadric::fit (points);
  }
}

IGL_INLINE void CurvatureCalculator::finalEigenStuff(int i, const std::vector<Eigen::Vector3d>& ref, Quadric& q)
{

  const double a = q.a();
  const double b = q.b();
  const double c = q.c();
  const double d = q.d();
  const double e = q.e();

//  if (fabs(a) < 10e-8 || fabs(b) < 10e-8)
//  {
//    std::cout << "Degenerate quadric: " << i << std::endl;
//  }

  double E = 1.0 + d*d;
  double F = d*e;
  double G = 1.0 + e*e;

  Eigen::Vector3d n = Eigen::Vector3d(-d,-e,1.0).normalized();

  double L = 2.0 * a * n[2];
  double M = b * n[2];
  double N = 2 * c * n[2];


  // ----------------- Eigen stuff
  Eigen::Matrix2d m;
  m << L*G - M*F, M*E-L*F, M*E-L*F, N*E-M*F;
  m = m / (E*G-F*F);
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> eig(m);

  Eigen::Vector2d c_val = eig.eigenvalues();
  Eigen::Matrix2d c_vec = eig.eigenvectors();

  // std::cerr << "c_val:" << c_val << std::endl;
  // std::cerr << "c_vec:" << c_vec << std::endl;

  // std::cerr << "c_vec:" << c_vec(0) << " "  << c_vec(1) << std::endl;

  c_val = -c_val;

  Eigen::Vector3d v1, v2;
  v1[0] = c_vec(0);
  v1[1] = c_vec(1);
  v1[2] = 0; //d * v1[0] + e * v1[1];

  v2[0] = c_vec(2);
  v2[1] = c_vec(3);
  v2[2] = 0; //d * v2[0] + e * v2[1];


  // v1 = v1.normalized();
  // v2 = v2.normalized();

  Eigen::Vector3d v1global = ref[0] * v1[0] + ref[1] * v1[1] + ref[2] * v1[2];
  Eigen::Vector3d v2global = ref[0] * v2[0] + ref[1] * v2[1] + ref[2] * v2[2];

  v1global.normalize();
  v2global.normalize();

  v1global *= c_val(0);
  v2global *= c_val(1);

  if (c_val[0] > c_val[1])
  {
    curv[i]=std::vector<double>(2);
    curv[i][0]=c_val(1);
    curv[i][1]=c_val(0);
    curvDir[i]=std::vector<Eigen::Vector3d>(2);
    curvDir[i][0]=v2global;
    curvDir[i][1]=v1global;
  }
  else
  {
    curv[i]=std::vector<double>(2);
    curv[i][0]=c_val(0);
    curv[i][1]=c_val(1);
    curvDir[i]=std::vector<Eigen::Vector3d>(2);
    curvDir[i][0]=v1global;
    curvDir[i][1]=v2global;
  }
  // ---- end Eigen stuff
}

IGL_INLINE void CurvatureCalculator::getKRing(const int start, const double r, std::vector<int>&vv)
{
  int bufsize=vertices.rows();
  vv.reserve(bufsize);
  std::list<std::pair<int,int> > queue;
  bool* visited = (bool*)calloc(bufsize,sizeof(bool));
  queue.push_back(std::pair<int,int>(start,0));
  visited[start]=true;
  while (!queue.empty())
  {
    int toVisit=queue.front().first;
    int distance=queue.front().second;
    queue.pop_front();
    vv.push_back(toVisit);
    if (distance<(int)r)
    {
      for (unsigned int i=0; i<vertex_to_vertices[toVisit].size(); ++i)
      {
        int neighbor=vertex_to_vertices[toVisit][i];
        if (!visited[neighbor])
        {
          queue.push_back(std::pair<int,int> (neighbor,distance+1));
          visited[neighbor]=true;
        }
      }
    }
  }
  free(visited);
  return;
}


IGL_INLINE void CurvatureCalculator::getSphere(const int start, const double r, std::vector<int> &vv, int min)
{
  int bufsize=vertices.rows();
  vv.reserve(bufsize);
  std::list<int>* queue= new std::list<int>();
  bool* visited = (bool*)calloc(bufsize,sizeof(bool));
  queue->push_back(start);
  visited[start]=true;
  Eigen::Vector3d me=vertices.row(start);
  std::priority_queue<std::pair<int, double>, std::vector<std::pair<int, double> >, comparer >* extra_candidates= new  std::priority_queue<std::pair<int, double>, std::vector<std::pair<int, double> >, comparer >();
  while (!queue->empty())
  {
    int toVisit=queue->front();
    queue->pop_front();
    vv.push_back(toVisit);
    for (unsigned int i=0; i<vertex_to_vertices[toVisit].size(); ++i)
    {
      int neighbor=vertex_to_vertices[toVisit][i];
      if (!visited[neighbor])
      {
        Eigen::Vector3d neigh=vertices.row(neighbor);
        double distance=(me-neigh).norm();
        if (distance<r)
          queue->push_back(neighbor);
        else if ((int)vv.size()<min)
          extra_candidates->push(std::pair<int,double>(neighbor,distance));
        visited[neighbor]=true;
      }
    }
  }
  while (!extra_candidates->empty() && (int)vv.size()<min)
  {
    std::pair<int, double> cand=extra_candidates->top();
    extra_candidates->pop();
    vv.push_back(cand.first);
    for (unsigned int i=0; i<vertex_to_vertices[cand.first].size(); ++i)
    {
      int neighbor=vertex_to_vertices[cand.first][i];
      if (!visited[neighbor])
      {
        Eigen::Vector3d neigh=vertices.row(neighbor);
        double distance=(me-neigh).norm();
        extra_candidates->push(std::pair<int,double>(neighbor,distance));
        visited[neighbor]=true;
      }
    }
  }
  free(extra_candidates);
  free(queue);
  free(visited);
}

IGL_INLINE Eigen::Vector3d CurvatureCalculator::project(const Eigen::Vector3d& v, const Eigen::Vector3d& vp, const Eigen::Vector3d& ppn)
{
  return (vp - (ppn * ((vp - v).dot(ppn))));
}

IGL_INLINE void CurvatureCalculator::computeReferenceFrame(int i, const Eigen::Vector3d& normal, std::vector<Eigen::Vector3d>& ref )
{

  Eigen::Vector3d longest_v=Eigen::Vector3d(vertices.row(vertex_to_vertices[i][0]));

  longest_v=(project(vertices.row(i),longest_v,normal)-Eigen::Vector3d(vertices.row(i))).normalized();

  /* L'ultimo asse si ottiene come prodotto vettoriale tra i due
   * calcolati */
  Eigen::Vector3d y_axis=(normal.cross(longest_v)).normalized();
  ref[0]=longest_v;
  ref[1]=y_axis;
  ref[2]=normal;
}

IGL_INLINE void CurvatureCalculator::getAverageNormal(int j, const std::vector<int>& vv, Eigen::Vector3d& normal)
{
  normal=(vertex_normals.row(j)).normalized();
  if (localMode)
    return;

  for (unsigned int i=0; i<vv.size(); ++i)
  {
    normal+=vertex_normals.row(vv[i]).normalized();
  }
  normal.normalize();
}

IGL_INLINE void CurvatureCalculator::getProjPlane(int j, const std::vector<int>& vv, Eigen::Vector3d& ppn)
{
  int nr;
  double a, b, c;
  double nx, ny, nz;
  double abcq;

  a = b = c = 0;

  if (localMode)
  {
    for (unsigned int i=0; i<vertex_to_faces.at(j).size(); ++i)
    {
      Eigen::Vector3d faceNormal=face_normals.row(vertex_to_faces.at(j).at(i));
      a += faceNormal[0];
      b += faceNormal[1];
      c += faceNormal[2];
    }
  }
  else
  {
    for (unsigned int i=0; i<vv.size(); ++i)
    {
      a+= vertex_normals.row(vv[i])[0];
      b+= vertex_normals.row(vv[i])[1];
      c+= vertex_normals.row(vv[i])[2];
    }
  }
  nr = rotateForward (&a, &b, &c);
  abcq = a*a + b*b + c*c;
  nx = sqrt (a*a / abcq);
  ny = sqrt (b*b / abcq);
  nz = sqrt (1 - nx*nx - ny*ny);
  rotateBackward (nr, &a, &b, &c);
  rotateBackward (nr, &nx, &ny, &nz);

  ppn = chooseMax (Eigen::Vector3d(nx, ny, nz), Eigen::Vector3d (a, b, c), a * b);
  ppn.normalize();
}


IGL_INLINE double CurvatureCalculator::getAverageEdge()
{
  double sum = 0;
  int count = 0;

  for (int i = 0; i<faces.rows(); ++i)
  {
    for (short unsigned j=0; j<3; ++j)
    {
      Eigen::Vector3d p1=vertices.row(faces.row(i)[j]);
      Eigen::Vector3d p2=vertices.row(faces.row(i)[(j+1)%3]);

      double l = (p1-p2).norm();

      sum+=l;
      ++count;
    }
  }

  return (sum/(double)count);
}


IGL_INLINE void CurvatureCalculator::applyProjOnPlane(const Eigen::Vector3d& ppn, const std::vector<int>& vin, std::vector<int> &vout)
{
  for (std::vector<int>::const_iterator vpi = vin.begin(); vpi != vin.end(); ++vpi)
    if (vertex_normals.row(*vpi) * ppn > 0.0)
      vout.push_back(*vpi);
}

IGL_INLINE void CurvatureCalculator::applyMontecarlo(const std::vector<int>& vin, std::vector<int> *vout)
{
  if (montecarloN >= vin.size ())
  {
    *vout = vin;
    return;
  }

  float p = ((float) montecarloN) / (float) vin.size();
  for (std::vector<int>::const_iterator vpi = vin.begin(); vpi != vin.end(); ++vpi)
  {
    float r;
    if ((r = ((float)rand () / RAND_MAX)) < p)
    {
      vout->push_back(*vpi);
    }
  }
}

IGL_INLINE void CurvatureCalculator::computeCurvature()
{
  //CHECK che esista la mesh
  const size_t vertices_count=vertices.rows();

  if (vertices_count ==0)
    return;

  curvDir=std::vector< std::vector<Eigen::Vector3d> >(vertices_count);
  curv=std::vector<std::vector<double> >(vertices_count);



  scaledRadius=getAverageEdge()*sphereRadius;

  std::vector<int> vv;
  std::vector<int> vvtmp;
  Eigen::Vector3d normal;

  //double time_spent;
  //double searchtime=0, ref_time=0, fit_time=0, final_time=0;

  for (size_t i=0; i<vertices_count; ++i)
  {
    vv.clear();
    vvtmp.clear();
    Eigen::Vector3d me=vertices.row(i);
    switch (st)
    {
      case SPHERE_SEARCH:
        getSphere(i,scaledRadius,vv,6);
        break;
      case K_RING_SEARCH:
        getKRing(i,kRing,vv);
        break;
      default:
        fprintf(stderr,"Error: search type not recognized");
        return;
    }

    if (vv.size()<6)
    {
      std::cerr << "Could not compute curvature of radius " << scaledRadius << std::endl;
      return;
    }


    if (projectionPlaneCheck)
    {
      vvtmp.reserve (vv.size ());
      applyProjOnPlane (vertex_normals.row(i), vv, vvtmp);
      if (vvtmp.size() >= 6 && vvtmp.size()<vv.size())
        vv = vvtmp;
    }


    switch (nt)
    {
      case AVERAGE:
        getAverageNormal(i,vv,normal);
        break;
      case PROJ_PLANE:
        getProjPlane(i,vv,normal);
        break;
      default:
        fprintf(stderr,"Error: normal type not recognized");
        return;
    }
    if (vv.size()<6)
    {
      std::cerr << "Could not compute curvature of radius " << scaledRadius << std::endl;
      return;
    }
    if (montecarlo)
    {
      if(montecarloN<6)
        break;
      vvtmp.reserve(vv.size());
      applyMontecarlo(vv,&vvtmp);
      vv=vvtmp;
    }

    if (vv.size()<6)
      return;
    std::vector<Eigen::Vector3d> ref(3);
    computeReferenceFrame(i,normal,ref);

    Quadric q;
    fitQuadric (me, ref, vv, &q);
    finalEigenStuff(i,ref,q);
  }

  lastRadius=sphereRadius;
  curvatureComputed=true;
}

IGL_INLINE void CurvatureCalculator::printCurvature(const std::string& outpath)
{
  using namespace std;
  if (!curvatureComputed)
    return;

  std::ofstream of;
  of.open(outpath.c_str());

  if (!of)
  {
    fprintf(stderr, "Error: could not open output file %s\n", outpath.c_str());
    return;
  }

  int vertices_count=vertices.rows();
  of << vertices_count << endl;
  for (int i=0; i<vertices_count; ++i)
  {
    of << curv[i][0] << " " << curv[i][1] << " " << curvDir[i][0][0] << " " << curvDir[i][0][1] << " " << curvDir[i][0][2] << " " <<
    curvDir[i][1][0] << " " << curvDir[i][1][1] << " " << curvDir[i][1][2] << endl;
  }

  of.close();

}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedPD1,
  typename DerivedPD2,
  typename DerivedPV1,
  typename DerivedPV2>
IGL_INLINE void igl::principal_curvature(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedPD1>& PD1,
  Eigen::PlainObjectBase<DerivedPD2>& PD2,
  Eigen::PlainObjectBase<DerivedPV1>& PV1,
  Eigen::PlainObjectBase<DerivedPV2>& PV2,
  unsigned radius,
  bool useKring)
{
  if (radius < 2)
  {
    radius = 2;
    std::cout << "WARNING: igl::principal_curvature needs a radius >= 2, fixing it to 2." << std::endl;
  }

  // Preallocate memory
  PD1.resize(V.rows(),3);
  PD2.resize(V.rows(),3);

  // Preallocate memory
  PV1.resize(V.rows(),1);
  PV2.resize(V.rows(),1);

  // Precomputation
  CurvatureCalculator cc;
  cc.init(V.template cast<double>(),F.template cast<int>());
  cc.sphereRadius = radius;

  if (useKring)
  {
    cc.kRing = radius;
    cc.st = K_RING_SEARCH;
  }

  // Compute
  cc.computeCurvature();

  // Copy it back
  for (unsigned i=0; i<V.rows(); ++i)
  {
    PD1.row(i) << cc.curvDir[i][0][0], cc.curvDir[i][0][1], cc.curvDir[i][0][2];
    PD2.row(i) << cc.curvDir[i][1][0], cc.curvDir[i][1][1], cc.curvDir[i][1][2];
    PD1.row(i).normalize();
    PD2.row(i).normalize();

    if (std::isnan(PD1(i,0)) || std::isnan(PD1(i,1)) || std::isnan(PD1(i,2)) || std::isnan(PD2(i,0)) || std::isnan(PD2(i,1)) || std::isnan(PD2(i,2)))
    {
      PD1.row(i) << 0,0,0;
      PD2.row(i) << 0,0,0;
    }

    PV1(i) = cc.curv[i][0];
    PV2(i) = cc.curv[i][1];

    if (PD1.row(i) * PD2.row(i).transpose() > 10e-6)
    {
      std::cerr << "PRINCIPAL_CURVATURE: Something is wrong with vertex: " << i << std::endl;
      PD1.row(i) *= 0;
      PD2.row(i) *= 0;
    }
  }

}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::principal_curvature<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, unsigned int, bool);
template void igl::principal_curvature<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, unsigned int, bool);
template void igl::principal_curvature<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, unsigned int, bool);
#endif
