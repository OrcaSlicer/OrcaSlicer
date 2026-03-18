#import "OrcaSlicerAppService.h"

#include <memory>

#include "portability/app/SlicingOrchestrator.hpp"

@implementation OrcaSlicerSliceOutput
@end

@implementation OrcaSlicerSliceFailure
@end

@interface OrcaSlicerAppService () {
    std::unique_ptr<Slic3r::portability::app::SlicingOrchestrator> _orchestrator;
}
@end

@implementation OrcaSlicerAppService

+ (instancetype)sharedService
{
    static OrcaSlicerAppService *service = nil;
    static dispatch_once_t        onceToken;
    dispatch_once(&onceToken, ^{
        service = [[OrcaSlicerAppService alloc] initPrivate];
    });
    return service;
}

- (instancetype)init
{
    return [OrcaSlicerAppService sharedService];
}

- (instancetype)initPrivate
{
    self = [super init];
    if (self == nil)
        return nil;

    _orchestrator = std::make_unique<Slic3r::portability::app::SlicingOrchestrator>();
    return self;
}

- (BOOL)startSliceWithModelName:(NSString *)modelName
                  qualityPreset:(NSString *)qualityPreset
                  infillPercent:(NSInteger)infillPercent
                supportsEnabled:(BOOL)supportsEnabled
                progressHandler:(void (^)(double progressPercent, NSString *message))progressHandler
              completionHandler:(void (^)(OrcaSlicerSliceOutput *_Nullable output,
                                          OrcaSlicerSliceFailure *_Nullable failure,
                                          BOOL cancelled))completionHandler
{
    Slic3r::portability::app::SliceRequest request;
    request.model_name       = modelName.UTF8String != nullptr ? modelName.UTF8String : "";
    request.quality_preset   = qualityPreset.UTF8String != nullptr ? qualityPreset.UTF8String : "";
    request.infill_percent   = (int) infillPercent;
    request.supports_enabled = supportsEnabled;

    return _orchestrator->start(
        request,
        [progressHandler](const Slic3r::portability::app::SliceProgress& progress) {
            if (progressHandler == nil)
                return;

            NSString *message = [NSString stringWithUTF8String:progress.message.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{
                progressHandler(progress.progress_percent, message ?: @"");
            });
        },
        [completionHandler](std::unique_ptr<Slic3r::portability::app::SliceSuccess> success,
                            std::unique_ptr<Slic3r::portability::app::SliceFailure> failure,
                            bool cancelled) {
            OrcaSlicerSliceFailure *preparedFailure = nil;
            OrcaSlicerSliceOutput  *preparedOutput  = nil;
            BOOL                    wasCancelled    = cancelled ? YES : NO;

            if (failure != nullptr) {
                preparedFailure               = [OrcaSlicerSliceFailure new];
                preparedFailure.message       = [NSString stringWithUTF8String:failure->user_message.c_str()] ?: @"Slice failed.";
                preparedFailure.diagnosticLog = [NSString stringWithUTF8String:failure->diagnostic_log.c_str()] ?: @"";
            }

            if (success != nullptr) {
                preparedOutput                          = [OrcaSlicerSliceOutput new];
                preparedOutput.layerCount               = success->layer_count;
                preparedOutput.toolpathCount            = success->toolpath_count;
                preparedOutput.estimatedPrintTimeSeconds = success->estimated_print_time_seconds;
                preparedOutput.statusText               = [NSString stringWithUTF8String:success->status_text.c_str()] ?: @"Slice complete";
                preparedOutput.detailText               = [NSString stringWithUTF8String:success->detail_text.c_str()] ?: @"";
                preparedOutput.diagnosticLog            = [NSString stringWithUTF8String:success->diagnostic_log.c_str()] ?: @"";
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                if (completionHandler == nil)
                    return;

                if (wasCancelled) {
                    completionHandler(nil, nil, YES);
                    return;
                }

                if (preparedFailure != nil) {
                    completionHandler(nil, preparedFailure, NO);
                    return;
                }

                if (preparedOutput != nil) {
                    completionHandler(preparedOutput, nil, NO);
                    return;
                }

                OrcaSlicerSliceFailure *unknownFailure = [OrcaSlicerSliceFailure new];
                unknownFailure.message                 = @"Slice finished with an unknown state.";
                unknownFailure.diagnosticLog           = @"slice.failure: completion callback missing result payload";
                completionHandler(nil, unknownFailure, NO);
            });
        });
}

- (void)cancelSlice { _orchestrator->cancel(); }

- (BOOL)isSliceRunning { return _orchestrator->is_running(); }

@end
