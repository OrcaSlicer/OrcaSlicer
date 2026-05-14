//
// Copyright (C) 2014 Christian Sch�ller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_XML_SERIALIZABLE_XML_H
#define IGL_XML_SERIALIZABLE_XML_H
// -----------------------------------------------------------------------------
// Functions to save and load a serialization of fundamental c++ data types to
// and from a xml file. STL containers, Eigen matrix types and nested data
// structures are also supported. To serialize a user defined class implement
// the interface XMLSerializable or XMLSerializableBase.
//
// See also: serialize.h
// -----------------------------------------------------------------------------

#include "../igl_inline.h"


#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <tinyxml2.h>

#include <type_traits>
#include <functional>
#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <memory>

//#define SERIALIZE_XML(x) igl::xml::serialize_xml(x,#x,doc,element);
//#define DESERIALIZE_XML(x) igl::xml::deserialize_xml(x,#x,,doc,element);

namespace igl
{
  namespace xml
  {
    struct XMLSerializableBase;
    /// Serialize object to file
    /// @tparam T  type of the object to serialize
    /// @param[in] obj        object to serialize
    /// @param[in] filename   name of the file containing the serialization
    /// Serialize object to file
    ///
    /// @tparam T  type of the object to serialize
    /// @param[in] obj        object to serialize
    /// @param[in] objectName unique object name,used for the identification
    /// @param[in] filename   name of the file containing the serialization
    /// @param[in] binary     set to true to serialize the object in binary format (faster for big data)
    /// @param[in] overwrite  set to true to overwrite an existing xml file
    template <typename T>
    IGL_INLINE void serialize_xml(
      const T& obj,
      const std::string& objectName,
      const std::string& filename,
      bool binary = false,
      bool overwrite = false);
    /// \overload
    template <typename T>
    IGL_INLINE void serialize_xml(const T& obj,const std::string& filename);
    /// \overload
    ///
    /// @param[in,out] doc        contains current tinyxml2 virtual representation of the xml data
    /// @param[in,out] element    tinyxml2 virtual representation of the current xml node
    template <typename T>
    IGL_INLINE void serialize_xml(
      const T& obj,
      const std::string& objectName,
      tinyxml2::XMLDocument* doc,
      tinyxml2::XMLElement* element,
      bool binary = false);

    /// deserialize object to file
    ///
    /// @tparam T  type of the object to serialize
    /// @param[in] obj        object to serialize
    /// @param[in] objectName unique object name,used for the identification
    /// @param[in] filename   name of the file containing the serialization
    /// @param[in] binary     set to true to serialize the object in binary format (faster for big data)
    /// @param[in] overwrite  set to true to overwrite an existing xml file
    ///
    /// \fileinfo
    template <typename T>
    IGL_INLINE void deserialize_xml(T& obj,const std::string& objectName,const std::string& filename);
    /// \overload
    ///
    /// \fileinfo
    template <typename T>
    IGL_INLINE void deserialize_xml(T& obj,const std::string& filename);
    /// Deserialize to xml doc
    ///
    /// @param[in,out] doc        contains current tinyxml2 virtual representation of the xml data
    /// @param[in,out] element    tinyxml2 virtual representation of the current xml node
    ///
    /// \fileinfo
    template <typename T>
    IGL_INLINE void deserialize_xml(T& obj,const std::string& objectName,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element);

    // internal functions
    /// @private
    namespace serialization_xml
    {
      // fundamental types
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_fundamental<T>::value>::type serialize(const T& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_fundamental<T>::value>::type deserialize(T& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      // std::string
      IGL_INLINE void serialize(const std::string& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      IGL_INLINE void deserialize(std::string& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      // XMLSerializableBase
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_base_of<XMLSerializableBase,T>::value>::type serialize(const T& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_base_of<XMLSerializableBase,T>::value>::type deserialize(T& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      // STL containers
      template <typename T1, typename T2>
      IGL_INLINE void serialize(const std::pair<T1,T2>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T1,typename T2>
      IGL_INLINE void deserialize(std::pair<T1,T2>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      template <typename T1,typename T2>
      IGL_INLINE void serialize(const std::vector<T1,T2>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T1,typename T2>
      IGL_INLINE void deserialize(std::vector<T1,T2>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      template <typename T>
      IGL_INLINE void serialize(const std::set<T>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T>
      IGL_INLINE void deserialize(std::set<T>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      template <typename T1,typename T2>
      IGL_INLINE void serialize(const std::map<T1,T2>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T1,typename T2>
      IGL_INLINE void deserialize(std::map<T1,T2>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      // Eigen types

      // Serialize a Dense Eigen Matrix to xml (in the matrix= attribute,
      // awkward...)
      //
      // Inputs:
      //   obj  MR by MC matrix of T types
      //   name  name of matrix
      //   to_string  function converting T to string
      // Outputs:
      //   doc  pointer to xml document
      //   element  pointer to xml element
      //
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void serialize(
        const Eigen::Matrix<T,R,C,P,MR,MC>& obj,
        const std::string& name,
        const std::function<std::string(const T &) >& to_string,
        tinyxml2::XMLDocument* doc,
        tinyxml2::XMLElement* element);
      // De-Serialize a Dense Eigen Matrix from xml (in the matrix= attribute,
      // awkward...)
      //
      // Inputs:
      //   doc  pointer to xml document
      //   element  pointer to xml element
      //   name  name of matrix
      //   from_string  function string to T
      // Outputs:
      //   obj  MR by MC matrix of T types
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void deserialize(
        const tinyxml2::XMLDocument* doc,
        const tinyxml2::XMLElement* element,
        const std::string& name,
        const std::function<void(const std::string &,T &)> & from_string,
        Eigen::Matrix<T,R,C,P,MR,MC>& obj);

      // Legacy APIs
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void serialize(
        const Eigen::Matrix<T,R,C,P,MR,MC>& obj,
        tinyxml2::XMLDocument* doc,
        tinyxml2::XMLElement* element,
        const std::string& name);
      template<typename T,int R,int C,int P,int MR,int MC>
      IGL_INLINE void deserialize(
        Eigen::Matrix<T,R,C,P,MR,MC>& obj,
        const tinyxml2::XMLDocument* doc,
        const tinyxml2::XMLElement* element,
        const std::string& name);

      template<typename T,int P,typename I>
      IGL_INLINE void serialize(const Eigen::SparseMatrix<T,P,I>& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template<typename T,int P,typename I>
      IGL_INLINE void deserialize(Eigen::SparseMatrix<T,P,I>& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      // raw pointers
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_pointer<T>::value>::type serialize(const T& obj,tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      template <typename T>
      IGL_INLINE typename std::enable_if<std::is_pointer<T>::value>::type deserialize(T& obj,const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element,const std::string& name);

      // helper functions
      tinyxml2::XMLElement* getElement(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element,const std::string& name);
      IGL_INLINE void getAttribute(const char* src,bool& dest);
      IGL_INLINE void getAttribute(const char* scr,char& dest);
      IGL_INLINE void getAttribute(const char* src,std::string& dest);
      IGL_INLINE void getAttribute(const char* src,float& dest);
      IGL_INLINE void getAttribute(const char* src,double& dest);
      template<typename T>
      IGL_INLINE typename std::enable_if<std::is_integral<T>::value && std::is_unsigned<T>::value>::type getAttribute(const char* src,T& dest);
      template<typename T>
      IGL_INLINE typename std::enable_if<std::is_integral<T>::value && !std::is_unsigned<T>::value>::type getAttribute(const char* src,T& dest);
      IGL_INLINE void replaceSubString(std::string& str,const std::string& search,const std::string& replace);
      IGL_INLINE void encodeXMLElementName(std::string& name);
      IGL_INLINE void decodeXMLElementName(std::string& name);
      IGL_INLINE std::string base64_encode(unsigned char const* bytes_to_encode,unsigned int in_len);
      IGL_INLINE std::string base64_decode(std::string const& encoded_string);

      // compile time type serializable check
      template <typename T>
      struct is_stl_container { static const bool value = false; };
      template <typename T1,typename T2>
      struct is_stl_container<std::pair<T1,T2> > { static const bool value = true; };
      template <typename T1,typename T2>
      struct is_stl_container<std::vector<T1,T2> > { static const bool value = true; };
      template <typename T>
      struct is_stl_container<std::set<T> > { static const bool value = true; };
      template <typename T1,typename T2>
      struct is_stl_container<std::map<T1,T2> > { static const bool value = true; };

      template <typename T>
      struct is_eigen_type { static const bool value = false; };
      template <typename T,int R,int C,int P,int MR,int MC>
      struct is_eigen_type<Eigen::Matrix<T,R,C,P,MR,MC> > { static const bool value = true; };
      template <typename T,int P,typename I>
      struct is_eigen_type<Eigen::SparseMatrix<T,P,I> > { static const bool value = true; };

      template <typename T>
      struct is_serializable {
        using T0 = typename  std::remove_pointer<T>::type;
        static const bool value = std::is_fundamental<T0>::value || std::is_same<std::string,T0>::value || std::is_base_of<XMLSerializableBase,T0>::value
          || is_stl_container<T0>::value || is_eigen_type<T0>::value;
      };
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
  #include "serialize_xml.cpp"
#endif

#endif
