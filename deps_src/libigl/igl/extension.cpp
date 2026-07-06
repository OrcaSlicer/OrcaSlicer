#include "extension.h"
#include "pathinfo.h"

IGL_INLINE std::string igl::extension( const std::string & path)
{
  std::string d,b,e,f;
  pathinfo(path,d,b,e,f);
  return e;
}
