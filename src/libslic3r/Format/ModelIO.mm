#include "ModelIO.hpp"
#import <ModelIO/ModelIO.h>

namespace Slic3r {

    std::string make_temp_stl_with_modelio(const std::string &input_file)
    {
        NSURL    *input_url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:input_file.c_str()]];
        MDLAsset *asset     = [[MDLAsset alloc] initWithURL:input_url];

        NSString *tmp_file_name = [[[NSUUID UUID] UUIDString] stringByAppendingPathExtension:@"stl"];
        NSURL    *tmp_file_url  = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:tmp_file_name]];

        if ([asset exportAssetToURL:tmp_file_url]) {
            std::string output_file = std::string([[tmp_file_url path] UTF8String]);
            return output_file;
        }

        return std::string();
    }
    void delete_temp_file(const std::string &temp_file)
    {
        NSString *file_path = [NSString stringWithUTF8String:temp_file.c_str()];
        [[NSFileManager defaultManager] removeItemAtPath:file_path error:NULL];
    }

} // namespace Slic3r