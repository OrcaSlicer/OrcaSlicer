#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface OrcaMetalViewportView : UIView

- (void)setCameraWithViewMatrix:(const double *)viewMatrix projectionMatrix:(const double *)projectionMatrix isLookingDownward:(BOOL)lookingDownward;
- (void)setLookingDownward:(BOOL)lookingDownward;
- (BOOL)isRendererReady;
- (NSString *)rendererInitializationSummary;

@end

NS_ASSUME_NONNULL_END
