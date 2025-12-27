// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "writeDAE.h"
#include "../STR.h"
#include <tinyxml2.h>
#include <map>
#include <list>

template <typename DerivedV, typename DerivedF>
IGL_INLINE bool igl::xml::writeDAE(
  const std::string & filename,
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F)
{
  using namespace std;
  using namespace Eigen;
  using namespace tinyxml2;

  XMLDocument* doc = new XMLDocument();

  const auto & ele = [&doc](
    const std::string tag,
    // Can't just use `{}` as the default argument because of a clang-bug
    // http://stackoverflow.com/questions/17264067/
    const std::map<std::string,std::string> attribs =
      std::map<std::string,std::string>(),
    const std::string text="",
    const std::list<XMLElement *> children =
       std::list<XMLElement *>()
    )->XMLElement *
  {
    XMLElement * element = doc->NewElement(tag.c_str());
    for(const auto & key_value :  attribs)
    {
      element->SetAttribute(key_value.first.c_str(),key_value.second.c_str());
    }
    if(!text.empty())
    {
      element->InsertEndChild(doc->NewText(text.c_str()));
    }
    for(auto & child : children)
    {
      element->InsertEndChild(child);
    }
    return element;
  };

  Eigen::IOFormat row_format(Eigen::FullPrecision,0," "," ","","","");
  doc->InsertEndChild(
    ele("COLLADA",
    {
      {"xmlns","http://www.collada.org/2005/11/COLLADASchema"},
      {"version","1.4.1"}
    },
    "",
    {
      ele("asset",{},"",
      {
        ele("unit",{{"meter","0.0254000"},{"name","inch"}}),
        ele("up_axis",{},"Y_UP")
      }),
      ele("library_visual_scenes",{},"",
      {
        ele("visual_scene",{{"id","ID2"}},"",
        {
          ele("node",{{"name","SketchUp"}},"",
          {
            ele("node",{{"id","ID3"},{"name","group_0"}},"",
            {
              ele("matrix",{},"1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"),
              ele("instance_geometry",{{"url","#ID4"}},"",
              {
                ele("bind_material",{},"",{ele("technique_common")}),
              }),
            }),
          }),
        }),
      }),
      ele("library_geometries",{},"",
      {
        ele("geometry",{{"id","ID4"}},"",
        {
          ele("mesh",{},"",
          {
            ele("source",{{"id","ID7"}},"",
            {
              ele(
                "float_array",
                {{"count",STR(V.size())},{"id","ID10"}},
                STR(V.format(row_format))),
              ele("technique_common",{},"",
              {
                ele(
                  "accessor",
                  {{"count",STR(V.rows())},{"source","#ID8"},{"stride","3"}},
                  "",
                {
                  ele("param",{{"name","X"},{"type","float"}}),
                  ele("param",{{"name","Y"},{"type","float"}}),
                  ele("param",{{"name","Z"},{"type","float"}}),
                })
              })
            }),
            ele(
              "vertices",
              {{"id","ID9"}},
              "",
              {ele("input",{{"semantic","POSITION"},{"source","#ID7"}})}),
            ele(
              "triangles",
              {{"count",STR(F.rows())}},
              "",
            {
              ele("input",{{"semantic","VERTEX"},{"source","#ID9"}}),
              ele("p",{},STR(F.format(row_format))),
            })
          })
        })
      }),
      ele("scene",{},"",{ele("instance_visual_scene",{{"url","#ID2"}})}),
    }));
  // tinyxml2 seems **not** to print the <?xml ...?> header by default, but it
  // also seems that that's OK
  XMLError error = doc->SaveFile(filename.c_str());
  bool ret = true;
  if(error != XML_NO_ERROR)
  {
    doc->PrintError();
    ret = false;
  }
  delete doc;
  return ret;
}
