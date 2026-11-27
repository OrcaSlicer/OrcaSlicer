#ifndef IGL_PLACEHOLDERS_H
#define IGL_PLACEHOLDERS_H

#include <Eigen/Core>

#if EIGEN_VERSION_AT_LEAST(3, 4, 90)
  #define IGL_PLACEHOLDERS_ALL Eigen::placeholders::all
#else
  #define IGL_PLACEHOLDERS_ALL Eigen::all
#endif

namespace igl {
  namespace placeholders {
    const auto all = IGL_PLACEHOLDERS_ALL;
  }
}

#endif
