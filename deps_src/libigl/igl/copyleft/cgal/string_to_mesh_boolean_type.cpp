#include "string_to_mesh_boolean_type.h"
#include <algorithm>
#include <cassert>
#include <vector>

IGL_INLINE bool igl::copyleft::cgal::string_to_mesh_boolean_type( 
  const std::string & s,
  MeshBooleanType & type)
{
  using namespace std;
  string eff_s = s;
  transform(eff_s.begin(), eff_s.end(), eff_s.begin(), ::tolower);
  const auto & find_any = 
    [](const vector<string> & haystack, const string & needle)->bool
  {
    return find(haystack.begin(), haystack.end(), needle) != haystack.end();
  };
  if(find_any({"union","unite","u","∪"},eff_s))
  {
    type = MESH_BOOLEAN_TYPE_UNION;
  }else if(find_any({"intersect","intersection","i","∩"},eff_s))
  {
    type = MESH_BOOLEAN_TYPE_INTERSECT;
  }else if(
    find_any(
      {"minus","subtract","difference","relative complement","m","\\"},eff_s))
  {
    type = MESH_BOOLEAN_TYPE_MINUS;
  }else if(find_any({"xor","symmetric difference","x","∆"},eff_s))
  {
    type = MESH_BOOLEAN_TYPE_XOR;
  }else if(find_any({"resolve"},eff_s))
  {
    type = MESH_BOOLEAN_TYPE_RESOLVE;
  }else
  {
    return false;
  }
  return true;
}

IGL_INLINE igl::MeshBooleanType 
igl::copyleft::cgal::string_to_mesh_boolean_type( 
  const std::string & s)
{
  MeshBooleanType type;
#ifndef NDEBUG
  const bool ret = 
#endif
    string_to_mesh_boolean_type(s,type);
  assert(ret && "Unknown MeshBooleanType name");
  return type;
}
