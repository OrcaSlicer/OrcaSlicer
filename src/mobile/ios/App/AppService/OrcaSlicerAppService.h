#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface OrcaSlicerSliceOutput : NSObject
@property(nonatomic, assign) NSInteger layerCount;
@property(nonatomic, assign) NSInteger toolpathCount;
@property(nonatomic, assign) NSInteger estimatedPrintTimeSeconds;
@property(nonatomic, copy) NSString *statusText;
@property(nonatomic, copy) NSString *detailText;
@property(nonatomic, copy) NSString *diagnosticLog;
@end

@interface OrcaSlicerSliceFailure : NSObject
@property(nonatomic, copy) NSString *message;
@property(nonatomic, copy) NSString *diagnosticLog;
@end

@interface OrcaSlicerAppService : NSObject
+ (instancetype)sharedService;

- (BOOL)startSliceWithModelName:(NSString *)modelName
                  qualityPreset:(NSString *)qualityPreset
                  infillPercent:(NSInteger)infillPercent
                supportsEnabled:(BOOL)supportsEnabled
                progressHandler:(void (^)(double progressPercent, NSString *message))progressHandler
              completionHandler:(void (^)(OrcaSlicerSliceOutput *_Nullable output,
                                          OrcaSlicerSliceFailure *_Nullable failure,
                                          BOOL cancelled))completionHandler;

- (void)cancelSlice;
- (BOOL)isSliceRunning;
@end

NS_ASSUME_NONNULL_END
