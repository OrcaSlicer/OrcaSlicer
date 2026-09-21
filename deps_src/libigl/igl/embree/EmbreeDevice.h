// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Vladimir Fonov <vladimir.fonov@gmail.com>
//               2013 Alec Jacobson <alecjacobson@gmail.com>
//               2014 Christian Schüller <schuellchr@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
//

#ifndef IGL_EMBREE_EMBREE_DEVICE_H
#define IGL_EMBREE_EMBREE_DEVICE_H
#include <embree4/rtcore.h>
#include <iostream>

namespace igl
{
  namespace embree
  {
     /// keep track of embree device instance
     struct EmbreeDevice
     {
        RTCDevice embree_device;
        int       embree_device_cntr;

        static EmbreeDevice & instance()
        {
            static EmbreeDevice s;
            return s;
        } // instance

        EmbreeDevice(const EmbreeDevice &) = delete;
        EmbreeDevice & operator = (const EmbreeDevice &) = delete;

        static RTCDevice get_device(const char *config=nullptr)
        {
            return instance().get(config);
        }

        static void release_device(void)
        {
            instance().release();
        }

    private:

        EmbreeDevice():embree_device(nullptr),embree_device_cntr(0) {}

        ~EmbreeDevice() 
        { 
            if(embree_device)
                rtcReleaseDevice(embree_device);
        }

        RTCDevice get(const char *config=nullptr) 
        {
            if(!embree_device) 
            {
                embree_device = rtcNewDevice (config);
                if(rtcGetDeviceError (embree_device) != RTC_ERROR_NONE)
                {
                  assert(false && "Embree: An error occurred while initializing embree core!");
#ifdef IGL_VERBOSE
                  std::cerr << "Embree: An error occurred while initializing embree core!" << std::endl;
                }
                else
                {
                  std::cerr << "Embree: core initialized." << std::endl;
#endif
                }
            }
            ++embree_device_cntr;
            return embree_device;
        }

        void release()
        {
            if(!--embree_device_cntr) {
                rtcReleaseDevice (embree_device);
                embree_device = nullptr;                
#ifdef IGL_VERBOSE
                std::cerr << "Embree: core released." << std::endl;
#endif
            }
        }
     };
  }
}

#endif // IGL_EMBREE_EMBREE_DEVICE_H
