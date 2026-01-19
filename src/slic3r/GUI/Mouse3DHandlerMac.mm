#include "Mouse3DController.hpp"

#include <stdint.h>
#include <dlfcn.h>

#include <array>

#include <boost/log/trivial.hpp>
#include <cstdio>


static Slic3r::GUI::Mouse3DController* mouse_3d_controller = nullptr;

static uint16_t clientID = 0;

static bool driver_loaded = false;
static bool has_new_driver = false;  // drivers >= 10.2.2 are "new", and can process events on a separate thread

// replicate just enough of the 3Dx API for our uses, not everything the driver provides

#define kConnexionClientModeTakeOver 1
#define kConnexionMaskAxis 0x3f00
#define kConnexionMaskAll 0x3fff
#define kConnexionMaskAllButtons 0xffffffff
#define kConnexionCmdHandleButtons 2
#define kConnexionCmdHandleAxis 3
#define kConnexionCmdAppSpecific 10
#define kConnexionMsgDeviceState '3dSR'
#define kConnexionCtlGetDeviceID '3did'

#pragma pack(push, 2)
struct ConnexionDeviceState {
  uint16_t version;
  uint16_t client;
  uint16_t command;
  int16_t param;
  int32_t value;
  uint64_t time;
  uint8_t report[8];
  uint16_t buttons8;  // obsolete! (pre-10.x drivers)
  int16_t axis[6];    // tx, ty, tz, rx, ry, rz
  uint16_t address;
  uint32_t buttons;
};
#pragma pack(pop)

// callback functions:
typedef void (*AddedHandler)(uint32_t);
typedef void (*RemovedHandler)(uint32_t);
typedef void (*MessageHandler)(uint32_t, uint32_t msg_type, void *msg_arg);

// driver functions:
typedef int16_t (*SetConnexionHandlers_ptr)(MessageHandler, AddedHandler, RemovedHandler, bool);
typedef int16_t (*InstallConnexionHandlers_ptr)(MessageHandler, AddedHandler, RemovedHandler);
typedef void (*CleanupConnexionHandlers_ptr)();
typedef uint16_t (*RegisterConnexionClient_ptr)(uint32_t signature,
                                                const char *name,
                                                uint16_t mode,
                                                uint32_t mask);
typedef void (*SetConnexionClientButtonMask_ptr)(uint16_t clientID, uint32_t buttonMask);
typedef void (*UnregisterConnexionClient_ptr)(uint16_t clientID);
typedef int16_t (*ConnexionClientControl_ptr)(uint16_t clientID,
                                              uint32_t message,
                                              int32_t param,
                                              int32_t *result);

#define DECLARE_FUNC(name) name##_ptr name = nullptr

DECLARE_FUNC(SetConnexionHandlers);
DECLARE_FUNC(InstallConnexionHandlers);
DECLARE_FUNC(CleanupConnexionHandlers);
DECLARE_FUNC(RegisterConnexionClient);
DECLARE_FUNC(SetConnexionClientButtonMask);
DECLARE_FUNC(UnregisterConnexionClient);
DECLARE_FUNC(ConnexionClientControl);

static void *load_func(void *module, const char *func_name)
{
  void *func = dlsym(module, func_name);

  if (func) {
      BOOST_LOG_TRIVIAL(info) << func_name <<" loaded";
  }
  else {
    BOOST_LOG_TRIVIAL(error) <<"loading 3dx drivers dlsym error: "<< dlerror();
  }

  return func;
}

#define LOAD_FUNC(name) name = (name##_ptr)load_func(module, #name)

static void *module;  // handle to the whole driver

static bool load_driver_functions()
{
  if (driver_loaded)
    return true;

  module = dlopen("/Library/Frameworks/3DconnexionClient.framework/3DconnexionClient",
                  RTLD_LAZY | RTLD_LOCAL);

  if (module) {
    BOOST_LOG_TRIVIAL(info) << "loading 3dx drivers";
    LOAD_FUNC(SetConnexionHandlers);

    if (SetConnexionHandlers != nullptr) {
      driver_loaded = true;
      has_new_driver = true;
    }
    else {
      BOOST_LOG_TRIVIAL(info) << "installing 3dx drivers";
      LOAD_FUNC(InstallConnexionHandlers);
      driver_loaded = (InstallConnexionHandlers != nullptr);
    }

    if (driver_loaded) {
      LOAD_FUNC(CleanupConnexionHandlers);
      LOAD_FUNC(RegisterConnexionClient);
      LOAD_FUNC(SetConnexionClientButtonMask);
      LOAD_FUNC(UnregisterConnexionClient);
      LOAD_FUNC(ConnexionClientControl);
    }
  }
  else {
    BOOST_LOG_TRIVIAL(error) << "3dx drivers module loading error: "<< dlerror() ;
  }

  BOOST_LOG_TRIVIAL(info) << "3dx drivers loaded: " << (driver_loaded ? (has_new_driver ? "YES, new" : "YES, old") : "NO");
  return driver_loaded;
}

static void unload_driver()
{
  dlclose(module);
}

static void DeviceAdded(uint32_t unused)
{
  BOOST_LOG_TRIVIAL(info)<<"3dx device added";
  // determine exactly which device is plugged in
  int32_t result;
  ConnexionClientControl(clientID, kConnexionCtlGetDeviceID, 0, &result);
  int vendorID  = result >> 16;
  int productID = result & 0xffff;
  //TODO: verify device
  char buf[64];
  sprintf(buf, "VID%04X,PID%04X", vendorID, productID);
  mouse_3d_controller->connected(buf);
}

static void DeviceRemoved(uint32_t unused)
{
  BOOST_LOG_TRIVIAL(info) << "3dx device removed\n";
// not accessible in a free function
//  assert(m_connected);
//  assert(! m_device_str.empty());
  mouse_3d_controller->disconnected();
}

static void DeviceEvent(uint32_t unused, uint32_t msg_type, void *msg_arg)
{
  if (msg_type == kConnexionMsgDeviceState) {
    ConnexionDeviceState *s = (ConnexionDeviceState *)msg_arg;
    if (s->client == clientID) {
      switch (s->command) {
        case kConnexionCmdHandleAxis: {
          // The axis field is an array of 6 signed 16-bit integers corresponding to the 6 device axes. Data is ordered as Tx, Tz, Ty, Rx, Rz, Ry. The values reported are scaled by the driver according to the speed slider settings on the 3Dconnexion preference panel. At maximum speed, the range is - 1024 to 1024. Typical range that you should optimize your application for should be -500 to 500.
          // Actually we are getting values way over 1024. Max is probably 2048 now.
          Slic3r::GUI::Mouse3DController::DataPacketAxis packet;
          for (int i = 0; i < 6; ++ i)
              packet[i] = (double)s->axis[i]/350.0;//wanted to divide by 500 but 350 is used at raw input so i used same value.
          mouse_3d_controller->handle_input(packet);
          break;
        }
        case kConnexionCmdHandleButtons:
          break;
        case kConnexionCmdAppSpecific:
          break;
        default:
        break;
      }
    }
  }
}

namespace Slic3r {
namespace GUI {

// Initialize the application.
void Mouse3DController::init()
{
  BOOST_LOG_TRIVIAL(info) << "3dx mac handler starts";
  if (load_driver_functions()) {
    mouse_3d_controller = this;

    uint16_t error = has_new_driver ? 
      SetConnexionHandlers(DeviceEvent, DeviceAdded, DeviceRemoved, false) :
      InstallConnexionHandlers(DeviceEvent, DeviceAdded, DeviceRemoved);

    if (! error) {
      // Registration is done either by 4letter constant (CFBundleSignature - obsolete
      //and we dont have that) or Executable name in pascal string(first byte is string lenght).
      //If no packets are recieved the name might be different - check cmake. If debugging try commenting
      // set_target_properties(OrcaSlicer PROPERTIES OUTPUT_NAME "orca-slicer")
      clientID = RegisterConnexionClient(
          0, "\013OrcaSlicer", kConnexionClientModeTakeOver, kConnexionMaskAxis);
        BOOST_LOG_TRIVIAL(info) << "3dx mac handler registered";
    }
  }
}

void Mouse3DController::shutdown()
{
  if (driver_loaded) {
    UnregisterConnexionClient(clientID);
    CleanupConnexionHandlers();
    unload_driver();
  }
  // Copy the current parameters to parameter database, mark the device as disconnected.
  this->disconnected();
  mouse_3d_controller = nullptr;
}

}}//namespace Slic3r::GUI
