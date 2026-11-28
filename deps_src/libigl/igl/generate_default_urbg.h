#ifndef IGL_GENERATE_DEFAULT_URBG_H
#define IGL_GENERATE_DEFAULT_URBG_H
#include <random>

namespace igl {
  using DEFAULT_URBG = std::mt19937;
  inline DEFAULT_URBG generate_default_urbg() 
  {
    return DEFAULT_URBG(std::rand());
  }
}
#endif

