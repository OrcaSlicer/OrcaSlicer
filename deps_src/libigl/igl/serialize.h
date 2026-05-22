// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SERIALIZE_H
#define IGL_SERIALIZE_H

// -----------------------------------------------------------------------------
// Functions to save and load a serialization of fundamental c++ data types to
// and from a binary file. STL containers, Eigen matrix types and nested data
// structures are also supported. To serialize a user defined class implement
// the interface Serializable or SerializableBase.
//
// See also: xml/serialize_xml.h
// -----------------------------------------------------------------------------
// TODOs:
// * arbitrary pointer graph structures
// -----------------------------------------------------------------------------

// Known issues: This is not written in libigl-style so it isn't (easily)
// "dualized" into the static library.
//

#include <type_traits>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <numeric>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <cstdint>
#include <list>

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include "igl_inline.h"

// non-intrusive serialization helper macros

#define SERIALIZE_TYPE(Type,Params) \
namespace igl { namespace serialization { \
  void _serialization(bool s,Type& obj,std::vector<char>& buffer) {Params} \
  template<> inline void serialize(const Type& obj,std::vector<char>& buffer) { \
    _serialization(true,const_cast<Type&>(obj),buffer); \
    } \
  template<> inline void deserialize(Type& obj,const std::vector<char>& buffer) { \
    _serialization(false,obj,const_cast<std::vector<char>&>(buffer)); \
    } \
}}

#define SERIALIZE_TYPE_SOURCE(Type,Params) \
namespace igl { namespace serialization { \
  void _serialization(bool s,Type& obj,std::vector<char>& buffer) {Params} \
  void _serialize(const Type& obj,std::vector<char>& buffer) { \
    _serialization(true,const_cast<Type&>(obj),buffer); \
    } \
  void _deserialize(Type& obj,const std::vector<char>& buffer) { \
    _serialization(false,obj,const_cast<std::vector<char>&>(buffer)); \
    } \
}}

#define SERIALIZE_MEMBER(Object) igl::serializer(s,obj.Object,std::string(#Object),buffer);
#define SERIALIZE_MEMBER_NAME(Object,Name) igl::serializer(s,obj.Object,std::string(Name),buffer);


namespace igl
{
  struct IndexedPointerBase;

  // Serializes the given object either to a file or to a provided buffer
  // Templates:
  //   T  type of the object to serialize
  // Inputs:
  //   obj        object to serialize
  //   objectName unique object name,used for the identification
  //   overwrite  set to true to overwrite an existing file
  //   filename   name of the file containing the serialization
  // Outputs:
  //   buffer     binary serialization
  //
  template <typename T>
  inline bool serialize(const T& obj,const std::string& filename);
  template <typename T>
  inline bool serialize(const T& obj,const std::string& objectName,const std::string& filename,bool overwrite = false);
  template <typename T>
  inline bool serialize(const T& obj,const std::string& objectName,std::vector<char>& buffer);
  template <typename T>
  inline bool serialize(const T& obj,const std::string& objectName,std::vector<char>& buffer);

  // Deserializes the given data from a file or buffer back to the provided object
  //
  // Templates:
  //   T  type of the object to serialize
  // Inputs:
  //   buffer     binary serialization
  //   objectName unique object name, used for the identification
  //   filename   name of the file containing the serialization
  // Outputs:
  //   obj        object to load back serialization to
  //
  template <typename T>
  inline bool deserialize(T& obj,const std::string& filename);
  template <typename T>
  inline bool deserialize(T& obj,const std::string& objectName,const std::string& filename);
  template <typename T>
  inline bool deserialize(T& obj,const std::string& objectName,const std::vector<char>& buffer);

  // Wrapper to expose both, the de- and serialization as one function
  //
  template <typename T>
  inline bool serializer(bool serialize,T& obj,const std::string& filename);
  template <typename T>
  inline bool serializer(bool serialize,T& obj,const std::string& objectName,const std::string& filename,bool overwrite = false);
  template <typename T>
  inline bool serializer(bool serialize,T& obj,const std::string& objectName,std::vector<char>& buffer);

  // User defined types have to either overload the function igl::serialization::serialize()
  // and igl::serialization::deserialize() for their type (non-intrusive serialization):
  //
  // namespace igl { namespace serialization
  // {
  //   template<>
  //   inline void serialize(const UserType& obj,std::vector<char>& buffer) {
  //     ::igl::serialize(obj.var,"var",buffer);
  //   }
  //
  //   template<>
  //   inline void deserialize(UserType& obj,const std::vector<char>& buffer) {
  //     ::igl::deserialize(obj.var,"var",buffer);
  //   }
  // }}
  //
  // or use this macro for convenience:
  //
  // SERIALIZE_TYPE(UserType,
  //   SERIALIZE_MEMBER(var)
  // )
  //
  // or to derive from the class Serializable and add their the members
  // in InitSerialization like the following:
  //
  // class UserType : public igl::Serializable {
  //
  //   int var;
  //
  //   void InitSerialization() {
  //     this->Add(var,"var");
  //   }
  // };

  // Base interface for user defined types
  struct SerializableBase
  {
    virtual ~SerializableBase() = default;
    virtual void Serialize(std::vector<char>& buffer) const = 0;
    virtual void Deserialize(const std::vector<char>& buffer) = 0;
  };

  // Convenient interface for user defined types
  class Serializable: public SerializableBase
  {
  private:

    template <typename T>
    struct SerializationObject : public SerializableBase
    {
      bool Binary;
      std::string Name;
      std::unique_ptr<T> Object;

      void Serialize(std::vector<char>& buffer) const override {
        igl::serialize(*Object,Name,buffer);
      }

      void Deserialize(const std::vector<char>& buffer) override {
        igl::deserialize(*Object,Name,buffer);
      }
    };

    mutable bool initialized;
    mutable std::vector<SerializableBase*> objects;

  public:

    // You **MUST** Override this function to add your member variables which
    // should be serialized
    //
    // http://stackoverflow.com/a/6634382/148668
    virtual void InitSerialization() = 0;

    // Following functions can be overridden to handle the specific events.
    // Return false to prevent the de-/serialization of an object.
    inline virtual bool PreSerialization() const;
    inline virtual void PostSerialization() const;
    inline virtual bool PreDeserialization();
    inline virtual void PostDeserialization();

    // Default implementation of SerializableBase interface
    inline void Serialize(std::vector<char>& buffer) const override final;
    inline void Deserialize(const std::vector<char>& buffer) override final;

    // Default constructor, destructor, assignment and copy constructor
    inline Serializable();
    inline Serializable(const Serializable& obj);
    virtual inline ~Serializable();
    inline Serializable& operator=(const Serializable& obj);

    // Use this function to add your variables which should be serialized
    template <typename T>
    inline void Add(T& obj,std::string name,bool binary = false);
  };

  // structure for pointer handling
  struct IndexedPointerBase
  {
    enum { BEGIN,END } Type;
    size_t Index;
  };
  template<typename T>
  struct IndexedPointer: public IndexedPointerBase
  {
    const T* Object;
  };

  // internal functions
  namespace serialization
  {
    // compile time type checks
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
    struct is_stl_container<std::list<T> > { static const bool value = true; };

    template <typename T>
    struct is_eigen_type { static const bool value = false; };
    template <typename T,int R,int C,int P,int MR,int MC>
    struct is_eigen_type<Eigen::Matrix<T,R,C,P,MR,MC> > { static const bool value = true; };
    template <typename T,int R,int C,int P,int MR,int MC>
    struct is_eigen_type<Eigen::Array<T,R,C,P,MR,MC> > { static const bool value = true; };
    template <typename T,int P,typename I>
    struct is_eigen_type<Eigen::SparseMatrix<T,P,I> > { static const bool value = true; };

    template <typename T>
    struct is_smart_ptr { static const bool value = false; };
    template <typename T>
    struct is_smart_ptr<std::shared_ptr<T> > { static const bool value = true; };
    template <typename T>
    struct is_smart_ptr<std::unique_ptr<T> > { static const bool value = true; };
    template <typename T>
    struct is_smart_ptr<std::weak_ptr<T> > { static const bool value = true; };

    template <typename T>
    struct is_serializable {
      static const bool value = std::is_fundamental<T>::value || std::is_same<std::string,T>::value || std::is_enum<T>::value || std::is_base_of<SerializableBase,T>::value
        || is_stl_container<T>::value || is_eigen_type<T>::value || std::is_pointer<T>::value || serialization::is_smart_ptr<T>::value;
    };

    // non serializable types
    template <typename T>
    inline typename std::enable_if<!is_serializable<T>::value,size_t>::type getByteSize(const T& obj);
    template <typename T>
    inline typename std::enable_if<!is_serializable<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline typename std::enable_if<!is_serializable<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter);

    // fundamental types
    template <typename T>
    inline typename std::enable_if<std::is_fundamental<T>::value,size_t>::type getByteSize(const T& obj);
    template <typename T>
    inline typename std::enable_if<std::is_fundamental<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline typename std::enable_if<std::is_fundamental<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter);

    // std::string
    inline size_t getByteSize(const std::string& obj);
    inline void serialize(const std::string& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    inline void deserialize(std::string& obj,std::vector<char>::const_iterator& iter);

    // enum types
    template <typename T>
    inline typename std::enable_if<std::is_enum<T>::value,size_t>::type getByteSize(const T& obj);
    template <typename T>
    inline typename std::enable_if<std::is_enum<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline typename std::enable_if<std::is_enum<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter);

    // SerializableBase
    template <typename T>
    inline typename std::enable_if<std::is_base_of<SerializableBase,T>::value,size_t>::type getByteSize(const T& obj);
    template <typename T>
    inline typename std::enable_if<std::is_base_of<SerializableBase,T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline typename std::enable_if<std::is_base_of<SerializableBase,T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter);

    // stl containers
    // std::pair
    template <typename T1,typename T2>
    inline size_t getByteSize(const std::pair<T1,T2>& obj);
    template <typename T1,typename T2>
    inline void serialize(const std::pair<T1,T2>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T1,typename T2>
    inline void deserialize(std::pair<T1,T2>& obj,std::vector<char>::const_iterator& iter);

    // std::vector
    template <typename T1,typename T2>
    inline size_t getByteSize(const std::vector<T1,T2>& obj);
    template <typename T1,typename T2>
    inline void serialize(const std::vector<T1,T2>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T1,typename T2>
    inline void deserialize(std::vector<T1,T2>& obj,std::vector<char>::const_iterator& iter);
    template <typename T2>
    inline void deserialize(std::vector<bool,T2>& obj,std::vector<char>::const_iterator& iter);

    // std::set
    template <typename T>
    inline size_t getByteSize(const std::set<T>& obj);
    template <typename T>
    inline void serialize(const std::set<T>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline void deserialize(std::set<T>& obj,std::vector<char>::const_iterator& iter);

    // std::map
    template <typename T1,typename T2>
    inline size_t getByteSize(const std::map<T1,T2>& obj);
    template <typename T1,typename T2>
    inline void serialize(const std::map<T1,T2>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T1,typename T2>
    inline void deserialize(std::map<T1,T2>& obj,std::vector<char>::const_iterator& iter);

    // std::list
    template <typename T>
    inline size_t getByteSize(const std::list<T>& obj);
    template <typename T>
    inline void serialize(const std::list<T>& obj, std::vector<char>& buffer, std::vector<char>::iterator& iter);
    template <typename T>
    inline void deserialize(std::list<T>& obj, std::vector<char>::const_iterator& iter);

    // Eigen types
    template<typename T,int R,int C,int P,int MR,int MC>
    inline size_t getByteSize(const Eigen::Matrix<T,R,C,P,MR,MC>& obj);
    template<typename T,int R,int C,int P,int MR,int MC>
    inline void serialize(const Eigen::Matrix<T,R,C,P,MR,MC>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template<typename T,int R,int C,int P,int MR,int MC>
    inline void deserialize(Eigen::Matrix<T,R,C,P,MR,MC>& obj,std::vector<char>::const_iterator& iter);

    template<typename T,int R,int C,int P,int MR,int MC>
    inline size_t getByteSize(const Eigen::Array<T,R,C,P,MR,MC>& obj);
    template<typename T,int R,int C,int P,int MR,int MC>
    inline void serialize(const Eigen::Array<T,R,C,P,MR,MC>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template<typename T,int R,int C,int P,int MR,int MC>
    inline void deserialize(Eigen::Array<T,R,C,P,MR,MC>& obj,std::vector<char>::const_iterator& iter);

    template<typename T,int P,typename I>
    inline size_t getByteSize(const Eigen::SparseMatrix<T,P,I>& obj);
    template<typename T,int P,typename I>
    inline void serialize(const Eigen::SparseMatrix<T,P,I>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template<typename T,int P,typename I>
    inline void deserialize(Eigen::SparseMatrix<T,P,I>& obj,std::vector<char>::const_iterator& iter);

    template<typename T,int P>
    inline size_t getByteSize(const Eigen::Quaternion<T,P>& obj);
    template<typename T,int P>
    inline void serialize(const Eigen::Quaternion<T,P>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template<typename T,int P>
    inline void deserialize(Eigen::Quaternion<T,P>& obj,std::vector<char>::const_iterator& iter);

    // raw pointers
    template <typename T>
    inline typename std::enable_if<std::is_pointer<T>::value,size_t>::type getByteSize(const T& obj);
    template <typename T>
    inline typename std::enable_if<std::is_pointer<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline typename std::enable_if<std::is_pointer<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter);

    // std::shared_ptr and std::unique_ptr
    template <typename T>
    inline typename std::enable_if<serialization::is_smart_ptr<T>::value,size_t>::type getByteSize(const T& obj);
    template <typename T>
    inline typename std::enable_if<serialization::is_smart_ptr<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <template<typename> class T0, typename T1>
    inline typename std::enable_if<serialization::is_smart_ptr<T0<T1> >::value>::type deserialize(T0<T1>& obj,std::vector<char>::const_iterator& iter);

    // std::weak_ptr
    template <typename T>
    inline size_t getByteSize(const std::weak_ptr<T>& obj);
    template <typename T>
    inline void serialize(const std::weak_ptr<T>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter);
    template <typename T>
    inline void deserialize(std::weak_ptr<T>& obj,std::vector<char>::const_iterator& iter);

    // functions to overload for non-intrusive serialization
    template <typename T>
    inline void serialize(const T& obj,std::vector<char>& buffer);
    template <typename T>
    inline void deserialize(T& obj,const std::vector<char>& buffer);

    // helper functions
    template <typename T>
    inline void updateMemoryMap(T& obj,size_t size);
  }
}

// Always include inlines for these functions

// IMPLEMENTATION

namespace igl
{
  template <typename T>
  inline bool serialize(const T& obj,const std::string& filename)
  {
    return serialize(obj,"obj",filename,true);
  }

  template <typename T>
  inline bool serialize(const T& obj,const std::string& objectName,const std::string& filename,bool overwrite)
  {
    bool success = false;

    std::vector<char> buffer;

    std::ios_base::openmode mode = std::ios::out | std::ios::binary;

    if(overwrite)
      mode |= std::ios::trunc;
    else
      mode |= std::ios::app;

    std::ofstream file(filename.c_str(),mode);

    if(file.is_open())
    {
      serialize(obj,objectName,buffer);

      file.write(&buffer[0],buffer.size());

      file.close();

      success = true;
    }
    else
    {
      std::cerr << "serialization: file " << filename << " not found!" << std::endl;
    }

    return success;
  }

  template <typename T>
  inline bool serialize(const T& obj,const std::string& objectName,std::vector<char>& buffer)
  {
    // serialize object data
    size_t size = serialization::getByteSize(obj);
    std::vector<char> tmp(size);
    auto it = tmp.begin();
    serialization::serialize(obj,tmp,it);

    std::string objectType(typeid(obj).name());
    size_t newObjectSize = tmp.size();
    size_t newHeaderSize = serialization::getByteSize(objectName) + serialization::getByteSize(objectType) + sizeof(size_t);
    size_t curSize = buffer.size();
    size_t newSize = curSize + newHeaderSize + newObjectSize;

    buffer.resize(newSize);

    std::vector<char>::iterator iter = buffer.begin()+curSize;

    // serialize object header (name/type/size)
    serialization::serialize(objectName,buffer,iter);
    serialization::serialize(objectType,buffer,iter);
    serialization::serialize(newObjectSize,buffer,iter);

    // copy serialized data to buffer
    iter = std::copy(tmp.begin(),tmp.end(),iter);

    return true;
  }

  template <typename T>
  inline bool deserialize(T& obj,const std::string& filename)
  {
    return deserialize(obj,"obj",filename);
  }

  template <typename T>
  inline bool deserialize(T& obj,const std::string& objectName,const std::string& filename)
  {
    bool success = false;

    std::ifstream file(filename.c_str(),std::ios::binary);

    if(file.is_open())
    {
      file.seekg(0,std::ios::end);
      std::streamoff size = file.tellg();
      file.seekg(0,std::ios::beg);

      std::vector<char> buffer(size);
      file.read(&buffer[0],size);

      success = deserialize(obj, objectName, buffer);
      file.close();
    }
    else
    {
      std::cerr << "serialization: file " << filename << " not found!" << std::endl;
    }

    return success;
  }

  template <typename T>
  inline bool deserialize(T& obj,const std::string& objectName,const std::vector<char>& buffer)
  {
    bool success = false;

    // find suitable object header
    auto objectIter = buffer.cend();
    auto iter = buffer.cbegin();
    while(iter != buffer.end())
    {
      std::string name;
      std::string type;
      size_t size;
      serialization::deserialize(name,iter);
      serialization::deserialize(type,iter);
      serialization::deserialize(size,iter);

      if(name == objectName && type == typeid(obj).name())
      {
        objectIter = iter;
        //break; // find first suitable object header
      }

      iter+=size;
    }

    if(objectIter != buffer.end())
    {
      serialization::deserialize(obj,objectIter);
      success = true;
    }
    else
    {
      obj = T();
    }

    return success;
  }

  // Wrapper function which combines both, de- and serialization
  template <typename T>
  inline bool serializer(bool s,T& obj,const std::string& filename)
  {
    return s ? serialize(obj,filename) : deserialize(obj,filename);
  }

  template <typename T>
  inline bool serializer(bool s,T& obj,const std::string& objectName,const std::string& filename,bool overwrite)
  {
    return s ? serialize(obj,objectName,filename,overwrite) : deserialize(obj,objectName,filename);
  }

  template <typename T>
  inline bool serializer(bool s,T& obj,const std::string& objectName,std::vector<char>& buffer)
  {
    return s ? serialize(obj,objectName,buffer) : deserialize(obj,objectName,buffer);
  }

  inline bool Serializable::PreSerialization() const
  {
    return true;
  }

  inline void Serializable::PostSerialization() const
  {
  }

  inline bool Serializable::PreDeserialization()
  {
    return true;
  }

  inline void Serializable::PostDeserialization()
  {
  }

  inline void Serializable::Serialize(std::vector<char>& buffer) const
  {
    if(this->PreSerialization())
    {
      if(initialized == false)
      {
        objects.clear();
        (const_cast<Serializable*>(this))->InitSerialization();
        initialized = true;
      }

      for(const auto& v : objects)
      {
        v->Serialize(buffer);
      }

      this->PostSerialization();
    }
  }

  inline void Serializable::Deserialize(const std::vector<char>& buffer)
  {
    if(this->PreDeserialization())
    {
      if(initialized == false)
      {
        objects.clear();
        (const_cast<Serializable*>(this))->InitSerialization();
        initialized = true;
      }

      for(auto& v : objects)
      {
        v->Deserialize(buffer);
      }

      this->PostDeserialization();
    }
  }

  inline Serializable::Serializable()
  {
    initialized = false;
  }

  inline Serializable::Serializable(const Serializable& /*obj*/)
  {
    initialized = false;
    objects.clear();
  }

  inline Serializable::~Serializable()
  {
    initialized = false;
    objects.clear();
  }

  inline Serializable& Serializable::operator=(const Serializable& obj)
  {
    if(this != &obj)
    {
      if(initialized)
      {
        initialized = false;
        objects.clear();
      }
    }
    return *this;
  }

  template <typename T>
  inline void Serializable::Add(T& obj,const std::string name,bool binary)
  {
    auto object = new SerializationObject<T>();
    object->Binary = binary;
    object->Name = name;
    object->Object = std::unique_ptr<T>(&obj);

    objects.push_back(object);
  }

  namespace serialization
  {
    template <typename T>
    inline typename std::enable_if<!is_serializable<T>::value,size_t>::type getByteSize(const T& /*obj*/)
    {
      return sizeof(std::vector<char>::size_type);
    }

    template <typename T>
    inline typename std::enable_if<!is_serializable<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      // data
      std::vector<char> tmp;
      serialize<>(obj,tmp);

      // size
      size_t size = buffer.size();
      serialization::serialize(tmp.size(),buffer,iter);
      size_t cur = iter - buffer.begin();

      buffer.resize(size+tmp.size());
      iter = buffer.begin()+cur;
      iter = std::copy(tmp.begin(),tmp.end(),iter);
    }

    template <typename T>
    inline typename std::enable_if<!is_serializable<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter)
    {
      std::vector<char>::size_type size;
      serialization::deserialize<>(size,iter);

      std::vector<char> tmp;
      tmp.resize(size);
      std::copy(iter,iter+size,tmp.begin());

      deserialize<>(obj,tmp);
      iter += size;
    }

    // fundamental types

    template <typename T>
    inline typename std::enable_if<std::is_fundamental<T>::value,size_t>::type getByteSize(const T& /*obj*/)
    {
      return sizeof(T);
    }

    template <typename T>
    inline typename std::enable_if<std::is_fundamental<T>::value>::type serialize(const T& obj,std::vector<char>& /*buffer*/,std::vector<char>::iterator& iter)
    {
      //serialization::updateMemoryMap(obj,sizeof(T));
      const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(&obj);
      iter = std::copy(ptr,ptr+sizeof(T),iter);
    }

    template <typename T>
    inline typename std::enable_if<std::is_fundamental<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter)
    {
      std::uint8_t* ptr = reinterpret_cast<std::uint8_t*>(&obj);
      std::copy(iter,iter+sizeof(T),ptr);
      iter += sizeof(T);
    }

    // std::string

    inline size_t getByteSize(const std::string& obj)
    {
      return getByteSize(obj.length())+obj.length()*sizeof(std::uint8_t);
    }

    inline void serialize(const std::string& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.length(),buffer,iter);
      for(const auto& cur : obj)
      {
        serialization::serialize(cur,buffer,iter);
      }
    }

    inline void deserialize(std::string& obj,std::vector<char>::const_iterator& iter)
    {
      size_t size;
      serialization::deserialize(size,iter);

      std::string str(size,'\0');
      for(size_t i=0; i<size; ++i)
      {
        serialization::deserialize(str.at(i),iter);
      }

      obj = str;
    }

    // enum types

    template <typename T>
    inline typename std::enable_if<std::is_enum<T>::value,size_t>::type getByteSize(const T& /*obj*/)
    {
      return sizeof(T);
    }

    template <typename T>
    inline typename std::enable_if<std::is_enum<T>::value>::type serialize(const T& obj,std::vector<char>& /*buffer*/,std::vector<char>::iterator& iter)
    {
      const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(&obj);
      iter = std::copy(ptr,ptr+sizeof(T),iter);
    }

    template <typename T>
    inline typename std::enable_if<std::is_enum<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter)
    {
      std::uint8_t* ptr = reinterpret_cast<std::uint8_t*>(&obj);
      std::copy(iter,iter+sizeof(T),ptr);
      iter += sizeof(T);
    }

    // SerializableBase

    template <typename T>
    inline typename std::enable_if<std::is_base_of<SerializableBase,T>::value,size_t>::type getByteSize(const T& /*obj*/)
    {
      return sizeof(std::vector<char>::size_type);
    }

    template <typename T>
    inline typename std::enable_if<std::is_base_of<SerializableBase,T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      // data
      std::vector<char> tmp;
      obj.Serialize(tmp);

      // size
      size_t size = buffer.size();
      serialization::serialize(tmp.size(),buffer,iter);
      size_t cur = iter - buffer.begin();

      buffer.resize(size+tmp.size());
      iter = buffer.begin()+cur;
      iter = std::copy(tmp.begin(),tmp.end(),iter);
    }

    template <typename T>
    inline typename std::enable_if<std::is_base_of<SerializableBase,T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter)
    {
      std::vector<char>::size_type size;
      serialization::deserialize(size,iter);

      std::vector<char> tmp;
      tmp.resize(size);
      std::copy(iter,iter+size,tmp.begin());

      obj.Deserialize(tmp);
      iter += size;
    }

    // STL containers

    // std::pair

    template <typename T1,typename T2>
    inline size_t getByteSize(const std::pair<T1,T2>& obj)
    {
      return getByteSize(obj.first)+getByteSize(obj.second);
    }

    template <typename T1,typename T2>
    inline void serialize(const std::pair<T1,T2>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.first,buffer,iter);
      serialization::serialize(obj.second,buffer,iter);
    }

    template <typename T1,typename T2>
    inline void deserialize(std::pair<T1,T2>& obj,std::vector<char>::const_iterator& iter)
    {
      serialization::deserialize(obj.first,iter);
      serialization::deserialize(obj.second,iter);
    }

    // std::vector

    template <typename T1,typename T2>
    inline size_t getByteSize(const std::vector<T1,T2>& obj)
    {
      return std::accumulate(obj.begin(),obj.end(),sizeof(size_t),[](const size_t& acc,const T1& cur) { return acc+getByteSize(cur); });
    }

    template <typename T1,typename T2>
    inline void serialize(const std::vector<T1,T2>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      size_t size = obj.size();
      serialization::serialize(size,buffer,iter);
      for(const T1& cur : obj)
      {
        serialization::serialize(cur,buffer,iter);
      }
    }

    template <typename T1,typename T2>
    inline void deserialize(std::vector<T1,T2>& obj,std::vector<char>::const_iterator& iter)
    {
      size_t size;
      serialization::deserialize(size,iter);

      obj.resize(size);
      for(T1& v : obj)
      {
        serialization::deserialize(v,iter);
      }
    }

    template <typename T2>
    inline void deserialize(std::vector<bool,T2>& obj,std::vector<char>::const_iterator& iter)
    {
      size_t size;
      serialization::deserialize(size,iter);

      obj.resize(size);
      for(int i=0;i<obj.size();i++)
      {
        bool val;
        serialization::deserialize(val,iter);
        obj[i] = val;
      }
    }

    //std::set

    template <typename T>
    inline size_t getByteSize(const std::set<T>& obj)
    {
      return std::accumulate(obj.begin(),obj.end(),getByteSize(obj.size()),[](const size_t& acc,const T& cur) { return acc+getByteSize(cur); });
    }

    template <typename T>
    inline void serialize(const std::set<T>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.size(),buffer,iter);
      for(const T& cur : obj)
      {
        serialization::serialize(cur,buffer,iter);
      }
    }

    template <typename T>
    inline void deserialize(std::set<T>& obj,std::vector<char>::const_iterator& iter)
    {
      size_t size;
      serialization::deserialize(size,iter);

      obj.clear();
      for(size_t i=0; i<size; ++i)
      {
        T val;
        serialization::deserialize(val,iter);
        obj.insert(val);
      }
    }

    // std::map

    template <typename T1,typename T2>
    inline size_t getByteSize(const std::map<T1,T2>& obj)
    {
      return std::accumulate(obj.begin(),obj.end(),sizeof(size_t),[](const size_t& acc,const std::pair<T1,T2>& cur) { return acc+getByteSize(cur); });
    }

    template <typename T1,typename T2>
    inline void serialize(const std::map<T1,T2>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.size(),buffer,iter);
      for(const auto& cur : obj)
      {
        serialization::serialize(cur,buffer,iter);
      }
    }

    template <typename T1,typename T2>
    inline void deserialize(std::map<T1,T2>& obj,std::vector<char>::const_iterator& iter)
    {
      size_t size;
      serialization::deserialize(size,iter);

      obj.clear();
      for(size_t i=0; i<size; ++i)
      {
        std::pair<T1,T2> pair;
        serialization::deserialize(pair,iter);
        obj.insert(pair);
      }
    }

    //std::list

    template <typename T>
    inline size_t getByteSize(const std::list<T>& obj)
    {
        return std::accumulate(obj.begin(), obj.end(), getByteSize(obj.size()), [](const size_t& acc, const T& cur) { return acc + getByteSize(cur); });
    }

    template <typename T>
    inline void serialize(const std::list<T>& obj, std::vector<char>& buffer, std::vector<char>::iterator& iter)
    {
        serialization::serialize(obj.size(), buffer, iter);
        for (const T& cur : obj)
        {
            serialization::serialize(cur, buffer, iter);
        }
    }

    template <typename T>
    inline void deserialize(std::list<T>& obj, std::vector<char>::const_iterator& iter)
    {
        size_t size;
        serialization::deserialize(size, iter);

        obj.clear();
        for (size_t i = 0; i < size; ++i)
        {
            T val;
            serialization::deserialize(val, iter);
            obj.emplace_back(val);
        }
    }


    // Eigen types
    template<typename T,int R,int C,int P,int MR,int MC>
    inline size_t getByteSize(const Eigen::Matrix<T,R,C,P,MR,MC>& obj)
    {
      // space for numbers of rows,cols and data
      return 2*sizeof(typename Eigen::Matrix<T,R,C,P,MR,MC>::Index)+sizeof(T)*obj.rows()*obj.cols();
    }

    template<typename T,int R,int C,int P,int MR,int MC>
    inline void serialize(const Eigen::Matrix<T,R,C,P,MR,MC>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.rows(),buffer,iter);
      serialization::serialize(obj.cols(),buffer,iter);
      size_t size = sizeof(T)*obj.rows()*obj.cols();
      auto ptr = reinterpret_cast<const std::uint8_t*>(obj.data());
      iter = std::copy(ptr,ptr+size,iter);
    }

    template<typename T,int R,int C,int P,int MR,int MC>
    inline void deserialize(Eigen::Matrix<T,R,C,P,MR,MC>& obj,std::vector<char>::const_iterator& iter)
    {
      typename Eigen::Matrix<T,R,C,P,MR,MC>::Index rows,cols;
      serialization::deserialize(rows,iter);
      serialization::deserialize(cols,iter);
      size_t size = sizeof(T)*rows*cols;
      obj.resize(rows,cols);
      auto ptr = reinterpret_cast<std::uint8_t*>(obj.data());
      std::copy(iter,iter+size,ptr);
      iter+=size;
    }

    template<typename T,int R,int C,int P,int MR,int MC>
    inline size_t getByteSize(const Eigen::Array<T,R,C,P,MR,MC>& obj)
    {
      // space for numbers of rows,cols and data
      return 2*sizeof(typename Eigen::Array<T,R,C,P,MR,MC>::Index)+sizeof(T)*obj.rows()*obj.cols();
    }

    template<typename T,int R,int C,int P,int MR,int MC>
    inline void serialize(const Eigen::Array<T,R,C,P,MR,MC>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.rows(),buffer,iter);
      serialization::serialize(obj.cols(),buffer,iter);
      size_t size = sizeof(T)*obj.rows()*obj.cols();
      auto ptr = reinterpret_cast<const std::uint8_t*>(obj.data());
      iter = std::copy(ptr,ptr+size,iter);
    }

    template<typename T,int R,int C,int P,int MR,int MC>
    inline void deserialize(Eigen::Array<T,R,C,P,MR,MC>& obj,std::vector<char>::const_iterator& iter)
    {
      typename Eigen::Array<T,R,C,P,MR,MC>::Index rows,cols;
      serialization::deserialize(rows,iter);
      serialization::deserialize(cols,iter);
      size_t size = sizeof(T)*rows*cols;
      obj.resize(rows,cols);
      auto ptr = reinterpret_cast<std::uint8_t*>(obj.data());
      std::copy(iter,iter+size,ptr);
      iter+=size;
    }

    template<typename T,int P,typename I>
    inline size_t getByteSize(const Eigen::SparseMatrix<T,P,I>& obj)
    {
      // space for numbers of rows,cols,nonZeros and tripplets with data (rowIdx,colIdx,value)
      size_t size = sizeof(typename Eigen::SparseMatrix<T,P,I>::Index);
      return 3*size+(sizeof(T)+2*size)*obj.nonZeros();
    }

    template<typename T,int P,typename I>
    inline void serialize(const Eigen::SparseMatrix<T,P,I>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.rows(),buffer,iter);
      serialization::serialize(obj.cols(),buffer,iter);
      serialization::serialize(obj.nonZeros(),buffer,iter);

      for(int k=0;k<obj.outerSize();++k)
      {
        for(typename Eigen::SparseMatrix<T,P,I>::InnerIterator it(obj,k);it;++it)
        {
          serialization::serialize(it.row(),buffer,iter);
          serialization::serialize(it.col(),buffer,iter);
          serialization::serialize(it.value(),buffer,iter);
        }
      }
    }

    template<typename T,int P,typename I>
    inline void deserialize(Eigen::SparseMatrix<T,P,I>& obj,std::vector<char>::const_iterator& iter)
    {
      typename Eigen::SparseMatrix<T,P,I>::Index rows,cols,nonZeros;
      serialization::deserialize(rows,iter);
      serialization::deserialize(cols,iter);
      serialization::deserialize(nonZeros,iter);

      obj.resize(rows,cols);
      obj.setZero();

      std::vector<Eigen::Triplet<T,I> > triplets;
      for(int i=0;i<nonZeros;i++)
      {
        typename Eigen::SparseMatrix<T,P,I>::Index rowId,colId;
        serialization::deserialize(rowId,iter);
        serialization::deserialize(colId,iter);
        T value;
        serialization::deserialize(value,iter);
        triplets.push_back(Eigen::Triplet<T,I>(rowId,colId,value));
      }
      obj.setFromTriplets(triplets.begin(),triplets.end());
    }

    template<typename T,int P>
    inline size_t getByteSize(const Eigen::Quaternion<T,P>& /*obj*/)
    {
      return sizeof(T)*4;
    }

    template<typename T,int P>
    inline void serialize(const Eigen::Quaternion<T,P>& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj.w(),buffer,iter);
      serialization::serialize(obj.x(),buffer,iter);
      serialization::serialize(obj.y(),buffer,iter);
      serialization::serialize(obj.z(),buffer,iter);
    }

    template<typename T,int P>
    inline void deserialize(Eigen::Quaternion<T,P>& obj,std::vector<char>::const_iterator& iter)
    {
      serialization::deserialize(obj.w(),iter);
      serialization::deserialize(obj.x(),iter);
      serialization::deserialize(obj.y(),iter);
      serialization::deserialize(obj.z(),iter);
    }

    // pointers

    template <typename T>
    inline typename std::enable_if<std::is_pointer<T>::value,size_t>::type getByteSize(const T& obj)
    {
      size_t size = sizeof(bool);

      if(obj)
        size += getByteSize(*obj);

      return size;
    }

    template <typename T>
    inline typename std::enable_if<std::is_pointer<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialization::serialize(obj == nullptr,buffer,iter);

      if(obj)
        serialization::serialize(*obj,buffer,iter);
    }

    template <typename T>
    inline typename std::enable_if<std::is_pointer<T>::value>::type deserialize(T& obj,std::vector<char>::const_iterator& iter)
    {
      bool isNullPtr;
      serialization::deserialize(isNullPtr,iter);

      if(isNullPtr)
      {
        if(obj)
        {
          std::cout << "serialization: possible memory leak in serialization for '" << typeid(obj).name() << "'" << std::endl;
          obj = nullptr;
        }
      }
      else
      {
        if(obj)
        {
          std::cout << "serialization: possible memory corruption in deserialization for '" << typeid(obj).name() << "'" << std::endl;
        }
        else
        {
          obj = new typename std::remove_pointer<T>::type();
        }
        serialization::deserialize(*obj,iter);
      }
    }

    // std::shared_ptr and std::unique_ptr

    template <typename T>
    inline typename std::enable_if<serialization::is_smart_ptr<T>::value,size_t>::type getByteSize(const T& obj)
    {
      return getByteSize(obj.get());
    }

    template <typename T>
    inline typename std::enable_if<serialization::is_smart_ptr<T>::value>::type serialize(const T& obj,std::vector<char>& buffer,std::vector<char>::iterator& iter)
    {
      serialize(obj.get(),buffer,iter);
    }

    template <template<typename> class T0,typename T1>
    inline typename std::enable_if<serialization::is_smart_ptr<T0<T1> >::value>::type deserialize(T0<T1>& obj,std::vector<char>::const_iterator& iter)
    {
      bool isNullPtr;
      serialization::deserialize(isNullPtr,iter);

      if(isNullPtr)
      {
        obj.reset();
      }
      else
      {
        obj = T0<T1>(new T1());
        serialization::deserialize(*obj,iter);
      }
    }

    // std::weak_ptr

    template <typename T>
    inline size_t getByteSize(const std::weak_ptr<T>& /*obj*/)
    {
      return sizeof(size_t);
    }

    template <typename T>
    inline void serialize(const std::weak_ptr<T>& /*obj*/,std::vector<char>& /*buffer*/,std::vector<char>::iterator& /*iter*/)
    {

    }

    template <typename T>
    inline void deserialize(std::weak_ptr<T>& /*obj*/,std::vector<char>::const_iterator& /*iter*/)
    {

    }

    // functions to overload for non-intrusive serialization
    template <typename T>
    inline void serialize(const T& obj,std::vector<char>& /*buffer*/)
    {
      std::cerr << typeid(obj).name() << " is not serializable: derive from igl::Serializable or specialize the template function igl::serialization::serialize(const T& obj,std::vector<char>& buffer)" << std::endl;
    }

    template <typename T>
    inline void deserialize(T& obj,const std::vector<char>& /*buffer*/)
    {
      std::cerr << typeid(obj).name() << " is not deserializable: derive from igl::Serializable or specialize the template function igl::serialization::deserialize(T& obj, const std::vector<char>& buffer)" << std::endl;
    }

    // helper functions

    template <typename T>
    inline void updateMemoryMap(T& obj,size_t size,std::map<std::uintptr_t,IndexedPointerBase*>& memoryMap)
    {
      // check if object is already serialized
      auto startPtr = new IndexedPointer<T>();
      startPtr->Object = &obj;
      auto startBasePtr = static_cast<IndexedPointerBase*>(startPtr);
      startBasePtr->Type = IndexedPointerBase::BEGIN;
      auto startAddress = reinterpret_cast<std::uintptr_t>(&obj);
      auto p = std::pair<std::uintptr_t,IndexedPointerBase*>(startAddress,startBasePtr);

      auto el = memoryMap.insert(p);
      auto iter = ++el.first; // next elememt
      if(el.second && (iter == memoryMap.end() || iter->second->Type != IndexedPointerBase::END))
      {
        // not yet serialized
        auto endPtr = new IndexedPointer<T>();
        auto endBasePtr = static_cast<IndexedPointerBase*>(endPtr);
        endBasePtr->Type = IndexedPointerBase::END;
        auto endAddress = reinterpret_cast<std::uintptr_t>(&obj) + size - 1;
        auto p = std::pair<std::uintptr_t,IndexedPointerBase*>(endAddress,endBasePtr);

        // insert end address
        memoryMap.insert(el.first,p);
      }
      else
      {
        // already serialized

        // remove inserted address
        memoryMap.erase(el.first);
      }
    }
  }
}

#endif
