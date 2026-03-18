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

    _backend = std::make_unique<Slic3r::portability::render::ios::IOSMetalRenderBackend>();
    auto *layer = (CAMetalLayer *) self.layer;
    _backend->bind_metal_layer(layer);
    _backend->initialize();
    _backend->resize((int) CGRectGetWidth(self.bounds), (int) CGRectGetHeight(self.bounds));

    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(renderFrame)];
    [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];

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

    if (_backend != nullptr)
        _backend->resize((int) CGRectGetWidth(self.bounds), (int) CGRectGetHeight(self.bounds));
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
    if (_backend == nullptr)
        return;

    if (_sceneStateDirty) {
        _backend->submit_scene_state(_sceneState);
        _sceneStateDirty = NO;
    }

    _backend->render_frame();
}

@end
