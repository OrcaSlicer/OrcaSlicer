#ifndef IGL_XML_XMLSERIALIZABLE_H
#define IGL_XML_XMLSERIALIZABLE_H

#include "serialize_xml.h"
#include "../igl_inline.h"
#include "../serialize.h"

#include <tinyxml2.h>


// Interface for xml-serializable class see serialize_xml.h

// Pretty sure all of these IGL_INLINE should be inline

namespace igl
{
  namespace xml
  {
    // interface for user defined types
    struct XMLSerializableBase : public SerializableBase
    {
      virtual void Serialize(std::vector<char>& buffer) const = 0;
      virtual void Deserialize(const std::vector<char>& buffer) = 0;
      virtual void Serialize(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element) const = 0;
      virtual void Deserialize(const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element) = 0;
    };

    // Convenient interface for user defined types
    class XMLSerializable: public XMLSerializableBase
    {
    private:

      template <typename T>
      struct XMLSerializationObject: public XMLSerializableBase
      {
        bool Binary;
        std::string Name;
        T* Object;

        void Serialize(std::vector<char>& buffer) const {
          serialize(*Object,Name,buffer);
        }

        void Deserialize(const std::vector<char>& buffer) {
          deserialize(*Object,Name,buffer);
        }

        void Serialize(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element) const {
          serialize_xml(*Object,Name,doc,element,Binary);
        }

        void Deserialize(const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element) {
          deserialize_xml(*Object,Name,doc,element);
        }
      };

      mutable bool initialized;
      mutable std::vector<XMLSerializableBase*> objects;

    public:

      // Override this function to add your member variables which should be serialized
      IGL_INLINE virtual void InitSerialization() = 0;

      // Following functions can be overridden to handle the specific events.
      // Return false to prevent the de-/serialization of an object.
      IGL_INLINE virtual bool PreSerialization() const;
      IGL_INLINE virtual void PostSerialization() const;
      IGL_INLINE virtual bool PreDeserialization();
      IGL_INLINE virtual void PostDeserialization();

      // Default implementation of XMLSerializableBase interface
      IGL_INLINE void Serialize(std::vector<char>& buffer) const;
      IGL_INLINE void Deserialize(const std::vector<char>& buffer);
      IGL_INLINE void Serialize(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element) const;
      IGL_INLINE void Deserialize(const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element);

      // Default constructor, destructor, assignment and copy constructor
      IGL_INLINE XMLSerializable();
      IGL_INLINE XMLSerializable(const XMLSerializable& obj);
      IGL_INLINE ~XMLSerializable();
      IGL_INLINE XMLSerializable& operator=(const XMLSerializable& obj);

      // Use this function to add your variables which should be serialized
      template <typename T>
      IGL_INLINE void Add(T& obj,std::string name,bool binary = false);
    };

    // IMPLEMENTATION

    IGL_INLINE bool XMLSerializable::PreSerialization() const
    {
      return true;
    }

    IGL_INLINE void XMLSerializable::PostSerialization() const
    {
    }

    IGL_INLINE bool XMLSerializable::PreDeserialization()
    {
      return true;
    }

    IGL_INLINE void XMLSerializable::PostDeserialization()
    {
    }

    IGL_INLINE void XMLSerializable::Serialize(std::vector<char>& buffer) const
    {
      if(this->PreSerialization())
      {
        if(initialized == false)
        {
          objects.clear();
          (const_cast<XMLSerializable*>(this))->InitSerialization();
          initialized = true;
        }

        for(unsigned int i=0;i<objects.size();i++)
          objects[i]->Serialize(buffer);

        this->PostSerialization();
      }
    }

    IGL_INLINE void XMLSerializable::Deserialize(const std::vector<char>& buffer)
    {
      if(this->PreDeserialization())
      {
        if(initialized == false)
        {
          objects.clear();
          (const_cast<XMLSerializable*>(this))->InitSerialization();
          initialized = true;
        }

        for(unsigned int i=0;i<objects.size();i++)
          objects[i]->Deserialize(buffer);

        this->PostDeserialization();
      }
    }

    IGL_INLINE void XMLSerializable::Serialize(tinyxml2::XMLDocument* doc,tinyxml2::XMLElement* element) const
    {
      if(this->PreSerialization())
      {
        if(initialized == false)
        {
          objects.clear();
          (const_cast<XMLSerializable*>(this))->InitSerialization();
          initialized = true;
        }

        for(unsigned int i=0;i<objects.size();i++)
          objects[i]->Serialize(doc,element);

        this->PostSerialization();
      }
    }

    IGL_INLINE void XMLSerializable::Deserialize(const tinyxml2::XMLDocument* doc,const tinyxml2::XMLElement* element)
    {
      if(this->PreDeserialization())
      {
        if(initialized == false)
        {
          objects.clear();
          (const_cast<XMLSerializable*>(this))->InitSerialization();
          initialized = true;
        }

        for(unsigned int i=0;i<objects.size();i++)
          objects[i]->Deserialize(doc,element);

        this->PostDeserialization();
      }
    }

    IGL_INLINE XMLSerializable::XMLSerializable()
    {
      initialized = false;
    }

    IGL_INLINE XMLSerializable::XMLSerializable(const XMLSerializable& obj)
    {
      initialized = false;
      objects.clear();
    }

    IGL_INLINE XMLSerializable::~XMLSerializable()
    {
      initialized = false;
      objects.clear();
    }


    IGL_INLINE XMLSerializable& XMLSerializable::operator=(const XMLSerializable& obj)
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
    IGL_INLINE void XMLSerializable::Add(T& obj,std::string name,bool binary)
    {
      XMLSerializationObject<T>* object = new XMLSerializationObject<T>();
      object->Binary = binary;
      object->Name = name;
      object->Object = &obj;

      objects.push_back(object);
    }

  }
}
#endif
