// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_XML_REANTTWEAKBAR_XML_SERIALIZATION_H
#define IGL_XML_REANTTWEAKBAR_XML_SERIALIZATION_H
#include "../igl_inline.h"
#include "serialize_xml.h"

#undef IGL_HEADER_ONLY
#include "../anttweakbar/ReAntTweakBar.h"

// Forward declarations
namespace igl
{
  namespace anttweakbar
  {
    class ReTwBar;
  }
};
namespace tinyxml2
{
  class XMLDocument;
};

namespace igl
{
  namespace xml
  {
  
//  namespace
//  {
  
//    IGL_INLINE bool save_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, const char* file_name);
//    IGL_INLINE bool save_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, tinyxml2::XMLDocument* doc);
//    IGL_INLINE bool load_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, const char *file_name);
//    IGL_INLINE bool load_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, tinyxml2::XMLDocument* doc);
    
    
    IGL_INLINE bool save_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, const char* file_name)
    {
      const char * name_chars = TwGetBarName(bar->bar);
      std::string name = std::string(name_chars) + "_AntTweakBar";
      
      const std::vector< ::igl::anttweakbar::ReTwRWItem>& rw_items = bar->get_rw_items();
      for(std::vector< ::igl::anttweakbar::ReTwRWItem>::const_iterator it = rw_items.begin(); it != rw_items.end(); it++)
      {
        std::string val = bar->get_value_as_string(it->var,it->type);
        //::igl::XMLSerializer::SaveObject(val,it->name,name,file_name,false);
        ::igl::serialize_xml(val,it->name,file_name,false,false);
      }
      
      char var[REANTTWEAKBAR_MAX_CB_VAR_SIZE];
      // Print all CB variables
      const std::vector< ::igl::anttweakbar::ReTwCBItem>& cb_items = bar->get_cb_items();
      for(std::vector< ::igl::anttweakbar::ReTwCBItem>::const_iterator it = cb_items.begin(); it != cb_items.end(); it++)
      {
        TwType type = it->type;
        //TwSetVarCallback setCallback = it->setCallback;
        TwGetVarCallback getCallback = it->getCallback;
        void * clientData = it->clientData;
        // I'm not sure how to do what I want to do. getCallback needs to be sure
        // that it can write to var. So var needs to point to a valid and big
        // enough chunk of memory
        getCallback(var,clientData);
        
        std::string val = bar->get_value_as_string(var,type);
        //::igl::XMLSerializer::SaveObject(val,it->name,name,file_name,false);
        ::igl::serialize_xml(val,it->name,file_name,false,false);
      }
      
      return true;
    }
    
    /*IGL_INLINE bool save_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, tinyxml2::XMLDocument* doc)
    {
      std::vector<char**> buffer;
      
      const char * name_chars = TwGetBarName(bar->bar);
      std::string name = std::string(name_chars) + "_AntTweakBar";
      ::igl::XMLSerializer* s = new ::igl::XMLSerializer(name);
      
      const std::vector< ::igl::anttweakbar::ReTwRWItem>& rw_items = bar->get_rw_items();
      for(std::vector< ::igl::anttweakbar::ReTwRWItem>::const_iterator it = rw_items.begin(); it != rw_items.end(); it++)
      {
        std::string val = bar->get_value_as_string(it->var,it->type);
        char** cval = new char*; // create char* on heap
        *cval = new char[val.size()+1];
        buffer.push_back(cval);
        strcpy(*cval,val.c_str());
        s->Add(*cval,it->name);
      }
      
      char var[REANTTWEAKBAR_MAX_CB_VAR_SIZE];
      // Print all CB variables
      const std::vector< ::igl::anttweakbar::ReTwCBItem>& cb_items = bar->get_cb_items();
      for(std::vector< ::igl::anttweakbar::ReTwCBItem>::const_iterator it = cb_items.begin(); it != cb_items.end(); it++)
      {
        TwType type = it->type;
        //TwSetVarCallback setCallback = it->setCallback;
        TwGetVarCallback getCallback = it->getCallback;
        void * clientData = it->clientData;
        // I'm not sure how to do what I want to do. getCallback needs to be sure
        // that it can write to var. So var needs to point to a valid and big
        // enough chunk of memory
        getCallback(var,clientData);
        
        std::string val = bar->get_value_as_string(var,type);
        char** cval = new char*; // create char* on heap
        *cval = new char[val.size()+1];
        buffer.push_back(cval);
        strcpy(*cval,val.c_str());
        s->Add(*cval,it->name);
      }
      
      s->SaveToXMLDoc(name,doc);
      
      // delete pointer buffers
      for(unsigned int i=0;i<buffer.size();i++)
      {
        delete[] *buffer[i];
        delete buffer[i];
      }
      
      delete s;
      
      return true;
    }*/
    
    IGL_INLINE bool load_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, const char *file_name)
    {
      char type_str[REANTTWEAKBAR_MAX_WORD];
      char value_str[REANTTWEAKBAR_MAX_WORD];
      TwType type;
      
      const char * name_chars = TwGetBarName(bar->bar);
      std::string name = std::string(name_chars) + "_AntTweakBar";
      
      const std::vector< ::igl::anttweakbar::ReTwRWItem>& rw_items = bar->get_rw_items();
      for(std::vector< ::igl::anttweakbar::ReTwRWItem>::const_iterator it = rw_items.begin(); it != rw_items.end(); it++)
      {
        char* val;
        //::igl::XMLSerializer::LoadObject(val,it->name,name,file_name);
        ::igl::deserialize_xml(val,it->name,file_name);
        sscanf(val,"%s %[^\n]",type_str,value_str);
        
        if(!bar->type_from_string(type_str,type))
        {
          printf("ERROR: %s type not found... Skipping...\n",type_str);
          continue;
        }
        
        bar->set_value_from_string(it->name.c_str(),type,value_str);
        delete[] val;
      }
      
      const std::vector< ::igl::anttweakbar::ReTwCBItem>& cb_items = bar->get_cb_items();
      for(std::vector< ::igl::anttweakbar::ReTwCBItem>::const_iterator it = cb_items.begin(); it != cb_items.end(); it++)
      {
        char* val;
        //::igl::XMLSerializer::LoadObject(val,it->name,name,file_name);
        ::igl::deserialize_xml(val,it->name,file_name);
        sscanf(val,"%s %[^\n]",type_str,value_str);
        
        if(!bar->type_from_string(type_str,type))
        {
          printf("ERROR: %s type not found... Skipping...\n",type_str);
          continue;
        }
        
        bar->set_value_from_string(it->name.c_str(),type,value_str);
        delete[] val;
      }
      
      return true;
    }
    
    /*IGL_INLINE bool load_ReAntTweakBar(::igl::anttweakbar::ReTwBar* bar, tinyxml2::XMLDocument* doc)
    {
      std::map<std::string,char*> variables;
      std::map<std::string,char*> cbVariables;
      
      const char * name_chars = TwGetBarName(bar->bar);
      std::string name = std::string(name_chars) + "_AntTweakBar";
      ::igl::XMLSerializer* s = new ::igl::XMLSerializer(name);
      
      std::map<std::string,char*>::iterator iter;
      const std::vector< ::igl::anttweakbar::ReTwRWItem>& rw_items = bar->get_rw_items();
      for(std::vector< ::igl::anttweakbar::ReTwRWItem>::const_iterator it = rw_items.begin(); it != rw_items.end(); it++)
      {
        variables[it->name] = NULL;
        iter = variables.find(it->name);
        s->Add(iter->second,iter->first);
      }
      
      // Add all CB variables
      const std::vector< ::igl::anttweakbar::ReTwCBItem>& cb_items = bar->get_cb_items();
      for(std::vector< ::igl::anttweakbar::ReTwCBItem>::const_iterator it = cb_items.begin(); it != cb_items.end(); it++)
      {
        cbVariables[it->name] = NULL;
        iter = cbVariables.find(it->name);
        s->Add(iter->second,iter->first);
      }
      
      s->LoadFromXMLDoc(doc);
      
      // Set loaded values
      char type_str[REANTTWEAKBAR_MAX_WORD];
      char value_str[REANTTWEAKBAR_MAX_WORD];
      TwType type;
      
      for(iter = variables.begin(); iter != variables.end(); iter++)
      {
        if(iter->second == NULL)
        {
          printf("ERROR: '%s' entry not found... Skipping...\n",iter->first.c_str());
          continue;
        }
        
        sscanf(iter->second,"%s %[^\n]",type_str,value_str);
        
        if(!bar->type_from_string(type_str,type))
        {
          printf("ERROR: Type '%s' of '%s' not found... Skipping...\n",type_str,iter->first.c_str());
          continue;
        }
        
        bar->set_value_from_string(iter->first.c_str(),type,value_str);
      }
      
      for(iter = cbVariables.begin(); iter != cbVariables.end(); iter++)
      {
        if(iter->second == NULL)
        {
          printf("ERROR: '%s' entry not found... Skipping...\n",iter->first.c_str());
          continue;
        }

        sscanf(iter->second,"%s %[^\n]",type_str,value_str);
        
        if(!bar->type_from_string(type_str,type))
        {
          printf("ERROR: Type '%s' of '%s' not found... Skipping...\n",type_str,iter->first.c_str());
          continue;
        }
        
        bar->set_value_from_string(iter->first.c_str(),type,value_str);
      }
      
      // delete buffers
      for(iter = variables.begin(); iter != variables.end(); iter++)
        delete[] iter->second;
      
      for(iter = cbVariables.begin(); iter != cbVariables.end(); iter++)
        delete[] iter->second;
      
      delete s;
      
      return true;
    }*/
    
//  }
  }
}

#endif
