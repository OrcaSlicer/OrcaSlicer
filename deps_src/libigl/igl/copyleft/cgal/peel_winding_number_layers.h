#ifndef IGL_COPYLEFT_CGAL_PEEL_WINDING_NUMBER_LAYERS_H
#define IGL_COPYLEFT_CGAL_PEEL_WINDING_NUMBER_LAYERS_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl {
  namespace copyleft {
    namespace cgal {
      /// Peel Winding number layers from a mesh
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[out] W  #V by 1 list of winding numbers
      template<
          typename DerivedV,
          typename DerivedF,
          typename DerivedW >
      IGL_INLINE size_t peel_winding_number_layers(
              const Eigen::MatrixBase<DerivedV > & V,
              const Eigen::MatrixBase<DerivedF > & F,
              Eigen::PlainObjectBase<DerivedW>& W);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "peel_winding_number_layers.cpp"
#endif
#endif
