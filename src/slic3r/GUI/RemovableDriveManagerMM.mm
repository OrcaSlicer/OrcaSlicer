#import "RemovableDriveManager.hpp"
#import "RemovableDriveManagerMM.h"
#import "GUI_App.hpp"
#import <AppKit/AppKit.h> 
#import <DiskArbitration/DiskArbitration.h>

static void eject_callback(DADiskRef disk, DADissenterRef dissenter, void *context)
{
    NSLog(@"eject successfull");
}

static void unmount_callback(DADiskRef disk, DADissenterRef dissenter, void *context)
{
    //if (dissenter) {
        //?
    //} else {
        DADiskEject(disk, kDADiskEjectOptionDefault, eject_callback, context);
    //}
}

@implementation RemovableDriveManagerMM

-(instancetype) init
{
	self = [super init];
	//if(self){}
	return self;
}

-(void) on_device_unmount: (NSNotification*) notification
{
    //NSLog(@"on device change");
    Slic3r::GUI::wxGetApp().removable_drive_manager()->update();
}

-(void) add_unmount_observer
{
    //NSLog(@"add unmount observer");
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector: @selector(on_device_unmount:) name:NSWorkspaceDidUnmountNotification object:nil];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self selector: @selector(on_device_unmount:) name:NSWorkspaceDidMountNotification object:nil];
}

-(NSArray*) list_dev
{
    // DEPRICATED:
    //NSArray* devices = [[NSWorkspace sharedWorkspace] mountedRemovableMedia];
	//return devices;
    
    NSArray *mountedRemovableMedia = [[NSFileManager defaultManager] mountedVolumeURLsIncludingResourceValuesForKeys:nil options:NSVolumeEnumerationSkipHiddenVolumes];
    NSMutableArray *result = [NSMutableArray array];
    for(NSURL *volURL in mountedRemovableMedia)
    {
        int                 err = 0;
        DADiskRef           disk;
        CFDictionaryRef     descDict = nullptr;        
        DASessionRef        session = DASessionCreate(nullptr);
        if (session == nullptr)
            err = EINVAL;
        if (err == 0) {
            disk = DADiskCreateFromVolumePath(nullptr,session,(CFURLRef)volURL);
            if (session == nullptr)
                err = EINVAL;
        }
        if (err == 0) {
            descDict = DADiskCopyDescription(disk);
            if (descDict == nullptr)
                err = EINVAL;
        }
        if (err == 0) {
            CFTypeRef mediaEjectableKey = CFDictionaryGetValue(descDict,kDADiskDescriptionMediaEjectableKey);
            BOOL ejectable = [(id)mediaEjectableKey boolValue];
            CFTypeRef deviceProtocolName = CFDictionaryGetValue(descDict,kDADiskDescriptionDeviceProtocolKey);
            
            CFTypeRef deviceModelKey = CFDictionaryGetValue(descDict, kDADiskDescriptionDeviceModelKey);
            //debug logging
            /*
            if (deviceProtocolName)
                NSLog(@"%@",(CFStringRef)deviceProtocolName);
            if (deviceModelKey)
                NSLog(@"-%@",(CFStringRef)deviceModelKey);
            */
            if (mediaEjectableKey != nullptr) {
                BOOL op = ejectable && 
                    ( (deviceProtocolName != nullptr && (CFEqual(deviceProtocolName, CFSTR("USB")) || CFEqual(deviceProtocolName, CFSTR("Secure Digital")))) || 
                      (deviceModelKey     != nullptr && CFEqual(deviceModelKey, CFSTR("SD Card Reader"))) );
                //!CFEqual(deviceModelKey, CFSTR("Disk Image"));
                if (op)
                    [result addObject:volURL.path];
            }
        }
        if (descDict != nullptr)
            CFRelease(descDict);
    }
    return result;
}

//this eject drive is not used now
-(void)eject_drive:(NSString *)path
{
    NSURL *url = [[NSURL alloc] initFileURLWithPath:path];
    DASessionRef session = DASessionCreate(nullptr);
    int err = 0;
    if (session == nullptr)
        err = EINVAL;
    DADiskRef disk = nullptr;
    if (err == 0)
        disk = DADiskCreateFromVolumePath(nullptr, session, (CFURLRef)url);
    if (err == 0)
        //DADiskUnmount(disk, kDADiskUnmountOptionDefault, nullptr, nullptr);
        DADiskUnmount(disk, kDADiskUnmountOptionWhole | kDADiskUnmountOptionForce, unmount_callback, nullptr);
    if (disk != nullptr)
        CFRelease(disk);
    if (session != nullptr)
        CFRelease(session);
}

@end

namespace Slic3r {
namespace GUI {

void RemovableDriveManager::register_window_osx()
{
    assert(m_impl_osx == nullptr);
    m_impl_osx = [[RemovableDriveManagerMM alloc] init];
	if (m_impl_osx)
		[(id)m_impl_osx add_unmount_observer];
}

void RemovableDriveManager::unregister_window_osx()
{
    if (m_impl_osx) {
        [(id)m_impl_osx release];
        m_impl_osx = nullptr;
    }
}

namespace search_for_drives_internal 
{
    void inspect_file(const std::string &path, const std::string &parent_path, std::vector<DriveData> &out);
}

void RemovableDriveManager::list_devices(std::vector<DriveData> &out) const
{
    assert(m_impl_osx != nullptr);
    if (m_impl_osx) {
    	NSArray* devices = [(id)m_impl_osx list_dev];
    	for (NSString* volumePath in devices)
        	search_for_drives_internal::inspect_file(std::string([volumePath UTF8String]), "/Volumes", out);
    }
}

// not used as of now
void RemovableDriveManager::eject_device(const std::string &path)
{
    assert(m_impl_osx != nullptr);
    if (m_impl_osx) {
        NSString * pth = [NSString stringWithCString:path.c_str() encoding:[NSString defaultCStringEncoding]];
        [(id)m_impl_osx eject_drive:pth];
    }
}

}}//namespace Slicer::GUI
