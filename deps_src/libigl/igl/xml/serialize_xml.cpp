// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "serialize_xml.h"
#include "../STR.h"
#include "../serialize.h"
#include "XMLSerializable.h"

#include <iterator>
#include <limits>
#include <iomanip>

namespace igl
{
  namespace xml
  {
    template <typename T>
    IGL_INLINE void serialize_xml(
      const T& obj,
      const std::string& filename)
    {
      serialize_xml(obj,"object",filename,false,true);
    }
  
    template <typename T>
    IGL_INLINE void serialize_xml(
      const T& obj,
      const std::string& objectName,
      const std::string& filename,
      bool binary,
      bool overwrite)
    {
      tinyxml2::XMLDocument* doc = new tinyxml2::XMLDocument();
  
      if(overwrite == false)
      {
        // Check if file exists
        tinyxml2::XMLError error = doc->LoadFile(filename.c_str());
        if(error != tinyxml2::XML_SUCCESS)
        {
          doc->Clear();
        }
      }
  
      tinyxml2::XMLElement* element = doc->FirstChildElement("serialization");
      if(element == NULL)
      {
        element = doc->NewElement("serialization");
        doc->InsertEndChild(element);
      }
  
      serialize_xml(obj,objectName,doc,element,binary);
  
      // Save
      tinyxml2::XMLError error = doc->SaveFile(filename.c_str());
      if(error != tinyxml2::XML_SUCCESS)
      {
        doc->PrintError();
      }
  
      delete doc;
    }
  
    template <typename T>
    IGL_INLINE void serialize_xml(
      const T& obj,
      const std::string& objectName,
      tinyxml2::XMLDocument* doc,
      tinyxml2::XMLElement* element,
      bool binary)
    {
      static_assert(
        serialization_xml::is_serializable<T>::value,
        "'igl::xml::serialize_xml': type is not serializable");
  
      std::string name(objectName);
      serialization_xml::encodeXMLElementName(name);
  
      tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
      
      if(child != NULL)
        element->DeleteChild(child);
  
      child = doc->NewElement(name.c_str());
      element->InsertEndChild(child);
  
      if(binary)
      {
        std::vector<char> buffer;
        serialize(obj,name,buffer);
        std::string data = 
          serialization_xml::base64_encode(
            reinterpret_cast<const unsigned char*>(
              buffer.data()),buffer.size());
        
        child->SetAttribute("binary",true);
  
        serialization_xml::serialize(data,doc,element,name);
      }
      else
      {
        serialization_xml::serialize(obj,doc,element,name);
      }
    }
  
    template <typename T>
    IGL_INLINE void deserialize_xml(T& obj,const std::string& filename)
    {
      deserialize_xml(obj,"object",filename);
    }
  
    template <typename T>
    IGL_INLINE void deserialize_xml(T& obj,const std::string& objectName,const std::string& filename)
    {
      tinyxml2::XMLDocument* doc = new tinyxml2::XMLDocument();
  
      tinyxml2::XMLError error = doc->LoadFile(filename.c_str());
      if(error != tinyxml2::XML_SUCCESS)
      {
        std::cerr << "File not found!" << std::endl;
        doc->PrintError();
        doc = NULL;
      }
      else
      {
        tinyxml2::XMLElement* element = doc->FirstChildElement("serialization");
        if(element == NULL)
        {
          std::cerr << "Name of object not found! Initialized with default value." << std::endl;
          obj = T();
        }
        else
        {
          deserialize_xml(obj,objectName,doc,element);
        }
  
        delete doc;
      }
    }
  
    template <typename T>
    IGL_INLINE void deserialize_xml(T& obj,const std::string& objectName,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element)
    {
      static_assert(serialization::is_serializable<T>::value,"'igl::xml::deserialize_xml': type is not deserializable");
  
      std::string name(objectName);
      serialization_xml::encodeXMLElementName(name);
  
      const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
      if(child != NULL)
      {
        bool isBinary = false;
        const tinyxml2::XMLAttribute* attr = child->FindAttribute("binary");
        if(attr != NULL)
        {
          std::string code;
          serialization_xml::deserialize(code,doc,element,name);
          std::string decoded = serialization_xml::base64_decode(code);
  
          std::vector<char> buffer;
          std::copy(decoded.c_str(),decoded.c_str()+decoded.length(),std::back_inserter(buffer));
  
          deserialize(obj,name,buffer);
        }
        else
        {
          serialization_xml::deserialize(obj,doc,element,name);
        }
      }
    }

    namespace serialization_xml
    {
      // fundamental types
  
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_fundamental<T>::value>::type serialize(const T& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* child = getElement(doc,element,name.c_str());
        child->SetAttribute("val",obj);
      }
  
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_fundamental<T>::value>::type deserialize(T& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          getAttribute(child->Attribute("val"),obj);
        }
        else
        {
          obj = T();
        }
      }
  
      // std::string
  
      IGL_INLINE void serialize(const std::string& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* child = getElement(doc,element,name.c_str());
        child->SetAttribute("val",obj.c_str());
      }
  
      IGL_INLINE void deserialize(std::string& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          getAttribute(child->Attribute("val"),obj);
        }
        else
        {
          obj = std::string("");
        }
      }
  
      // Serializable
  
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_base_of<XMLSerializableBase,T>::value>::type serialize(const T& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        // Serialize object implementing Serializable interface
        const XMLSerializableBase& object = dynamic_cast<const XMLSerializableBase&>(obj);
  
        tinyxml2::XMLElement* child = getElement(doc,element,name.c_str());
        object.Serialize(doc,child);
      }
  
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_base_of<XMLSerializableBase,T>::value>::type deserialize(T& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
  
        if(child != NULL)
        {
          obj.Deserialize(doc,child);
        }
        else
        {
          obj = T();
        }
      }
  
      // STL containers
  
      template <typename T1,typename T2>
      IGL_INLINE void serialize(const std::pair<T1,T2>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* pair = getElement(doc,element,name.c_str());
        serialize(obj.first,doc,pair,"first");
        serialize(obj.second,doc,pair,"second");
      }
  
      template <typename T1,typename T2>
      IGL_INLINE void deserialize(std::pair<T1,T2>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          deserialize(obj.first,doc,child,"first");
          deserialize(obj.second,doc,child,"second");
        }
        else
        {
          obj.first = T1();
          obj.second = T2();
        }
      }
  
      template <typename T1,typename T2>
      IGL_INLINE void serialize(const std::vector<T1,T2>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* vector = getElement(doc,element,name.c_str());
        vector->SetAttribute("size",(unsigned int)obj.size());
  
        std::stringstream num;
        for(unsigned int i=0;i<obj.size();i++)
        {
          num.str("");
          num << "value" << i;
          serialize(obj[i],doc,vector,num.str());
        }
      }
  
      template <typename T1,typename T2>
      IGL_INLINE void deserialize(std::vector<T1,T2>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        obj.clear();
  
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          unsigned int size = child->UnsignedAttribute("size");
          obj.resize(size);
  
          std::stringstream num;
          for(unsigned int i=0;i<size;i++)
          {
            num.str("");
            num << "value" << i;
  
            deserialize(obj[i],doc,child,num.str());
          }
        }
        else
        {
          obj.clear();
        }
      }
  
      template <typename T>
      IGL_INLINE void serialize(const std::set<T>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* set = getElement(doc,element,name.c_str());
        set->SetAttribute("size",(unsigned int)obj.size());
  
        std::stringstream num;
        typename std::set<T>::iterator iter = obj.begin();
        for(int i=0;iter!=obj.end();iter++,i++)
        {
          num.str("");
          num << "value" << i;
          serialize((T)*iter,doc,set,num.str());
        }
      }
  
      template <typename T>
      IGL_INLINE void deserialize(std::set<T>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        obj.clear();
  
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          unsigned int size = child->UnsignedAttribute("size");
  
          std::stringstream num;
          typename std::set<T>::iterator iter = obj.begin();
          for(int i=0;i<size;i++)
          {
            num.str("");
            num << "value" << i;
  
            T val;
            deserialize(val,doc,child,num.str());
            obj.insert(val);
          }
        }
        else
        {
          obj.clear();
        }
      }
  
      template <typename T1,typename T2>
      IGL_INLINE void serialize(const std::map<T1,T2>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* map = getElement(doc,element,name.c_str());
        map->SetAttribute("size",(unsigned int)obj.size());
  
        std::stringstream num;
        typename std::map<T1,T2>::const_iterator iter = obj.cbegin();
        for(int i=0;iter!=obj.end();iter++,i++)
        {
          num.str("");
          num << "value" << i;
          serialize((std::pair<T1,T2>)*iter,doc,map,num.str());
        }
      }
  
      template <typename T1,typename T2>
      IGL_INLINE void deserialize(std::map<T1,T2>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        obj.clear();
  
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          unsigned int size = child->UnsignedAttribute("size");
  
          std::stringstream num;
          typename std::map<T1,T2>::iterator iter = obj.begin();
          for(int i=0;i<size;i++)
          {
            num.str("");
            num << "value" << i;
  
            std::pair<T1,T2> pair;
            deserialize(pair,doc,child,num.str());
            obj.insert(pair);
          }
        }
        else
        {
          obj.clear();
        }
      }

      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void serialize(
        const Eigen::Matrix<T,R,C,P,MR,MC>& obj,
        const std::string& name,
        const std::function<std::string(const T &) >& to_string,
        tinyxml2::XMLDocument* doc,
        tinyxml2::XMLElement* element)
      {
        tinyxml2::XMLElement* matrix = getElement(doc,element,name.c_str());
  
        const unsigned int rows = obj.rows();
        const unsigned int cols = obj.cols();
  
        matrix->SetAttribute("rows",rows);
        matrix->SetAttribute("cols",cols);
  
        std::stringstream ms;
        ms << "\n";
        for(unsigned int r=0;r<rows;r++)
        {
          for(unsigned int c=0;c<cols;c++)
          {
            ms << to_string(obj(r,c)) << ",";
          }
          ms << "\n";
        }
  
        std::string mString = ms.str();
        if(mString.size() > 1)
          mString[mString.size()-2] = '\0';
  
        matrix->SetAttribute("matrix",mString.c_str());
      }
  
      // Eigen types
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void serialize(
        const Eigen::Matrix<T,R,C,P,MR,MC>& obj,
        tinyxml2::XMLDocument* doc,
        tinyxml2::XMLElement* element,
        const std::string& name)
      {
        const std::function<std::string(const T &) > to_string = 
          [](const T & v)->std::string
          {
            return
              STR(std::setprecision(std::numeric_limits<double>::digits10+2)<<v);
          };
        serialize(obj,name,to_string,doc,element);
      }
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void deserialize(
        const tinyxml2::XMLDocument* doc,
        const tinyxml2::XMLElement* element,
        const std::string& name,
        const std::function<void(const std::string &,T &)> & from_string,
        Eigen::Matrix<T,R,C,P,MR,MC>& obj)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        bool initialized = false;
        if(child != NULL)
        {
          const unsigned int rows = child->UnsignedAttribute("rows");
          const unsigned int cols = child->UnsignedAttribute("cols");
  
          if(rows > 0 && cols > 0)
          {
            obj.resize(rows,cols);
  
            const tinyxml2::XMLAttribute* attribute = child->FindAttribute("matrix");
            if(attribute != NULL)
            {
              std::string matTemp;
              getAttribute(attribute->Value(),matTemp);
  
              std::string line,srows,scols;
              std::stringstream mats;
              mats << matTemp;
  
              int r=0;
              std::string val;
              // for each line
              getline(mats,line);
              while(getline(mats,line))
              {
                // get current line
                std::stringstream liness(line);
  
                for(unsigned int c=0;c<cols-1;c++)
                {
                  // split line
                  getline(liness,val,',');
  
                  // push pack the data if any
                  if(!val.empty())
                  {
                    from_string(val,obj.coeffRef(r,c));
                  }
                }
  
                getline(liness,val);
                from_string(val,obj.coeffRef(r,cols-1));
  
                r++;
              }
              initialized = true;
            }
          }
        }
  
        if(!initialized)
        {
          obj = Eigen::Matrix<T,R,C,P,MR,MC>();
        }
      }
  
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void deserialize(
        Eigen::Matrix<T,R,C,P,MR,MC>& obj,
        const tinyxml2::XMLDocument* doc,
        const tinyxml2::XMLElement* element,
        const std::string& name)
      {
        const std::function<void(const std::string &,T &)> & from_string = 
          [](const std::string & s,T & v)
          {
            getAttribute(s.c_str(),v);
          };
        deserialize(doc,element,name,from_string,obj);
      }
  
      template<typename T,int P,typename I>
      IGL_INLINE void serialize(const Eigen::SparseMatrix<T,P,I>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* matrix = getElement(doc,element,name.c_str());
  
        const unsigned int rows = obj.rows();
        const unsigned int cols = obj.cols();
  
        matrix->SetAttribute("rows",rows);
        matrix->SetAttribute("cols",cols);
  
        char buffer[200];
        std::stringstream ms;
        ms << "\n";
        for(int k=0;k<obj.outerSize();++k)
        {
          for(typename Eigen::SparseMatrix<T,P,I>::InnerIterator it(obj,k);it;++it)
          {
            tinyxml2::XMLUtil::ToStr(it.value(),buffer,200);
            ms << it.row() << "," << it.col() << "," << buffer << "\n";
          }
        }
  
        std::string mString = ms.str();
        if(mString.size() > 0)
          mString[mString.size()-1] = '\0';
  
        matrix->SetAttribute("matrix",mString.c_str());
      }
  
      template<typename T,int P,typename I>
      IGL_INLINE void deserialize(Eigen::SparseMatrix<T,P,I>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        bool initialized = false;
        if(child != NULL)
        {
          const unsigned int rows = child->UnsignedAttribute("rows");
          const unsigned int cols = child->UnsignedAttribute("cols");
  
          if(rows > 0 && cols > 0)
          {
            obj.resize(rows,cols);
            obj.setZero();
  
            const tinyxml2::XMLAttribute* attribute = child->FindAttribute("matrix");
            if(attribute != NULL)
            {
              std::string matTemp;
              getAttribute(attribute->Value(),matTemp);
  
              std::string line,srows,scols;
              std::stringstream mats;
              mats << matTemp;
  
              std::vector<Eigen::Triplet<T,I> > triplets;
              int r=0;
              std::string val;
  
              // for each line
              getline(mats,line);
              while(getline(mats,line))
              {
                // get current line
                std::stringstream liness(line);
  
                // row
                getline(liness,val,',');
                int row = atoi(val.c_str());
                // col
                getline(liness,val,',');
                int col = atoi(val.c_str());
                // val
                getline(liness,val);
                T value;
                getAttribute(val.c_str(),value);
  
                triplets.push_back(Eigen::Triplet<T,I>(row,col,value));
  
                r++;
              }
  
              obj.setFromTriplets(triplets.begin(),triplets.end());
              initialized = true;
            }
          }
        }
  
        if(!initialized)
        {
          obj = Eigen::SparseMatrix<T,P,I>();
        }
      }
  
      // pointers
  
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_pointer<T>::value>::type serialize(const T& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* pointer = getElement(doc,element,name.c_str());
  
        bool isNullPtr = (obj == NULL);
  
        pointer->SetAttribute("isNullPtr",isNullPtr);
  
        if(isNullPtr == false)
          serialization_xml::serialize(*obj,doc,element,name);
      }
  
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_pointer<T>::value>::type deserialize(T& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name)
      {
        const tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child != NULL)
        {
          bool isNullPtr = child->BoolAttribute("isNullPtr");
  
          if(isNullPtr)
          {
            if(obj != NULL)
            {
              std::cout << "deserialization: possible memory leak for '" << typeid(obj).name() << "'" << std::endl;
              obj = NULL;
            }
          }
          else
          {
            if(obj != NULL)
              std::cout << "deserialization: possible memory leak for '" << typeid(obj).name() << "'" << std::endl;
  
            obj = new typename std::remove_pointer<T>::type();
  
            serialization_xml::deserialize(*obj,doc,element,name);
          }
        }
      }
  
      // helper functions
  
      IGL_INLINE tinyxml2::XMLElement* getElement(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name)
      {
        tinyxml2::XMLElement* child = element->FirstChildElement(name.c_str());
        if(child == NULL)
        {
          child = doc->NewElement(name.c_str());
          element->InsertEndChild(child);
        }
        return child;
      }
  
      IGL_INLINE void getAttribute(const char* src,bool& dest)
      {
        tinyxml2::XMLUtil::ToBool(src,&dest);
      }
  
      IGL_INLINE void getAttribute(const char* src,char& dest)
      {
        dest = (char)atoi(src);
      }
  
      IGL_INLINE void getAttribute(const char* src,std::string& dest)
      {
        dest = src;
      }
  
      IGL_INLINE void getAttribute(const char* src,float& dest)
      {
        tinyxml2::XMLUtil::ToFloat(src,&dest);
      }
  
      IGL_INLINE void getAttribute(const char* src,double& dest)
      {
        tinyxml2::XMLUtil::ToDouble(src,&dest);
      }
  
      template<typename T>
      IGL_INLINE typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value>::type getAttribute(const char* src,T& dest)
      {
        unsigned int val;
        tinyxml2::XMLUtil::ToUnsigned(src,&val);
        dest = (T)val;
      }
  
      template<typename T>
      IGL_INLINE typename std::enable_if<std::is_integral<T>::value && !std::is_unsigned<T>::value>::type getAttribute(const char* src,T& dest)
      {
        int val;
        tinyxml2::XMLUtil::ToInt(src,&val);
        dest = (T)val;
      }
  
      // tinyXML2 related stuff
      static const int numForbiddenChars = 8;
      static const char forbiddenChars[] ={' ','/','~','#','&','>','<','='};
  
      IGL_INLINE void replaceSubString(std::string& str,const std::string& search,const std::string& replace)
      {
        size_t pos = 0;
        while((pos = str.find(search,pos)) != std::string::npos)
        {
          str.replace(pos,search.length(),replace);
          pos += replace.length();
        }
      }
  
      IGL_INLINE void encodeXMLElementName(std::string& name)
      {
        // must not start with a digit
        if(isdigit(*name.begin()))
        {
          name = ":::" + name;
        }
  
        std::stringstream stream;
        for(int i=0;i<numForbiddenChars;i++)
        {
          std::string search;
          search = forbiddenChars[i];
          std::stringstream replaces;
          replaces << ":" << (int)forbiddenChars[i];
          std::string replace = replaces.str();
  
          replaceSubString(name,search,replace);
        }
      }
  
      IGL_INLINE void decodeXMLElementName(std::string& name)
      {
        if(name.find("::",0) == 0)
          name.replace(0,3,"");
  
        for(auto chr : forbiddenChars)
        {
          std::stringstream searchs;
          searchs << ":" << (int)chr;
          std::string search = searchs.str();
          std::string replace;
          replace = chr;
  
          replaceSubString(name,search,replace);
        }
      }
  
     /* Copyright(C) 2004-2008 Ren� Nyffenegger
  
        This source code is provided 'as-is',without any express or implied
        warranty.In no event will the author be held liable for any damages
        arising from the use of this software.
  
        Permission is granted to anyone to use this software for any purpose,
        including commercial applications,and to alter it and redistribute it
        freely,subject to the following restrictions:
  
        1. The origin of this source code must not be misrepresented; you must not
        claim that you wrote the original source code.If you use this source code
        in a product,an acknowledgment in the product documentation would be
        appreciated but is not required.
  
        2. Altered source versions must be plainly marked as such,and must not be
        misrepresented as being the original source code.
  
        3. This notice may not be removed or altered from any source distribution.
  
        Ren� Nyffenegger rene.nyffenegger@adp-gmbh.ch
        */
  
      static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
  
      static inline bool is_base64(unsigned char c) {
        return (isalnum(c) || (c == '+') || (c == '/'));
      }
  
      std::string base64_encode(unsigned char const* bytes_to_encode,unsigned int in_len)
      {
        std::string ret;
        int i = 0;
        int j = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];
  
        while(in_len--) {
          char_array_3[i++] = *(bytes_to_encode++);
          if(i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
  
            for(i = 0; (i <4) ; i++)
              ret += base64_chars[char_array_4[i]];
  
            i = 0;
          }
        }
  
        if(i)
        {
          for(j = i; j < 3; j++)
            char_array_3[j] = '\0';
  
          char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
          char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
          char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
          char_array_4[3] = char_array_3[2] & 0x3f;
  
          for(j = 0; (j < i + 1); j++)
            ret += base64_chars[char_array_4[j]];
  
          while((i++ < 3))
            ret += '=';
        }
  
        return ret;
      }
  
      std::string base64_decode(std::string const& encoded_string)
      {
        int in_len = encoded_string.size();
        int i = 0;
        int j = 0;
        int in_ = 0;
        unsigned char char_array_4[4],char_array_3[3];
        std::string ret;
  
        // construct fast lookup table
        // added by Christian Sch�ller (schuellc@inf.ethz.ch)
        int charLookup[200];
        for(int i=0;i<(int)(base64_chars.length());i++)
          charLookup[(int)base64_chars[i]] = i;
  
        while(in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
          char_array_4[i++] = encoded_string[in_]; in_++;
          if(i ==4) {
            for(i = 0; i <4; i++)
              char_array_4[i] = charLookup[char_array_4[i]]; // new fast lookup
            //char_array_4[i] = base64_chars.find(char_array_4[i]); // original version
  
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
  
            for(i = 0; (i < 3); i++)
              ret += char_array_3[i];
  
            i = 0;
          }
        }
  
        if(i) {
          for(j = i; j <4; j++)
            char_array_4[j] = 0;
  
          for(j = 0; j <4; j++)
            char_array_4[j] = base64_chars.find(char_array_4[j]);
  
          char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
          char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
          char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
  
          for(j = 0; (j < i - 1);
            j++) ret += char_array_3[j];
        }
  
        return ret;
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::xml::serialize_xml<std::vector<float, std::allocator<float> > >(std::vector<float, std::allocator<float> > const&, std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, bool, bool);
template void igl::xml::deserialize_xml<std::vector<float, std::allocator<float> > >(std::vector<float, std::allocator<float> >&, std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&);
#endif
