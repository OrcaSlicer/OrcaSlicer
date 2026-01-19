#import <Cocoa/Cocoa.h>

@interface RemovableDriveManagerMM : NSObject

-(instancetype) init;
-(void) add_unmount_observer;
-(void) on_device_unmount: (NSNotification*) notification;
-(NSArray*) list_dev;
-(void)eject_drive:(NSString *)path;
@end
