#import "OrcaMetalViewportView.h"

#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <memory>

#include "portability/render/ISceneRenderer.hpp"
#include "portability/render/ios/IOSMetalRenderBackend.hpp"

@interface OrcaMetalViewportView () {
    std::unique_ptr<Slic3r::portability::render::ios::IOSMetalRenderBackend> _backend;
    CADisplayLink *_displayLink;
    Slic3r::portability::render::RenderSceneState _sceneState;
    BOOL _sceneStateDirty;
    BOOL _rendererReady;
    NSString *_rendererInitializationSummary;
}
@end

@implementation OrcaMetalViewportView

+ (Class)layerClass
{
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self == nil)
        return nil;

    self.opaque = YES;
    self.contentScaleFactor = UIScreen.mainScreen.scale;

    _sceneState = Slic3r::portability::render::RenderSceneState{};
    _sceneStateDirty = YES;
    _rendererReady = NO;
    _rendererInitializationSummary = @"renderer not initialized";

    _backend = std::make_unique<Slic3r::portability::render::ios::IOSMetalRenderBackend>();
    auto *layer = (CAMetalLayer *) self.layer;
    NSLog(@"[OrcaMetal] OrcaMetalViewportView.initWithFrame frame=%@ bounds=%@ scale=%.2f", NSStringFromCGRect(frame), NSStringFromCGRect(self.bounds), self.contentScaleFactor);
    _backend->bind_metal_layer(layer);
    const bool initialized = _backend->initialize();
    if (initialized) {
        _rendererReady = YES;
        _rendererInitializationSummary = @"renderer ready";
        _backend->resize((int) CGRectGetWidth(self.bounds), (int) CGRectGetHeight(self.bounds));
        NSLog(@"[OrcaMetal] viewport renderer ready (display link deferred until layout)");
    } else {
        _rendererInitializationSummary = @"renderer init failed";
        self.backgroundColor = [UIColor colorWithRed:0.10 green:0.08 blue:0.14 alpha:1.0];
        NSLog(@"[OrcaMetal] viewport renderer initialization failed; keeping static fallback background");
    }

    return self;
}

- (void)dealloc
{
    [_displayLink invalidate];
    _displayLink = nil;

    if (_backend != nullptr) {
        _backend->shutdown();
        _backend.reset();
    }
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    NSLog(@"[OrcaMetal] OrcaMetalViewportView.layoutSubviews bounds=%@ scale=%.2f", NSStringFromCGRect(self.bounds), self.contentScaleFactor);

    if (_backend != nullptr)
        _backend->resize((int) CGRectGetWidth(self.bounds), (int) CGRectGetHeight(self.bounds));

    if (_rendererReady && _displayLink == nil &&
        CGRectGetWidth(self.bounds) > 0 && CGRectGetHeight(self.bounds) > 0) {
        _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(renderFrame)];
        [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        NSLog(@"[OrcaMetal] display link started after layout bounds=%@", NSStringFromCGRect(self.bounds));
    }
}

- (void)setCameraWithViewMatrix:(const double *)viewMatrix projectionMatrix:(const double *)projectionMatrix isLookingDownward:(BOOL)lookingDownward
{
    if (_backend == nullptr || viewMatrix == nullptr || projectionMatrix == nullptr)
        return;

    std::copy_n(viewMatrix, _sceneState.camera.view_matrix.size(), _sceneState.camera.view_matrix.begin());
    std::copy_n(projectionMatrix, _sceneState.camera.projection_matrix.size(), _sceneState.camera.projection_matrix.begin());
    _sceneState.camera.is_looking_downward = lookingDownward;
    _sceneStateDirty = YES;
}

- (void)setLookingDownward:(BOOL)lookingDownward
{
    _sceneState.camera.is_looking_downward = lookingDownward;
    _sceneStateDirty = YES;
}

- (void)renderFrame
{
    if (_backend == nullptr || !_rendererReady)
        return;

    if (_sceneStateDirty) {
        _backend->submit_scene_state(_sceneState);
        _sceneStateDirty = NO;
    }

    _backend->render_frame();
    static int render_log_count = 0;
    if (render_log_count < 5) {
        NSLog(@"[OrcaMetal] OrcaMetalViewportView.renderFrame #%d bounds=%@ ready=%@", render_log_count + 1, NSStringFromCGRect(self.bounds), _rendererReady ? @"true" : @"false");
        render_log_count += 1;
    }
}

- (BOOL)isRendererReady { return _rendererReady; }

- (NSString *)rendererInitializationSummary { return _rendererInitializationSummary ?: @"renderer status unknown"; }

@end
