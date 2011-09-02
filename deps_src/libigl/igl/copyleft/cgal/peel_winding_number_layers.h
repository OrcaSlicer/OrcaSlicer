#ifndef IGL_COPYLEFT_CGAL_PEEL_WINDING_NUMBER_LAYERS_H
#define IGL_COPYLEFT_CGAL_PEEL_WINDING_NUMBER_LAYERS_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl {
  namespace copyleft {
    namespace cgal {
        template<
            typename DerivedV,
            typename DerivedF,
            typename DerivedW >
        IGL_INLINE size_t peel_winding_number_layers(
                const Eigen::PlainObjectBase<DerivedV > & V,
                const Eigen::PlainObjectBase<DerivedF > & F,
                Eigen::PlainObjectBase<DerivedW>& W);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "peel_winding_number_layers.cpp"
#endif
#endif
