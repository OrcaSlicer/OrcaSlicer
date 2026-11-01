#ifndef PARTSKIPCOMMON_H
#define PARTSKIPCOMMON_H

#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {
    
enum PartState {
    psUnCheck,
    psChecked,
    psSkipped
};


typedef std::vector<std::pair<int, PartState>> PartsInfo;

}}

#endif // PARTSKIPCOMMON_H