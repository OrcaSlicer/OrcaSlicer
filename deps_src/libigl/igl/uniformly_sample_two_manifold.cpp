// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "uniformly_sample_two_manifold.h"
#include "verbose.h"
#include "slice.h"
#include "colon.h"
#include "all_pairs_distances.h"
#include "mat_max.h"
#include "vertex_triangle_adjacency.h"
#include "get_seconds.h"
#include "cat.h"
//#include "MT19937.h"
#include "partition.h"

//////////////////////////////////////////////////////////////////////////////
// Helper functions
//////////////////////////////////////////////////////////////////////////////

IGL_INLINE void igl::uniformly_sample_two_manifold(
  const Eigen::MatrixXd & W,
  const Eigen::MatrixXi & F,
  const int k,
  const double push,
  Eigen::MatrixXd & WS)
{
  using namespace Eigen;
  using namespace std;

  // Euclidean distance between two points on a mesh given as barycentric
  // coordinates
  // Inputs:
  //   W  #W by dim positions of mesh in weight space
  //   F  #F by 3 indices of triangles
  //   face_A  face index where 1st point lives
  //   bary_A  barycentric coordinates of 1st point on face_A
  //   face_B  face index where 2nd point lives
  //   bary_B  barycentric coordinates of 2nd point on face_B
  // Returns distance in euclidean space
  const auto & bary_dist = [] (
    const Eigen::MatrixXd & W,
    const Eigen::MatrixXi & F,
    const int face_A,
    const Eigen::Vector3d & bary_A,
    const int face_B,
    const Eigen::Vector3d & bary_B) -> double
  {
    return
      ((bary_A(0)*W.row(F(face_A,0)) +
        bary_A(1)*W.row(F(face_A,1)) +
        bary_A(2)*W.row(F(face_A,2)))
        -
        (bary_B(0)*W.row(F(face_B,0)) +
        bary_B(1)*W.row(F(face_B,1)) +
        bary_B(2)*W.row(F(face_B,2)))).norm();
  };

  // Base case if F is a tet list, find all faces and pass as non-manifold
  // triangle mesh
  if(F.cols() == 4)
  {
    verbose("uniform_sample.h: sampling tet mesh\n");
    MatrixXi T0 = F.col(0);
    MatrixXi T1 = F.col(1);
    MatrixXi T2 = F.col(2);
    MatrixXi T3 = F.col(3);
    // Faces from tets
    MatrixXi TF =
      cat(1,
        cat(1,
          cat(2,T0, cat(2,T1,T2)),
          cat(2,T0, cat(2,T2,T3))),
        cat(1,
          cat(2,T0, cat(2,T3,T1)),
          cat(2,T1, cat(2,T3,T2)))
      );
    assert(TF.rows() == 4*F.rows());
    assert(TF.cols() == 3);
    uniformly_sample_two_manifold(W,TF,k,push,WS);
    return;
  }

  double start = get_seconds();

  VectorXi S;
  // First get sampling as best as possible on mesh
  uniformly_sample_two_manifold_at_vertices(W,k,push,S);
  verbose("Lap: %g\n",get_seconds()-start);
  slice(W,S,colon<int>(0,W.cols()-1),WS);
  //cout<<"WSmesh=["<<endl<<WS<<endl<<"];"<<endl;

//#ifdef EXTREME_VERBOSE
  //cout<<"S=["<<endl<<S<<endl<<"];"<<endl;
//#endif

  // Build map from vertices to list of incident faces
  vector<vector<int> > VF,VFi;
  vertex_triangle_adjacency(W,F,VF,VFi);

  // List of list of face indices, for each sample gives index to face it is on
  vector<vector<int> > sample_faces; sample_faces.resize(k);
  // List of list of barycentric coordinates, for each sample gives b-coords in
  // face its on
  vector<vector<Eigen::Vector3d> > sample_barys; sample_barys.resize(k);
  // List of current maxmins amongst samples
  vector<int> cur_maxmin; cur_maxmin.resize(k);
  // List of distance matrices, D(i)(s,j) reveals distance from i's sth sample
  // to jth seed if j<k or (j-k)th "pushed" corner
  vector<MatrixXd> D; D.resize(k);

  // Precompute an W.cols() by W.cols() identity matrix
  MatrixXd I(MatrixXd::Identity(W.cols(),W.cols()));

  // Describe each seed as a face index and barycentric coordinates
  for(int i = 0;i < k;i++)
  {
    // Unreferenced vertex?
    assert(VF[S(i)].size() > 0);
    sample_faces[i].push_back(VF[S(i)][0]);
    // We're right on a face vertex so barycentric coordinates are 0, but 1 at
    // that vertex
    Eigen::Vector3d bary(0,0,0);
    bary( VFi[S(i)][0] ) = 1;
    sample_barys[i].push_back(bary);
    // initialize this to current maxmin
    cur_maxmin[i] = 0;
  }

  // initialize radius
  double radius = 1.0;
  // minimum radius (bound on precision)
  //double min_radius = 1e-5;
  double min_radius = 1e-5;
  int max_num_rand_samples_per_triangle = 100;
  int max_sample_attempts_per_triangle = 1000;
  // Max number of outer iterations for a given radius
  int max_iters = 1000;

  // continue iterating until radius is smaller than some threshold
  while(radius > min_radius)
  {
    // initialize each seed
    for(int i = 0;i < k;i++)
    {
      // Keep track of cur_maxmin data
      int face_i = sample_faces[i][cur_maxmin[i]];
      Eigen::Vector3d bary(sample_barys[i][cur_maxmin[i]]);
      // Find index in face of closest mesh vertex (on this face)
      int index_in_face =
        (bary(0) > bary(1) ? (bary(0) > bary(2) ? 0 : 2)
                           : (bary(1) > bary(2) ? 1 : 2));
      // find closest mesh vertex
      int vertex_i = F(face_i,index_in_face);
      // incident triangles
      vector<int> incident_F = VF[vertex_i];
      // We're going to try to place num_rand_samples_per_triangle samples on
      // each sample *after* this location
      sample_barys[i].clear();
      sample_faces[i].clear();
      cur_maxmin[i] = 0;
      sample_barys[i].push_back(bary);
      sample_faces[i].push_back(face_i);
      // Current seed location in weight space
      VectorXd seed =
        bary(0)*W.row(F(face_i,0)) +
        bary(1)*W.row(F(face_i,1)) +
        bary(2)*W.row(F(face_i,2));
#ifdef EXTREME_VERBOSE
      verbose("i: %d\n",i);
      verbose("face_i: %d\n",face_i);
      //cout<<"bary: "<<bary<<endl;
      verbose("index_in_face: %d\n",index_in_face);
      verbose("vertex_i: %d\n",vertex_i);
      verbose("incident_F.size(): %d\n",incident_F.size());
      //cout<<"seed: "<<seed<<endl;
#endif
      // loop over indcident triangles
      for(int f=0;f<(int)incident_F.size();f++)
      {
#ifdef EXTREME_VERBOSE
        verbose("incident_F[%d]: %d\n",f,incident_F[f]);
#endif
        int face_f = incident_F[f];
        int num_samples_f = 0;
        for(int s=0;s<max_sample_attempts_per_triangle;s++)
        {
          // Randomly sample unit square
          double u,v;
//      double ru = fgenrand();
//      double rv = fgenrand();
          double ru = (double)rand() / RAND_MAX;
          double rv = (double)rand() / RAND_MAX;
          // Reflect to lower triangle if above
          if((ru+rv)>1)
          {
            u = 1-rv;
            v = 1-ru;
          }else
          {
            u = ru;
            v = rv;
          }
          Eigen::Vector3d sample_bary(u,v,1-u-v);
          double d = bary_dist(W,F,face_i,bary,face_f,sample_bary);
          // check that sample is close enough
          if(d<radius)
          {
            // add sample to list
            sample_faces[i].push_back(face_f);
            sample_barys[i].push_back(sample_bary);
            num_samples_f++;
          }
          // Keep track of which random samples came from which face
          if(num_samples_f >= max_num_rand_samples_per_triangle)
          {
#ifdef EXTREME_VERBOSE
            verbose("Reached maximum number of samples per face\n");
#endif
            break;
          }
          if(s==(max_sample_attempts_per_triangle-1))
          {
#ifdef EXTREME_VERBOSE
            verbose("Reached maximum sample attempts per triangle\n");
#endif
          }
        }
#ifdef EXTREME_VERBOSE
        verbose("sample_faces[%d].size(): %d\n",i,sample_faces[i].size());
        verbose("sample_barys[%d].size(): %d\n",i,sample_barys[i].size());
#endif
      }
    }

    // Precompute distances from each seed's random samples to each "pushed"
    // corner
    // Put -1 in entries corresponding distance of a seed's random samples to
    // self
    // Loop over seeds
    for(int i = 0;i < k;i++)
    {
      // resize distance matrix for new samples
      D[i].resize(sample_faces[i].size(),k+W.cols());
      // Loop over i's samples
      for(int s = 0;s<(int)sample_faces[i].size();s++)
      {
        int sample_face = sample_faces[i][s];
        Eigen::Vector3d sample_bary = sample_barys[i][s];
        // Loop over other seeds
        for(int j = 0;j < k;j++)
        {
          // distance from sample(i,s) to seed j
          double d;
          if(i==j)
          {
            // phony self distance: Ilya's idea of infinite
            d = 10;
          }else
          {
            int seed_j_face = sample_faces[j][cur_maxmin[j]];
            Eigen::Vector3d seed_j_bary(sample_barys[j][cur_maxmin[j]]);
            d = bary_dist(W,F,sample_face,sample_bary,seed_j_face,seed_j_bary);
          }
          D[i](s,j) = d;
        }
        // Loop over corners
        for(int j = 0;j < W.cols();j++)
        {
          // distance from sample(i,s) to corner j
          double d =
            ((sample_bary(0)*W.row(F(sample_face,0)) +
              sample_bary(1)*W.row(F(sample_face,1)) +
              sample_bary(2)*W.row(F(sample_face,2)))
              - I.row(j)).norm()/push;
          // append after distances to seeds
          D[i](s,k+j) = d;
        }
      }
    }

    int iters = 0;
    while(true)
    {
      bool has_changed = false;
      // try to move each seed
      for(int i = 0;i < k;i++)
      {
        // for each sample look at distance to closest seed/corner
        VectorXd minD = D[i].rowwise().minCoeff();
        assert(minD.size() == (int)sample_faces[i].size());
        // find random sample with maximum minimum distance to other seeds
        int old_cur_maxmin = cur_maxmin[i];
        double max_min = -2;
        for(int s = 0;s<(int)sample_faces[i].size();s++)
        {
          if(max_min < minD(s))
          {
            max_min = minD(s);
            // Set this as the new seed location
            cur_maxmin[i] = s;
          }
        }
#ifdef EXTREME_VERBOSE
        verbose("max_min: %g\n",max_min);
        verbose("cur_maxmin[%d]: %d->%d\n",i,old_cur_maxmin,cur_maxmin[i]);
#endif
        // did location change?
        has_changed |= (old_cur_maxmin!=cur_maxmin[i]);
        // update distances of random samples of other seeds
      }
      // if no seed moved, exit
      if(!has_changed)
      {
        break;
      }
      iters++;
      if(iters>=max_iters)
      {
        verbose("Hit max iters (%d) before converging\n",iters);
      }
    }
    // shrink radius
    //radius *= 0.9;
    //radius *= 0.99;
    radius *= 0.9;
  }
  // Collect weight space locations
  WS.resize(k,W.cols());
  for(int i = 0;i<k;i++)
  {
    int face_i = sample_faces[i][cur_maxmin[i]];
    Eigen::Vector3d bary(sample_barys[i][cur_maxmin[i]]);
    WS.row(i) =
        bary(0)*W.row(F(face_i,0)) +
        bary(1)*W.row(F(face_i,1)) +
        bary(2)*W.row(F(face_i,2));
  }
  verbose("Lap: %g\n",get_seconds()-start);
  //cout<<"WSafter=["<<endl<<WS<<endl<<"];"<<endl;
}

IGL_INLINE void igl::uniformly_sample_two_manifold_at_vertices(
  const Eigen::MatrixXd & OW,
  const int k,
  const double push,
  Eigen::VectorXi & S)
{
  using namespace Eigen;
  using namespace std;

  // Copy weights and faces
  const MatrixXd & W = OW;
  /*const MatrixXi & F = OF;*/

  // Initialize seeds
  VectorXi G;
  Matrix<double,Dynamic,1>  ignore;
  partition(W,k+W.cols(),G,S,ignore);
  // Remove corners, which better be at top
  S = S.segment(W.cols(),k).eval();

  MatrixXd WS;
  slice(W,S,colon<int>(0,W.cols()-1),WS);
  //cout<<"WSpartition=["<<endl<<WS<<endl<<"];"<<endl;

  // number of vertices
  int n = W.rows();
  // number of dimensions in weight space
  int m = W.cols();
  // Corners of weight space
  MatrixXd I = MatrixXd::Identity(m,m);
  // append corners to bottom of weights
  MatrixXd WI(n+m,m);
  WI << W,I;
  // Weights at seeds and corners
  MatrixXd WSC(k+m,m);
  for(int i = 0;i<k;i++)
  {
    WSC.row(i) = W.row(S(i));
  }
  for(int i = 0;i<m;i++)
  {
    WSC.row(i+k) = WI.row(n+i);
  }
  // initialize all pairs sqaured distances
  MatrixXd sqrD;
  all_pairs_distances(WI,WSC,true,sqrD);
  // bring in corners by push factor (squared because distances are squared)
  sqrD.block(0,k,sqrD.rows(),m) /= push*push;

  int max_iters = 30;
  int j = 0;
  for(;j<max_iters;j++)
  {
    bool has_changed = false;
    // loop over seeds
    for(int i =0;i<k;i++)
    {
      int old_si = S(i);
      // set distance to ilya's idea of infinity
      sqrD.col(i).setZero();
      sqrD.col(i).array() += 10;
      // find vertex farthers from all other seeds
      MatrixXd minsqrD = sqrD.rowwise().minCoeff();
      MatrixXd::Index si,PHONY;
      minsqrD.maxCoeff(&si,&PHONY);
      MatrixXd Wsi = W.row(si);
      MatrixXd sqrDi;
      all_pairs_distances(WI,Wsi,true,sqrDi);
      sqrD.col(i) = sqrDi;
      S(i) = si;
      has_changed |= si!=old_si;
    }
    if(j == max_iters)
    {
      verbose("uniform_sample.h: Warning: hit max iters\n");
    }
    if(!has_changed)
    {
      break;
    }
  }
}
