#include "intersection_blocking_collapse_edge_callbacks.h"
#include "AABB.h"
#include "circulation.h"
#include "decimate_trivial_callbacks.h"
#include "collapse_edge_would_create_intersections.h"

// If debugging, it's a good idea to run this once in release mode to collect
// the edge id then adjust below.
//#define IGL_INTERSECTION_BLOCKING_COLLAPSE_EDGE_CALLBACKS_DEBUG
#ifdef IGL_INTERSECTION_BLOCKING_COLLAPSE_EDGE_CALLBACKS_DEBUG
#include "copyleft/cgal/is_self_intersecting.h"
#include "writePLY.h"
#endif

void igl::intersection_blocking_collapse_edge_callbacks(
  const igl::decimate_pre_collapse_callback  & orig_pre_collapse,
  const igl::decimate_post_collapse_callback & orig_post_collapse,
  const std::vector<igl::AABB<Eigen::MatrixXd,3> *> & leaves,
        igl::AABB<Eigen::MatrixXd,3> * & tree,
        igl::decimate_pre_collapse_callback  & pre_collapse,
        igl::decimate_post_collapse_callback & post_collapse
        )
{
  // Not clear padding would help much but could try it.
  const double pad = 0;
  const int leaves_size = (int)leaves.size();
  // Capture the original callbacks by value so that caller can destory their
  // copy.
  pre_collapse = 
    [orig_pre_collapse,leaves_size,&tree](
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXi & E,
      const Eigen::VectorXi & EMAP,
      const Eigen::MatrixXi & EF,
      const Eigen::MatrixXi & EI,
      const igl::min_heap< std::tuple<double,int,int> > & Q,
      const Eigen::VectorXi & EQ,
      const Eigen::MatrixXd & C,
      const int e)->bool
    {
      if(!orig_pre_collapse(V,F,E,EMAP,EF,EI,Q,EQ,C,e))
      {
        return false;
      }
      
      // Check if there would be (new) intersections
      return 
        !igl::collapse_edge_would_create_intersections(
          e,C.row(e).eval(),V,F,E,EMAP,EF,EI,*tree,leaves_size);
    };
  // leaves could also be captured by value
  post_collapse =
    [orig_post_collapse,leaves,pad,leaves_size,&tree](
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXi & E,
      const Eigen::VectorXi & EMAP,
      const Eigen::MatrixXi & EF,
      const Eigen::MatrixXi & EI,
      const igl::min_heap< std::tuple<double,int,int> > & Q,
      const Eigen::VectorXi & EQ,
      const Eigen::MatrixXd & C,
      const int e,
      const int e1,
      const int e2,
      const int f1,
      const int f2,
      const bool collapsed)
    {
      if(collapsed)
      {
        // detach leaves of deleted faces
        for(int f : {f1,f2})
        {
          // Skip faces that aren't in leaves (e.g., infinite faces)
          if(f >= leaves_size) { continue; }
          auto * sibling = leaves[f]->detach();
          sibling->refit_lineage();
          tree = sibling->root();
          delete leaves[f];
        }
        // If finding `Nf` becomes a bottleneck we could remember it via
        // `pre_collapse` the same way that
        // `qslim_optimal_collapse_edge_callbacks` remembers `v1` and `v2`
        const int m = F.rows();
        const auto survivors = 
          [&m,&e,&EF,&EI,&EMAP](const int f1, const int e1, int & d1)
        {
          int c;
          for(c=0;c<3;c++)
          {
            d1 = EMAP(f1+c*m);
            if((d1 != e) && (d1 != e1)) { break; }
          }
          assert(c<3);
        };
        int d1,d2;
        survivors(f1,e1,d1);
        survivors(f2,e2,d2);
        // Will circulating by continuing in the CCW direction of E(d1,:)
        // encircle the common edge? That is, is E(d1,1) the common vertex?
        const bool ccw = E(d1,1) == E(d2,0) || E(d1,1) == E(d2,1);
#ifndef NDEBUG
        // Common vertex.
        const int s = E(d1,ccw?1:0);
        assert(s == E(d2,0) || s == E(d2,1));
#endif
        std::vector<int> Nf;
        {
          std::vector<int> Nv;
          igl::circulation(d1,ccw,F,EMAP,EF,EI,Nv,Nf);
        }
        for(const int & f : Nf)
        {
          // Skip faces that aren't in leaves (e.g., infinite faces)
          if(f >= leaves_size) { continue; }
          Eigen::AlignedBox<double,3> box;
          box
            .extend(V.row(F(f,0)).transpose())
            .extend(V.row(F(f,1)).transpose())
            .extend(V.row(F(f,2)).transpose());
          // Always grab root (returns self if no update)
          tree = leaves[f]->update(box,pad)->root();
          assert(tree == tree->root());
        }
          assert(tree == tree->root());
      }
#ifdef IGL_INTERSECTION_BLOCKING_COLLAPSE_EDGE_CALLBACKS_DEBUG
#warning "🐌🐌🐌🐌🐌🐌🐌🐌🐌🐌🐌 Slow intersection checking..."
      constexpr bool stinker = false;
      //const bool stinker = e==2581;
      if(stinker && igl::copyleft::cgal::is_self_intersecting(V,F))
      {
        igl::writePLY("after.ply",V,F);
        printf("💩🛌@e=%d \n",e);
        exit(1);
      }
#endif
      // Finally. Run callback.
      return orig_post_collapse(
        V,F,E,EMAP,EF,EI,Q,EQ,C,e,e1,e2,f1,f2,collapsed);
    };
}

IGL_INLINE void igl::intersection_blocking_collapse_edge_callbacks(
  const igl::decimate_pre_collapse_callback  & orig_pre_collapse,
  const igl::decimate_post_collapse_callback & orig_post_collapse,
        igl::AABB<Eigen::MatrixXd,3> * & tree,
        igl::decimate_pre_collapse_callback  & pre_collapse,
        igl::decimate_post_collapse_callback & post_collapse)
{
  return intersection_blocking_collapse_edge_callbacks(
    orig_pre_collapse,
    orig_post_collapse,
    tree->gather_leaves(),
    tree,
    pre_collapse,
    post_collapse);
}

IGL_INLINE void igl::intersection_blocking_collapse_edge_callbacks(
  igl::AABB<Eigen::MatrixXd,3> * & tree,
  igl::decimate_pre_collapse_callback  & pre_collapse,
  igl::decimate_post_collapse_callback & post_collapse)
{
  igl::decimate_pre_collapse_callback  always_try;
  igl::decimate_post_collapse_callback never_care;
  igl::decimate_trivial_callbacks(always_try,never_care);
  intersection_blocking_collapse_edge_callbacks(
    always_try,
    never_care,
    tree,
    pre_collapse,
    post_collapse);
}
