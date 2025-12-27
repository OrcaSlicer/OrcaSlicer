#ifndef slic3r_MultiSendMachineModel_hpp_
#define slic3r_MultiSendMachineModel_hpp_

#include "DeviceManager.hpp"
#include "wx/dataview.h"

namespace Slic3r { 
namespace GUI {

class MultiSendMachineModel : public wxDataViewModel
{
public:
    MultiSendMachineModel();
    ~MultiSendMachineModel();

    void Init();

    wxDataViewItem AddMachine(MachineObject* obj);

private:
};

} // namespace GUI
} // namespace Slic3r

#endif
