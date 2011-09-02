//
//  BambuPlayer.h
//  BambuPlayer
//
//  Created by cmguo on 2021/12/6.
//

#import <Foundation/Foundation.h>
#import <AVFoundation/AVSampleBufferDisplayLayer.h>
#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN
	
@interface BambuPlayer : NSObject

+ (void) initialize;

- (instancetype) initWithDisplayLayer: (AVSampleBufferDisplayLayer*) layer;
- (instancetype) initWithImageView: (NSView*) view;
- (int) open: (char const *) url;
- (NSSize) videoSize;
- (int) play;
- (void) stop;
- (void) close;

- (void) setLogger: (void (*)(void const * context, int level, char const * msg)) logger withContext: (void const *) context;

@end

NS_ASSUME_NONNULL_END
