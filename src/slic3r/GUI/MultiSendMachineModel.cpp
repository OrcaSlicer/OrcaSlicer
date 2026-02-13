#include "MultiSendMachineModel.hpp"

#include "GUI.hpp"

namespace Slic3r {
namespace GUI {

MultiSendMachineModel::MultiSendMachineModel()
{
    ;
}

MultiSendMachineModel::~MultiSendMachineModel()
{
    ;
}

void MultiSendMachineModel::Init()
{
    ;
}

wxDataViewItem MultiSendMachineModel::AddMachine(MachineObject* obj)
{
    wxString name = from_u8(obj->get_dev_name());

    wxDataViewItem new_item;

    // TODO
    return new_item;
}


} // namespace GUI
} // namespace Slic3r
