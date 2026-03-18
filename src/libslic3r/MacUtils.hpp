#ifndef __MAC_UTILS_H
#define __MAC_UTILS_H
#include <string>

namespace Slic3r {

bool is_macos_support_boost_add_file_log();
int  is_mac_version_15();
std::wstring GetUserConfigDir();
}

#endif
