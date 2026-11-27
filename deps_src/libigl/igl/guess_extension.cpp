#include "guess_extension.h"

#include <string.h>

#include "is_stl.h"

IGL_INLINE void igl::guess_extension(FILE * fp, std::string & guess)
{
  const auto is_off = [](FILE * fp)-> bool
  {
    char header[1000];
    const std::string OFF("OFF");
    const std::string NOFF("NOFF");
    const std::string COFF("COFF");
    bool f = (fscanf(fp,"%s\n",header)==1 && (
        std::string(header).compare(0, OFF.length(), OFF)==0 ||
        std::string(header).compare(0, COFF.length(), COFF)==0 ||
        std::string(header).compare(0,NOFF.length(),NOFF)==0));
    rewind(fp);
    return f;
  };
  const auto is_ply = [](FILE * fp) -> bool
  {
    char header[1000];
    const std::string PLY("ply");
    bool f = (fscanf(fp,"%s\n",header)==1 && (std::string(header).compare(0, PLY.length(), PLY)==0 ));
    rewind(fp);
    return f;
  };
  const auto is_wrl = [](FILE * wrl_file)->bool
  {
    bool still_comments = true;
    char line[1000];
    std::string needle("point [");
    std::string haystack;
    while(still_comments)
    {
      if(fgets(line,1000,wrl_file) == NULL)
      {
        rewind(wrl_file);
        return false;
      }
      haystack = std::string(line);
      still_comments = std::string::npos == haystack.find(needle);
    }
    rewind(wrl_file);
    return true;
  };
  const auto is_mesh = [](FILE * mesh_file )->bool
  {
    char line[2048];
    // eat comments at beginning of file
    bool still_comments= true;
    while(still_comments)
    {
      if(fgets(line,2048,mesh_file) == NULL)
      {
        rewind(mesh_file);
        return false;
      }
      still_comments = (line[0] == '#' || line[0] == '\n');
    }
    char str[2048];
    sscanf(line," %s",str);
    // check that first word is MeshVersionFormatted
    if(0!=strcmp(str,"MeshVersionFormatted"))
    {
      rewind(mesh_file);
      return false;
    }
    rewind(mesh_file);
    return true;
  };
  guess = "obj";
  if(is_mesh(fp))
  {
    guess = "mesh";
  }else if(is_off(fp))
  {
    guess = "off";
  }else if(is_ply(fp))
  {
    guess = "ply";
  }else if(igl::is_stl(fp))
  {
    guess = "stl";
  }else if(is_wrl(fp))
  {
    guess = "wrl";
  }
  // else obj
  rewind(fp);
}

IGL_INLINE std::string igl::guess_extension(FILE * fp)
{
  std::string guess;
  guess_extension(fp,guess);
  return guess;
}
