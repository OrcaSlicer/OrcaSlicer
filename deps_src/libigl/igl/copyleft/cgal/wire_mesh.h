#ifndef IGL_COPYLEFT_CGAL_WIRE_MESH_H
#define IGL_COPYLEFT_CGAL_WIRE_MESH_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Construct a "wire" or "wireframe" or "strut" surface mesh, given a
      /// one-dimensional network of straight edges.
      ///
      /// @param[in] WV  #WV by 3 list of vertex positions
      /// @param[in] WE  #WE by 2 list of edge indices into WV
      /// @param[in] th  #WE diameter thicknesses of wire edges
      /// @param[in] poly_size  number of sides on each wire (e.g., 4 would produce wires by
      ///     connecting rectangular prisms).
      /// @param[in] solid  whether to resolve self-intersections to
      ///     create a "solid" output mesh (cf., [Zhou et al. 2016]
      /// @param[out] V  #V by 3 list of output vertices
      /// @param[out] F  #F by 3 list of output triangle indices into V
      /// @param[out] J  #F list of indices into [0,#WV+#WE) revealing "birth simplex" of
      ///     output faces J(j) < #WV means the face corresponds to the J(j)th
      ///     vertex in WV. J(j) >= #WV means the face corresponds to the
      ///     (J(j)-#WV)th edge in WE.
      template <
        typename DerivedWV,
        typename DerivedWE,
        typename Derivedth,
        typename DerivedV,
        typename DerivedF,
        typename DerivedJ>
      IGL_INLINE void wire_mesh(
        const Eigen::MatrixBase<DerivedWV> & WV,
        const Eigen::MatrixBase<DerivedWE> & WE,
        const Eigen::MatrixBase<Derivedth> & th,
        const int poly_size,
        const bool solid,
        Eigen::PlainObjectBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedJ> & J);
      /// \overload
      /// \brief uniform th
      template <
        typename DerivedWV,
        typename DerivedWE,
        typename DerivedV,
        typename DerivedF,
        typename DerivedJ>
      IGL_INLINE void wire_mesh(
        const Eigen::MatrixBase<DerivedWV> & WV,
        const Eigen::MatrixBase<DerivedWE> & WE,
        const double th,
        const int poly_size,
        const bool solid,
        Eigen::PlainObjectBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedJ> & J);
      /// \overload
      /// \brief Solid == true
      template <
        typename DerivedWV,
        typename DerivedWE,
        typename DerivedV,
        typename DerivedF,
        typename DerivedJ>
      IGL_INLINE void wire_mesh(
        const Eigen::MatrixBase<DerivedWV> & WV,
        const Eigen::MatrixBase<DerivedWE> & WE,
        const double th,
        const int poly_size,
        Eigen::PlainObjectBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedJ> & J);

    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "wire_mesh.cpp"
#endif
#endif
