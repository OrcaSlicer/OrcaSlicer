#import "MacUtils.hpp"

#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>

#ifdef WORDS_BIGENDIAN
    #define kCFStringEncodingUTF32Native kCFStringEncodingUTF32BE
#else
    #define kCFStringEncodingUTF32Native kCFStringEncodingUTF32LE
#endif

namespace Slic3r {

bool is_macos_support_boost_add_file_log()
{
    if (@available(macOS 12.0, *)) {
        return true;
    } else {
        return false;
    }
}

int is_mac_version_15()
{
    if (@available(macOS 15.0, *)) {//This code runs on macOS 15 or later.
        return true;
    } else {
        return false;
    }
}

// Source from wxWidgets wxStandardPaths::GetUserConfigDir, reimplemented using std strings
static std::wstring convertMacNewLines10To13(const std::wstring& str)
{
    std::wstring data(str);
    for (wchar_t& c : data) {
        if (c == L'\n')
            c = L'\r';
    }
    return data;
}

std::wstring CFAsString(CFStringRef ref)
{
    if ( !ref )
        return std::wstring() ;

    Size cflen = CFStringGetLength( ref )  ;

    CFStringEncoding cfencoding;
    std::wstring result;
    cfencoding = kCFStringEncodingUTF32Native;

    CFIndex cStrLen ;
    CFStringGetBytes( ref , CFRangeMake(0, cflen) , cfencoding ,
        '?' , false , NULL , 0 , &cStrLen ) ;
    char* buf = new char[cStrLen];
    CFStringGetBytes( ref , CFRangeMake(0, cflen) , cfencoding,
        '?' , false , (unsigned char*) buf , cStrLen , &cStrLen) ;

    result = std::wstring( (const wchar_t*) buf , cStrLen/4);

    delete[] buf ;
    return convertMacNewLines10To13(result);
}

std::wstring GetFMDirectory(
                                   NSSearchPathDirectory directory,
                                   NSSearchPathDomainMask domainMask)
{
    NSURL* url = [[NSFileManager defaultManager] URLForDirectory:directory
                                           inDomain:domainMask
                                  appropriateForURL:nil
                                             create:NO error:nil];
    return CFAsString((CFStringRef)url.path);
}

std::wstring GetUserConfigDir()
{
    return GetFMDirectory(NSLibraryDirectory, NSUserDomainMask) + L"/Preferences";
}
}; // namespace Slic3r
