// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "ReAntTweakBar.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>

// GLOBAL WRAPPERS
namespace 
{
  std::map<
    TwType,std::pair<const char *,std::vector<TwEnumVal> > 
    > ReTw_custom_types;
}

IGL_INLINE TwType igl::anttweakbar::ReTwDefineEnum(
  const char *name, 
  const TwEnumVal *enumValues, 
  unsigned int nbValues)
{
  using namespace std;
  // copy enum valus into vector
  std::vector<TwEnumVal> enum_vals;
  enum_vals.resize(nbValues);
  for(unsigned int j = 0; j<nbValues;j++)
  {
    enum_vals[j] = enumValues[j];
  }
  TwType type = TwDefineEnum(name,enumValues,nbValues);

  ReTw_custom_types[type] = 
    std::pair<const char *,std::vector<TwEnumVal> >(name,enum_vals);

  return type;
}

IGL_INLINE TwType igl::anttweakbar::ReTwDefineEnumFromString(
  const char * _Name,
  const char * _EnumString)
{
  // Taken directly from TwMgr.cpp, just replace TwDefineEnum with
  // ReTwDefineEnum
  using namespace std;
  {
    if (_EnumString == NULL) 
        return ReTwDefineEnum(_Name, NULL, 0);

    // split enumString
    stringstream EnumStream(_EnumString);
    string Label;
    vector<string> Labels;
    while( getline(EnumStream, Label, ',') ) {
        // trim Label
        size_t Start = Label.find_first_not_of(" \n\r\t");
        size_t End = Label.find_last_not_of(" \n\r\t");
        if( Start==string::npos || End==string::npos )
            Label = "";
        else
            Label = Label.substr(Start, (End-Start)+1);
        // store Label
        Labels.push_back(Label);
    }
    // create TwEnumVal array
    vector<TwEnumVal> Vals(Labels.size());
    for( int i=0; i<(int)Labels.size(); i++ )
    {
        Vals[i].Value = i;
        // Wrong:
        //Vals[i].Label = Labels[i].c_str();
        // Allocate char on heap
        // http://stackoverflow.com/a/10050258/148668
        char * c_label = new char[Labels[i].length()+1];
        std::strcpy(c_label, Labels[i].c_str());
        Vals[i].Label = c_label;
    }

    const TwType type = 
      ReTwDefineEnum(_Name, Vals.empty() ? 
        NULL : 
        &(Vals[0]), (unsigned int)Vals.size());
    return type;
  }
}

namespace
{
  struct ReTwTypeString
  {
    TwType type;
    const char * type_str;
  };

  #define RETW_NUM_DEFAULT_TYPE_STRINGS 23
  ReTwTypeString ReTwDefaultTypeStrings[RETW_NUM_DEFAULT_TYPE_STRINGS] = 
  {
    {TW_TYPE_UNDEF,"TW_TYPE_UNDEF"},
    {TW_TYPE_BOOLCPP,"TW_TYPE_BOOLCPP"},
    {TW_TYPE_BOOL8,"TW_TYPE_BOOL8"},
    {TW_TYPE_BOOL16,"TW_TYPE_BOOL16"},
    {TW_TYPE_BOOL32,"TW_TYPE_BOOL32"},
    {TW_TYPE_CHAR,"TW_TYPE_CHAR"},
    {TW_TYPE_INT8,"TW_TYPE_INT8"},
    {TW_TYPE_UINT8,"TW_TYPE_UINT8"},
    {TW_TYPE_INT16,"TW_TYPE_INT16"},
    {TW_TYPE_UINT16,"TW_TYPE_UINT16"},
    {TW_TYPE_INT32,"TW_TYPE_INT32"},
    {TW_TYPE_UINT32,"TW_TYPE_UINT32"},
    {TW_TYPE_FLOAT,"TW_TYPE_FLOAT"},
    {TW_TYPE_DOUBLE,"TW_TYPE_DOUBLE"},
    {TW_TYPE_COLOR32,"TW_TYPE_COLOR32"},
    {TW_TYPE_COLOR3F,"TW_TYPE_COLOR3F"},
    {TW_TYPE_COLOR4F,"TW_TYPE_COLOR4F"},
    {TW_TYPE_CDSTRING,"TW_TYPE_CDSTRING"},
    {TW_TYPE_STDSTRING,"TW_TYPE_STDSTRING"},
    {TW_TYPE_QUAT4F,"TW_TYPE_QUAT4F"},
    {TW_TYPE_QUAT4D,"TW_TYPE_QUAT4D"},
    {TW_TYPE_DIR3F,"TW_TYPE_DIR3F"},
    {TW_TYPE_DIR3D,"TW_TYPE_DIR3D"}
  };
}

IGL_INLINE igl::anttweakbar::ReTwBar::ReTwBar():
 bar(NULL),
  name(),
  rw_items(),cb_items()
{
}

IGL_INLINE igl::anttweakbar::ReTwBar::ReTwBar(
    const igl::anttweakbar::ReTwBar & that):
  bar(that.bar),
  name(that.name),
  rw_items(that.rw_items),
  cb_items(that.cb_items)
{
}

IGL_INLINE igl::anttweakbar::ReTwBar & 
igl::anttweakbar::ReTwBar::operator=(const igl::anttweakbar::ReTwBar & that)
{
  // check for self assignment
  if(this != &that)
  {
    bar = that.bar;
    rw_items = that.rw_items;
    cb_items = that.cb_items;
  }
  return *this;
}


// BAR WRAPPERS
IGL_INLINE void igl::anttweakbar::ReTwBar::TwNewBar(const char * _name)
{
  this->bar = ::TwNewBar(_name);
  // Alec: This causes trouble (not sure why) in multiple applications
  // (medit, puppet) Probably there is some sort of memory corrpution.
  // this->name = _name;
  // Suspiciously this also fails:
  //this->name = "foobar";
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwAddVarRW(
  const char *name, 
  TwType type, 
  void *var, 
  const char *def,
  const bool record)
{
  int ret = ::TwAddVarRW(this->bar,name,type,var,def);
  if(ret && record)
  {
    rw_items.push_back(ReTwRWItem(name,type,var));
  }
  return ret;
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwAddVarCB(
  const char *name, 
  TwType type, 
  TwSetVarCallback setCallback, 
  TwGetVarCallback getCallback, 
  void *clientData, 
  const char *def,
  const bool record)
{
  int ret = 
    ::TwAddVarCB(this->bar,name,type,setCallback,getCallback,clientData,def);
  if(ret && record)
  {
    cb_items.push_back(ReTwCBItem(name,type,setCallback,getCallback,clientData));
  }
  return ret;
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwAddVarRO(
  const char *name, 
  TwType type, 
  void *var, 
  const char *def)
{
  int ret = ::TwAddVarRO(this->bar,name,type,var,def);
  // Read only variables are not recorded
  //if(ret)
  //{
  //  rw_items.push_back(ReTwRWItem(name,type,var));
  //}
  return ret;
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwAddButton(
  const char *name, 
  TwButtonCallback buttonCallback, 
  void *clientData, 
  const char *def)
{
  int ret = 
    ::TwAddButton(this->bar,name,buttonCallback,clientData,def);
  // buttons are not recorded
  //if(ret)
  //{
  //  cb_items.push_back(ReTwCBItem(name,type,setCallback,getCallback,clientData));
  //}
  return ret;
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwSetParam(
  const char *varName, 
  const char *paramName, 
  TwParamValueType paramValueType, 
  unsigned int inValueCount, 
  const void *inValues)
{
  // For now just pass these along
  return 
    ::TwSetParam(
      this->bar,
      varName,
      paramName,
      paramValueType,
      inValueCount,
      inValues);
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwGetParam(
  const char *varName, 
  const char *paramName, 
  TwParamValueType paramValueType, 
  unsigned int outValueMaxCount,
  void *outValues)
{
  return 
    ::TwGetParam(
      this->bar,
      varName,
      paramName,
      paramValueType,
      outValueMaxCount,
      outValues);
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwRefreshBar()
{
  return ::TwRefreshBar(this->bar);
}

IGL_INLINE int igl::anttweakbar::ReTwBar::TwTerminate()
{
  //std::cout<<"TwTerminate"<<std::endl;
  int r = ::TwTerminate();
  //std::cout<<"  "<<r<<std::endl;
  return r;
}

IGL_INLINE bool igl::anttweakbar::ReTwBar::save(const char *file_name)
{
  FILE * fp;
  if(file_name == NULL)
  {
    fp = stdout;
  }else
  {
    fp = fopen(file_name,"w");
  }

  if(fp == NULL)
  {
    printf("ERROR: not able to open %s for writing...\n",file_name);
    return false;
  }

  // Print all RW variables
  for(
    std::vector<ReTwRWItem>::iterator it = rw_items.begin(); 
    it != rw_items.end(); 
    it++)
  {
    std::string s = (*it).name;
    const char * name = s.c_str();
    TwType type = (*it).type;
    void * var = (*it).var;
    fprintf(fp,"%s: %s\n",
      name,
      get_value_as_string(var,type).c_str());
  }

  char var[REANTTWEAKBAR_MAX_CB_VAR_SIZE];
  // Print all CB variables
  for(
    std::vector<ReTwCBItem>::iterator it = cb_items.begin(); 
    it != cb_items.end(); 
    it++)
  {
    const char * name = it->name.c_str();
    TwType type = it->type;
    //TwSetVarCallback setCallback = it->setCallback;
    TwGetVarCallback getCallback = it->getCallback;
    void * clientData = it->clientData;
    // I'm not sure how to do what I want to do. getCallback needs to be sure
    // that it can write to var. So var needs to point to a valid and big
    // enough chunk of memory
    getCallback(var,clientData);
    fprintf(fp,"%s: %s\n",
      name,
      get_value_as_string(var,type).c_str());
  }

  fprintf(fp,"\n");

  if(file_name != NULL)
  {
    fclose(fp);
  }
  // everything succeeded
  return true;
}

IGL_INLINE std::string igl::anttweakbar::ReTwBar::get_value_as_string(
  void * var, 
  TwType type)
{
  std::stringstream sstr;
  switch(type)
  {
    case TW_TYPE_BOOLCPP:
      {
        sstr << "TW_TYPE_BOOLCPP" << " ";
        sstr << *(static_cast<bool*>(var));
        break;
      }
    case TW_TYPE_QUAT4D:
      {
        sstr << "TW_TYPE_QUAT4D" << " ";
        // Q: Why does casting to double* work? shouldn't I have to cast to
        // double**?
        double * q = static_cast<double*>(var);
        sstr << std::setprecision(15) << q[0] << " " << q[1] << " " << q[2] << " " << q[3];
        break;
      }
    case TW_TYPE_QUAT4F:
      {
        sstr << "TW_TYPE_QUAT4F" << " ";
        // Q: Why does casting to float* work? shouldn't I have to cast to
        // float**?
        float * q = static_cast<float*>(var);
        sstr << q[0] << " " << q[1] << " " << q[2] << " " << q[3];
        break;
      }
    case TW_TYPE_COLOR4F:
      {
        sstr << "TW_TYPE_COLOR4F" << " ";
        float * c = static_cast<float*>(var);
        sstr << c[0] << " " << c[1] << " " << c[2] << " " << c[3];
        break;
      }
    case TW_TYPE_COLOR3F:
      {
        sstr << "TW_TYPE_COLOR3F" << " ";
        float * c = static_cast<float*>(var);
        sstr << c[0] << " " << c[1] << " " << c[2];
        break;
      }
    case TW_TYPE_DIR3D:
      {
        sstr << "TW_TYPE_DIR3D" << " ";
        double * d = static_cast<double*>(var);
        sstr << std::setprecision(15) << d[0] << " " << d[1] << " " << d[2];
        break;
      }
    case TW_TYPE_DIR3F:
      {
        sstr << "TW_TYPE_DIR3F" << " ";
        float * d = static_cast<float*>(var);
        sstr << d[0] << " " << d[1] << " " << d[2];
        break;
      }
    case TW_TYPE_BOOL32:
      {
        sstr << "TW_TYPE_BOOL32" << " ";
        sstr << *(static_cast<int*>(var));
        break;
      }
    case TW_TYPE_UINT8:
      {
        sstr << "TW_TYPE_UINT8" << " ";
        // Cast to int so that it's human readable
        sstr << (int)*(static_cast<unsigned char*>(var));
        break;
      }
    case TW_TYPE_INT32:
      {
        sstr << "TW_TYPE_INT32" << " ";
        sstr << *(static_cast<int*>(var));
        break;
      }
    case TW_TYPE_UINT32:
      {
        sstr << "TW_TYPE_UINT32" << " ";
        sstr << *(static_cast<unsigned int*>(var));
        break;
      }
    case TW_TYPE_FLOAT:
      {
        sstr << "TW_TYPE_FLOAT" << " ";
        sstr << *(static_cast<float*>(var));
        break;
      }
    case TW_TYPE_DOUBLE:
      {
        sstr << "TW_TYPE_DOUBLE" << " ";
        sstr << std::setprecision(15) << *(static_cast<double*>(var));
        break;
      }
    case TW_TYPE_STDSTRING:
      {
        sstr << "TW_TYPE_STDSTRING" << " ";
        std::string *destPtr = static_cast<std::string *>(var);
        sstr << destPtr->c_str();
        break;
      }
    default:
      {
        using namespace std;
        std::map<TwType,std::pair<const char *,std::vector<TwEnumVal> > >::const_iterator iter = 
          ReTw_custom_types.find(type);
        if(iter != ReTw_custom_types.end())
        {
          sstr << (*iter).second.first << " ";
          int enum_val = *(static_cast<int*>(var));
          // try find display name for enum value
          std::vector<TwEnumVal>::const_iterator eit = (*iter).second.second.begin();
          bool found = false;
          for(;eit<(*iter).second.second.end();eit++)
          {
            if(enum_val == eit->Value)
            {
              sstr << eit->Label;
              found = true;
              break;
            }
          }
          if(!found)
          {
            sstr << "ERROR_ENUM_VALUE_NOT_DEFINED";
          }
        }else
        {
          sstr << "ERROR_TYPE_NOT_SUPPORTED";
        }
        break;
      }
  }
  return sstr.str();
}

IGL_INLINE bool igl::anttweakbar::ReTwBar::load(const char *file_name)
{
  FILE * fp;
  fp = fopen(file_name,"r");

  if(fp == NULL)
  {
    printf("ERROR: not able to open %s for reading...\n",file_name);
    return false;
  }

  // go through file line by line
  char line[REANTTWEAKBAR_MAX_LINE];
  bool still_comments;
  char name[REANTTWEAKBAR_MAX_WORD];
  char type_str[REANTTWEAKBAR_MAX_WORD];
  char value_str[REANTTWEAKBAR_MAX_WORD];


  // line number
  int j = 0;
  bool finished = false;
  while(true)
  {
    // Eat comments
    still_comments = true;
    while(still_comments)
    {
      if(fgets(line,REANTTWEAKBAR_MAX_LINE,fp) == NULL)
      {
        finished = true;
        break;
      }
      // Blank lines and lines that begin with # are comments
      still_comments = (line[0] == '#' || line[0] == '\n');
      j++;
    }
    if(finished)
    {
      break;
    }

    sscanf(line,"%[^:]: %s %[^\n]",name,type_str,value_str);
    //printf("%s: %s %s\n",name, type_str,value_str);

    TwType type;
    if(!type_from_string(type_str,type))
    {
      printf("ERROR: %s type not found... Skipping...\n",type_str);
      continue;
    }
    set_value_from_string(name,type,value_str);

  }

  fclose(fp);
  
  // everything succeeded
  return true;
}

IGL_INLINE bool igl::anttweakbar::ReTwBar::type_from_string(
  const char *type_str, TwType & type)
{
  // first check default types
  for(int j = 0; j < RETW_NUM_DEFAULT_TYPE_STRINGS; j++)
  {
    if(strcmp(type_str,ReTwDefaultTypeStrings[j].type_str) == 0)
    {
      type = ReTwDefaultTypeStrings[j].type;
      return true;
      break;
    }
  }

  // then check custom types
  std::map<
    TwType,std::pair<const char *,std::vector<TwEnumVal> > 
    >::const_iterator iter = 
    ReTw_custom_types.begin();
  for(;iter != ReTw_custom_types.end(); iter++)
  {
    if(strcmp((*iter).second.first,type_str)==0)
    {
      type = (*iter).first;
      return true;
    }
  }
  return false;
}

bool igl::anttweakbar::ReTwBar::set_value_from_string(
  const char * name, 
  TwType type, 
  const char * value_str)
{
  void * value = NULL;
  // possible value slots
  int i;
  float v;
  double dv;
  float f[4];
  double d[4];
  bool b;
  unsigned int u;
  unsigned char uc;
  std::string s;

  // First try to get value from default types
  switch(type)
  {
    case TW_TYPE_BOOLCPP:
      {
        int ib;
        if(sscanf(value_str," %d",&ib) == 1)
        {
          b = ib!=0;
          value = &b;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_QUAT4D:
    //case TW_TYPE_COLOR4D:
      {
        if(sscanf(value_str," %lf %lf %lf %lf",&d[0],&d[1],&d[2],&d[3]) == 4)
        {
          value = &d;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_QUAT4F:
    case TW_TYPE_COLOR4F:
      {
        if(sscanf(value_str," %f %f %f %f",&f[0],&f[1],&f[2],&f[3]) == 4)
        {
          value = &f;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    //case TW_TYPE_COLOR3D:
    case TW_TYPE_DIR3D:
      {
        if(sscanf(value_str," %lf %lf %lf",&d[0],&d[1],&d[2]) == 3)
        {
          value = &d;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_COLOR3F:
    case TW_TYPE_DIR3F:
      {
        if(sscanf(value_str," %f %f %f",&f[0],&f[1],&f[2]) == 3)
        {
          value = &f;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_UINT8:
      {
        if(sscanf(value_str," %d",&i) == 1)
        {
          // Cast to unsigned char
          uc = (unsigned char) i;
          value = &uc;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_BOOL32:
    case TW_TYPE_INT32:
      {
        if(sscanf(value_str," %d",&i) == 1)
        {
          value = &i;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_UINT32:
      {
        if(sscanf(value_str," %u",&u) == 1)
        {
          value = &u;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_FLOAT:
      {
        if(sscanf(value_str," %f",&v) == 1)
        {
          value = &v;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_DOUBLE:
      {
        if(sscanf(value_str," %lf",&dv) == 1)
        {
          value = &dv;
        }else
        {
          printf("ERROR: Bad value format...\n");
          return false;
        }
        break;
      }
    case TW_TYPE_STDSTRING:
      {
        s  = value_str;
        value = &s;
        break;
      }
    default:
      // Try to find type in custom enum types
      std::map<TwType,std::pair<const char *,std::vector<TwEnumVal> > >::const_iterator iter = 
        ReTw_custom_types.find(type);
      if(iter != ReTw_custom_types.end())
      {
        std::vector<TwEnumVal>::const_iterator eit = (*iter).second.second.begin();
        bool found = false;
        for(;eit<(*iter).second.second.end();eit++)
        {
          if(strcmp(value_str,eit->Label) == 0)
          {
            i = eit->Value;
            value = &i;
            found = true;
            break;
          }
        }
        if(!found)
        {
          printf("ERROR_ENUM_VALUE_NOT_DEFINED");
        }
      }else
      {
        printf("ERROR_TYPE_NOT_SUPPORTED\n");
      }

      break;
  }


  // Find variable based on name
  // First look in RW items
  bool item_found = false;
  for(
    std::vector<ReTwRWItem>::iterator it = rw_items.begin(); 
    it != rw_items.end(); 
    it++)
  {
    if(it->name == name)
    {
      void * var = it->var;
      switch(type)
      {
        case TW_TYPE_BOOLCPP:
          {
            bool * bvar = static_cast<bool*>(var);
            bool * bvalue = static_cast<bool*>(value);
            *bvar = *bvalue;
            break;
          }
        case TW_TYPE_QUAT4D:
        //case TW_TYPE_COLOR4D:
          {
            double * dvar = static_cast<double*>(var);
            double * dvalue = static_cast<double*>(value);
            dvar[0] = dvalue[0];
            dvar[1] = dvalue[1];
            dvar[2] = dvalue[2];
            dvar[3] = dvalue[3];
            break;
          }
        case TW_TYPE_QUAT4F:
        case TW_TYPE_COLOR4F:
          {
            float * fvar = static_cast<float*>(var);
            float * fvalue = static_cast<float*>(value);
            fvar[0] = fvalue[0];
            fvar[1] = fvalue[1];
            fvar[2] = fvalue[2];
            fvar[3] = fvalue[3];
            break;
          }
        //case TW_TYPE_COLOR3D:
        case TW_TYPE_DIR3D:
          {
            double * dvar = static_cast<double*>(var);
            double * dvalue = static_cast<double*>(value);
            dvar[0] = dvalue[0];
            dvar[1] = dvalue[1];
            dvar[2] = dvalue[2];
            break;
          }
        case TW_TYPE_COLOR3F:
        case TW_TYPE_DIR3F:
          {
            float * fvar = static_cast<float*>(var);
            float * fvalue = static_cast<float*>(value);
            fvar[0] = fvalue[0];
            fvar[1] = fvalue[1];
            fvar[2] = fvalue[2];
            break;
          }
        case TW_TYPE_UINT8:
          {
            unsigned char * ucvar = static_cast<unsigned char*>(var);
            unsigned char * ucvalue = static_cast<unsigned char*>(value);
            *ucvar = *ucvalue;
            break;
          }
        case TW_TYPE_BOOL32:
        case TW_TYPE_INT32:
          {
            int * ivar = static_cast<int*>(var);
            int * ivalue = static_cast<int*>(value);
            *ivar = *ivalue;
            break;
          }
        case TW_TYPE_UINT32:
          {
            unsigned int * uvar =   static_cast<unsigned int*>(var);
            unsigned int * uvalue = static_cast<unsigned int*>(value);
            *uvar = *uvalue;
            break;
          }
        case TW_TYPE_FLOAT:
          {
            float * fvar = static_cast<float*>(var);
            float * fvalue = static_cast<float*>(value);
            *fvar = *fvalue;
            break;
          }
        case TW_TYPE_DOUBLE:
          {
            double * dvar =   static_cast<double*>(var);
            double * fvalue = static_cast<double*>(value);
            *dvar = *fvalue;
            break;
          }
        case TW_TYPE_STDSTRING:
          {
            std::string * svar =   static_cast<std::string*>(var);
            std::string * svalue = static_cast<std::string*>(value);
            *svar = *svalue;
            break;
          }
        default:
          // Try to find type in custom enum types
          std::map<TwType,std::pair<const char *,std::vector<TwEnumVal> > >::iterator iter = 
            ReTw_custom_types.find(type);
          if(iter != ReTw_custom_types.end())
          {
            int * ivar = static_cast<int*>(var);
            std::vector<TwEnumVal>::iterator eit = (*iter).second.second.begin();
            bool found = false;
            for(;eit<(*iter).second.second.end();eit++)
            {
              if(strcmp(value_str,eit->Label) == 0)
              {
                *ivar = eit->Value;
                found = true;
                break;
              }
            }
            if(!found)
            {
              printf("ERROR_ENUM_VALUE_NOT_DEFINED");
            }
          }else
          {
            printf("ERROR_TYPE_NOT_SUPPORTED\n");
          }
          break;
      }
      item_found = true;
      break;
    }
  }

  // Try looking in CB items
  if(!item_found)
  {
    for(
      std::vector<ReTwCBItem>::iterator it = cb_items.begin(); 
      it != cb_items.end(); 
      it++)
    {
      if(it->name==name)
      {
        it->setCallback(value,it->clientData);
        item_found = true;
        break;
      }
    }
  }

  if(!item_found)
  {
    printf("ERROR: item '%s' not found\n",name);
  }
  return true;
}

IGL_INLINE const std::vector<igl::anttweakbar::ReTwRWItem> & 
  igl::anttweakbar::ReTwBar::get_rw_items()
{
  return rw_items;
}

IGL_INLINE const std::vector<igl::anttweakbar::ReTwCBItem> & 
  igl::anttweakbar::ReTwBar::get_cb_items()
{
  return cb_items;
}
