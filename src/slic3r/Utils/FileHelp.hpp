#ifndef file_help_hpp_
#define file_help_hpp_
#include <string>

#define STL_SVG_MAX_FILE_SIZE_MB 3
namespace Slic3r {
   namespace Utils {

bool is_file_too_large(std::string file_path, bool &try_ok);
void slash_to_back_slash(std::string& file_path);// "//" to "\"
   }
}
#endif // file_help_hpp_
