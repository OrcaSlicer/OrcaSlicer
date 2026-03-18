#include "IOSMetalRenderBackend.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>

namespace Slic3r::portability::render::ios {

namespace {
constexpr MTLPixelFormat kPreferredPixelFormat = MTLPixelFormatBGRA8Unorm;
}

struct IOSMetalRenderBackend::MetalBackendState
{
    id<MTLDevice>       device = nil;
    id<MTLCommandQueue> command_queue = nil;
    CAMetalLayer       *metal_layer = nil;
};

IOSMetalRenderBackend::IOSMetalRenderBackend() : m_state(std::make_unique<MetalBackendState>()) {}

IOSMetalRenderBackend::~IOSMetalRenderBackend()
{
    shutdown();
}

std::string_view IOSMetalRenderBackend::backend_name() const
{
    return "ios-metal";
}

BackendType IOSMetalRenderBackend::backend_type() const
{
    return BackendType::Metal;
}

void IOSMetalRenderBackend::bind_metal_layer(CAMetalLayer *layer)
{
    if (m_state->metal_layer == layer)
        return;

    @autoreleasepool {
        if (m_state->metal_layer != nil)
            [m_state->metal_layer release];

        m_state->metal_layer = layer;
        if (m_state->metal_layer != nil) {
            [m_state->metal_layer retain];
            if (m_initialized)
                configure_layer_for_current_state(m_state->metal_layer);
        }
    }
}

bool IOSMetalRenderBackend::initialize()
{
    if (m_initialized)
        return true;

    @autoreleasepool {
        m_state->device = MTLCreateSystemDefaultDevice();
        if (m_state->device == nil)
            return false;

        if (![m_state->device supportsFeatureSet:MTLFeatureSet_iOS_GPUFamily1_v1])
            return false;

        m_state->command_queue = [m_state->device newCommandQueue];
        if (m_state->command_queue == nil)
            return false;

        if (m_state->metal_layer == nil)
            m_state->metal_layer = [[CAMetalLayer layer] retain];

        configure_layer_for_current_state(m_state->metal_layer);
    }

    m_initialized = true;
    return true;
}


void IOSMetalRenderBackend::configure_layer_for_current_state(CAMetalLayer *layer)
{
    if (layer == nil || m_state->device == nil)
        return;

    layer.device = m_state->device;
    layer.pixelFormat = kPreferredPixelFormat;
    layer.framebufferOnly = YES;
    layer.drawableSize = CGSizeMake(std::max(m_width, 1), std::max(m_height, 1));
}

void IOSMetalRenderBackend::resize(int width, int height)
{
    m_width = std::max(width, 0);
    m_height = std::max(height, 0);

    if (!m_initialized || m_state->metal_layer == nil)
        return;

    configure_layer_for_current_state(m_state->metal_layer);
}

void IOSMetalRenderBackend::render_frame()
{
    if (!m_initialized || m_state->metal_layer == nil || m_state->command_queue == nil)
        return;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [m_state->metal_layer nextDrawable];
        if (drawable == nil)
            return;

        MTLRenderPassDescriptor *render_pass_descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        render_pass_descriptor.colorAttachments[0].texture = drawable.texture;
        render_pass_descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        render_pass_descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        render_pass_descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.08, 0.12, 1.0);

        id<MTLCommandBuffer> command_buffer = [m_state->command_queue commandBuffer];
        if (command_buffer == nil)
            return;

        id<MTLRenderCommandEncoder> command_encoder = [command_buffer renderCommandEncoderWithDescriptor:render_pass_descriptor];
        if (command_encoder == nil)
            return;

        MTLViewport viewport = {
            .originX = 0.0,
            .originY = 0.0,
            .width = static_cast<double>(std::max(m_width, 1)),
            .height = static_cast<double>(std::max(m_height, 1)),
            .znear = 0.0,
            .zfar = 1.0,
        };
        [command_encoder setViewport:viewport];
        [command_encoder endEncoding];

        [command_buffer presentDrawable:drawable];
        [command_buffer commit];
    }
}

void IOSMetalRenderBackend::shutdown()
{
    if (!m_state)
        return;

    @autoreleasepool {
        m_state->command_queue = nil;
        m_state->device = nil;

        if (m_state->metal_layer != nil) {
            [m_state->metal_layer release];
        }

        m_state->metal_layer = nil;
    }

    m_initialized = false;
}

} // namespace Slic3r::portability::render::ios
