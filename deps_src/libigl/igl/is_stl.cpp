#include "is_stl.h"
#include <string>
IGL_INLINE bool igl::is_stl(FILE * stl_file, bool & is_ascii)
{

  //      solid?
  //   YES      NO
  //   /         if .stl, definitely binary
  //  /
  // perfect size?
  //      YES     NO
  //
  const auto perfect_size = [](FILE * stl_file)->bool
  {
    //stl_file = freopen(NULL,"rb",stl_file);
    // Read 80 header
    char header[80];
    if(fread(header,sizeof(char),80,stl_file)!=80)
    {
      return false;
    }
    // Read number of triangles
    unsigned int num_tri;
    if(fread(&num_tri,sizeof(unsigned int),1,stl_file)!=1)
    {
      return false;
    }
    fseek(stl_file,0,SEEK_END);
    int file_size = ftell(stl_file);
    fseek(stl_file,0,SEEK_SET);
    //stl_file = freopen(NULL,"r",stl_file);
    return (file_size == 80 + 4 + (4*12 + 2) * num_tri);
  };
  // Specifically 80 character header
  char header[80];
  char solid[80];
  is_ascii = true;
  bool f = true;
  if(fread(header,1,80,stl_file) != 80)
  {
    f = false;
    goto finish;
  }

  sscanf(header,"%s",solid);
  if(std::string("solid") == solid)
  {
    f = true;
    is_ascii = !perfect_size(stl_file);
  }else
  {
    is_ascii = false;
    f = perfect_size(stl_file);
  }
finish:
  rewind(stl_file);
  return f;
}

IGL_INLINE bool igl::is_stl(FILE * stl_file)
{
  bool is_ascii;
  return is_stl(stl_file,is_ascii);
}
