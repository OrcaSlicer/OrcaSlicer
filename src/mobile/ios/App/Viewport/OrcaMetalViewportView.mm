#import "OrcaMetalViewportView.h"

#import <QuartzCore/CAMetalLayer.h>

#include <memory>

#include "portability/render/ISceneRenderer.hpp"
#include "portability/render/ios/IOSMetalRenderBackend.hpp"

@interface OrcaMetalViewportView () {
    std::unique_ptr<Slic3r::portability::render::ios::IOSMetalRenderBackend> _backend;
    CADisplayLink *_displayLink;
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

- (void)setLookingDownward:(BOOL)lookingDownward
{
    if (_backend == nullptr)
        return;

    Slic3r::portability::render::RenderSceneState state;
    state.camera.is_looking_downward = lookingDownward;
    _backend->submit_scene_state(state);
}

- (void)renderFrame
{
    if (_backend != nullptr)
        _backend->render_frame();
}

@end
