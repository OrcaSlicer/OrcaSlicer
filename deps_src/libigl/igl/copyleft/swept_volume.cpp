#include "swept_volume.h"
#include "../swept_volume_bounding_box.h"
#include "../swept_volume_signed_distance.h"
#include "../voxel_grid.h"
#include "marching_cubes.h"
#include <iostream>

IGL_INLINE void igl::copyleft::swept_volume(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const std::function<Eigen::Affine3d(const double t)> & transform,
  const size_t steps,
  const size_t grid_res,
  const size_t isolevel_grid,
  Eigen::MatrixXd & SV,
  Eigen::MatrixXi & SF)
{
  using namespace std;
  using namespace Eigen;
  using namespace igl;
  using namespace igl::copyleft;

  const auto & Vtransform = 
    [&V,&transform](const size_t vi,const double t)->RowVector3d
  {
    Vector3d Vvi = V.row(vi).transpose();
    return (transform(t)*Vvi).transpose();
  };
  AlignedBox3d Mbox;
  swept_volume_bounding_box(V.rows(),Vtransform,steps,Mbox);

  // Amount of padding: pad*h should be >= isolevel
  const int pad = isolevel_grid+1;
  // number of vertices on the largest side
  const int s = grid_res+2*pad;
  const double h = Mbox.diagonal().maxCoeff()/(double)(s-2.*pad-1.);
  const double isolevel = isolevel_grid*h;

  // create grid
  RowVector3i res;
  MatrixXd GV;
  voxel_grid(Mbox,s,pad,GV,res);

  // compute values
  VectorXd S;
  swept_volume_signed_distance(V,F,transform,steps,GV,res,h,isolevel,S);
  S.array()-=isolevel;
  marching_cubes(S,GV,res(0),res(1),res(2),SV,SF);
}
