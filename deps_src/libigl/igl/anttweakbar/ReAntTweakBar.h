// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ANTTWEAKBAR_REANTTWEAKBAR_H
#define IGL_ANTTWEAKBAR_REANTTWEAKBAR_H
#include "../igl_inline.h"
// ReAntTweakBar is a minimal wrapper for the AntTweakBar library that allows
// "bars" to be saved and load from disk. Changing your existing app that uses
// AntTweakBar to use ReAntTweakBar is trivial.
// 
// Many (but not all) variable types are supported. I'll try to keep track them
// here:
//   TW_TYPE_BOOLCPP
//   TW_TYPE_QUAT4F
//   TW_TYPE_QUAT4D
//   TW_TYPE_COLOR4F
//   TW_TYPE_COLOR4D
//   TW_TYPE_COLOR3F
//   TW_TYPE_DIR3F
//   TW_TYPE_DIR3D
//   TW_TYPE_BOOL32
//   TW_TYPE_INT32
//   TW_TYPE_UINT32
//   TW_TYPE_FLOAT
//   TW_TYPE_DOUBLE
//   TW_TYPE_UINT8
//   and
//   custom TwTypes made with TwDefineEnum
// 
// I'm working on adding the rest on an as-needed basis. Adding a new type only
// requires changes in a few places...
// 
//
//

// This allows the user to have a non-global, static installation of
// AntTweakBar
#include <AntTweakBar.h>
// Instead of including AntTweakBar.h, just define the necessary types
// Types used:
//   - TwType
//   - TwEnumVal
//   - TwSetVarCallback
//   - TwGetVarCallback
//   - TwBar
//   - TwButtonCallback


#include <vector>
#include <string>

#define REANTTWEAKBAR_MAX_CB_VAR_SIZE 1000
// Max line size for reading files
#define REANTTWEAKBAR_MAX_LINE 1000
#define REANTTWEAKBAR_MAX_WORD 100

namespace igl
{
  namespace anttweakbar
  {
    TwType ReTwDefineEnum(
      const char *name, 
      const TwEnumVal *enumValues, 
      unsigned int nbValues);
    TwType ReTwDefineEnumFromString(const char * name,const char * enumString);
    
    struct ReTwRWItem
    {
      //const char * name;
      std::string name;
      TwType type;
      void * var;
      // Default constructor
      IGL_INLINE ReTwRWItem(
        const std::string _name,
        TwType _type, 
        void *_var):
        name(_name),
        type(_type),
        var(_var)
      {
      }
      // Shallow copy constructor
      // I solemnly swear it's OK to copy var this way
      // Q: Is it really?
      IGL_INLINE ReTwRWItem(const ReTwRWItem & that):
        name(that.name),
        type(that.type),
        var(that.var)
      {
      }
      // Shallow assignment 
      // I solemnly swear it's OK to copy var this way
      IGL_INLINE ReTwRWItem & operator=(const ReTwRWItem & that)
      {
        if(this != &that)
        {
          this->name = that.name;
          this->type = that.type;
          this->var = that.var;
        }
        return *this;
      }
    };
    
    struct ReTwCBItem
    {
      //const char * name;
      std::string name;
      TwType type;
      TwSetVarCallback setCallback;
      TwGetVarCallback getCallback;
      void * clientData;
      // Default constructor
      IGL_INLINE ReTwCBItem(
        const std::string _name,
        TwType _type, 
        TwSetVarCallback _setCallback,
        TwGetVarCallback _getCallback,
        void * _clientData):
        name(_name),
        type(_type),
        setCallback(_setCallback),
        getCallback(_getCallback),
        clientData(_clientData)
      {
      }
      // Shallow copy
      // I solemnly swear it's OK to copy clientData this way
      IGL_INLINE ReTwCBItem(const ReTwCBItem & that):
        name(that.name),
        type(that.type),
        setCallback(that.setCallback),
        getCallback(that.getCallback),
        clientData(that.clientData)
      {
      }
      // Shallow assignment
      // I solemnly swear it's OK to copy clientData this way
      IGL_INLINE ReTwCBItem & operator=(const ReTwCBItem & that)
      {
        if(this != &that)
        {
          name = that.name;
          type = that.type;
          setCallback = that.setCallback;
          getCallback = that.getCallback;
          clientData = that.clientData;
        }
        return *this;
      }
  
    };
    
    class ReTwBar
    {
      // VARIABLES
      // Should be private, but seeing as I'm not going to implement all of the
      // AntTweakBar public functions right away, I'll expose this so that at
      // anytime AntTweakBar functions can be called directly on the bar
      public:
        TwBar * bar;
        std::string name;
      protected:
        std::vector<ReTwRWItem> rw_items;
        std::vector<ReTwCBItem> cb_items;
      public:
        // Default constructor with explicit initialization
        IGL_INLINE ReTwBar();
      private:
        // Copy constructor does shallow copy
        IGL_INLINE ReTwBar(const ReTwBar & that);
        // Assignment operator does shallow assignment
        IGL_INLINE ReTwBar &operator=(const ReTwBar & that);
    
      // WRAPPERS FOR ANTTWEAKBAR FUNCTIONS 
      public:
        IGL_INLINE void TwNewBar(const char *_name);
        IGL_INLINE int TwAddVarRW(
          const char *name, 
          TwType type, 
          void *var, 
          const char *def,
          const bool record=true);
        IGL_INLINE int TwAddVarCB(
          const char *name, 
          TwType type, 
          TwSetVarCallback setCallback, 
          TwGetVarCallback getCallback, 
          void *clientData, 
          const char *def,
          const bool record=true);
        // Wrappers for convenience (not recorded, just passed on)
        IGL_INLINE int TwAddVarRO(const char *name, TwType type, void *var, const char *def);
        IGL_INLINE int TwAddButton(
          const char *name, 
          TwButtonCallback buttonCallback, 
          void *clientData, 
          const char *def);
        IGL_INLINE int TwSetParam(
          const char *varName, 
          const char *paramName, 
          TwParamValueType paramValueType, 
          unsigned int inValueCount, 
          const void *inValues);
        IGL_INLINE int TwGetParam(
          const char *varName, 
          const char *paramName, 
          TwParamValueType paramValueType, 
          unsigned int outValueMaxCount, 
          void *outValues);
        IGL_INLINE int TwRefreshBar();
        IGL_INLINE int TwTerminate();
    
    
      // IO FUNCTIONS
      public:
        // Save current items to file
        // Input:
        //   file_name  name of file to save data to, can be null which means print
        //   to stdout
        // Return:
        //   true only if there were no (fatal) errors
        IGL_INLINE bool save(const char *file_name);
        std::string get_value_as_string(
          void * var, 
          TwType type);
        // Load into current items from file
        // Input:
        //   file_name  name of input file to load
        // Return:
        //   true only if there were no (fatal) errors
        IGL_INLINE bool load(const char *file_name);
        // Get TwType from string
        // Input
        //   type_str  string of type 
        // Output
        //   type  TwType converted from string
        // Returns
        //   true only if string matched a valid type
        IGL_INLINE bool type_from_string(const char *type_str, TwType & type);
        // I realize that I mix std::string and const char * all over the place.
        // What can you do...
        IGL_INLINE bool set_value_from_string(
          const char * name, 
          TwType type, 
          const char * value_str);
        IGL_INLINE const std::vector<ReTwRWItem> & get_rw_items();
        IGL_INLINE const std::vector<ReTwCBItem> & get_cb_items();
    };
  }
}

// List of TwBar functions
//TW_API TwBar *      TW_CALL TwNewBar(const char *barName);
//TW_API int          TW_CALL TwDeleteBar(TwBar *bar);
//TW_API int          TW_CALL TwDeleteAllBars();
//TW_API int          TW_CALL TwSetTopBar(const TwBar *bar);
//TW_API TwBar *      TW_CALL TwGetTopBar();
//TW_API int          TW_CALL TwSetBottomBar(const TwBar *bar);
//TW_API TwBar *      TW_CALL TwGetBottomBar();
//TW_API const char * TW_CALL TwGetBarName(TwBar *bar);
//TW_API int          TW_CALL TwGetBarCount();
//TW_API TwBar *      TW_CALL TwGetBarByIndex(int barIndex);
//TW_API TwBar *      TW_CALL TwGetBarByName(const char *barName);
//TW_API int          TW_CALL TwRefreshBar(TwBar *bar);
//TW_API int          TW_CALL TwTerminate();
//
//TW_API int      TW_CALL TwAddVarRW(TwBar *bar, const char *name, TwType type, void *var, const char *def);
//TW_API int      TW_CALL TwAddVarRO(TwBar *bar, const char *name, TwType type, const void *var, const char *def);
//TW_API int      TW_CALL TwAddVarCB(TwBar *bar, const char *name, TwType type, TwSetVarCallback setCallback, TwGetVarCallback getCallback, void *clientData, const char *def);
//TW_API int      TW_CALL TwAddButton(TwBar *bar, const char *name, TwButtonCallback callback, void *clientData, const char *def);
//TW_API int      TW_CALL TwAddSeparator(TwBar *bar, const char *name, const char *def);
//TW_API int      TW_CALL TwRemoveVar(TwBar *bar, const char *name);
//TW_API int      TW_CALL TwRemoveAllVars(TwBar *bar);

// Until AntTweakBar dependency folder exists, this is header-only
#ifndef IGL_STATIC_LIBRARY
#  include "ReAntTweakBar.cpp"
#endif

#endif
