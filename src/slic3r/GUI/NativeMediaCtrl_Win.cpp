#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include "NativeMediaCtrl.h"
#include <boost/log/trivial.hpp>

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace Slic3r { namespace GUI {

class NativeMediaCtrl::Impl
{
public:
    Impl(NativeMediaCtrl* owner, HWND hwnd);
    ~Impl();

    bool Load(const wxString& url);
    void Play();
    void Stop();
    void Pause();

    NativeMediaState GetState() const { return m_state.load(); }
    wxSize GetVideoSize() const { return m_video_size; }
    NativeMediaError GetLastError() const { return m_error; }

    void UpdateLayout(int width, int height);

private:
    bool InitializeMediaFoundation();
    void ShutdownMediaFoundation();
    bool CreateMediaSource(const wxString& url);
    bool CreateSourceReader();
    bool CreateRenderingResources();
    bool CreateVideoTexture(UINT width, UINT height, DXGI_FORMAT format);
    void RenderVideoFrame(const BYTE* data, DWORD length);
    void RenderLoop();
    void CleanupResources();
    void NotifyStateChanged(NativeMediaState state);
    void NotifyError(NativeMediaError error);
    NativeMediaError MapHResultToError(HRESULT hr);

    NativeMediaCtrl* m_owner;
    HWND m_hwnd;
    std::atomic<NativeMediaState> m_state;
    NativeMediaError m_error;
    wxSize m_video_size;
    wxString m_url;

    ComPtr<IMFMediaSource> m_media_source;
    ComPtr<IMFSourceReader> m_source_reader;
    ComPtr<ID3D11Device> m_d3d_device;
    ComPtr<ID3D11DeviceContext> m_d3d_context;
    ComPtr<IDXGISwapChain1> m_swap_chain;
    ComPtr<ID3D11RenderTargetView> m_render_target;

    ComPtr<ID3D11Texture2D> m_video_texture;
    ComPtr<ID3D11ShaderResourceView> m_video_srv;
    ComPtr<ID3D11VertexShader> m_vertex_shader;
    ComPtr<ID3D11PixelShader> m_pixel_shader;
    ComPtr<ID3D11Buffer> m_vertex_buffer;
    ComPtr<ID3D11SamplerState> m_sampler_state;
    ComPtr<ID3D11InputLayout> m_input_layout;

    std::thread m_render_thread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_paused;
    std::mutex m_mutex;
    bool m_mf_initialized;

    int m_width;
    int m_height;
    int m_video_width;
    int m_video_height;
    GUID m_video_format;
};

NativeMediaCtrl::Impl::Impl(NativeMediaCtrl* owner, HWND hwnd)
    : m_owner(owner)
    , m_hwnd(hwnd)
    , m_state(NativeMediaState::Stopped)
    , m_error(NativeMediaError::None)
    , m_video_size(1920, 1080)
    , m_running(false)
    , m_paused(false)
    , m_mf_initialized(false)
    , m_width(640)
    , m_height(480)
    , m_video_width(0)
    , m_video_height(0)
    , m_video_format(GUID_NULL)
{
    m_mf_initialized = InitializeMediaFoundation();
    if (m_mf_initialized) {
        BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: Media Foundation initialized";
    } else {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Failed to initialize Media Foundation";
    }
}

NativeMediaCtrl::Impl::~Impl()
{
    Stop();
    ShutdownMediaFoundation();
}

bool NativeMediaCtrl::Impl::InitializeMediaFoundation()
{
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: MFStartup failed: " << std::hex << hr;
        return false;
    }

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &m_d3d_device,
        nullptr,
        &m_d3d_context
    );

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            feature_levels,
            ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            &m_d3d_device,
            nullptr,
            &m_d3d_context
        );
    }

    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: D3D11CreateDevice failed: " << std::hex << hr;
        return false;
    }

    ComPtr<IDXGIDevice1> dxgi_device;
    hr = m_d3d_device.As(&dxgi_device);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    RECT rect;
    GetClientRect(m_hwnd, &rect);
    m_width = std::max(1L, rect.right - rect.left);
    m_height = std::max(1L, rect.bottom - rect.top);

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.Width = m_width;
    swap_chain_desc.Height = m_height;
    swap_chain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    hr = factory->CreateSwapChainForHwnd(
        m_d3d_device.Get(),
        m_hwnd,
        &swap_chain_desc,
        nullptr,
        nullptr,
        &m_swap_chain
    );

    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: CreateSwapChain failed: " << std::hex << hr;
        return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) return false;

    hr = m_d3d_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &m_render_target);
    if (FAILED(hr)) return false;

    if (!CreateRenderingResources()) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Failed to create rendering resources";
        return false;
    }

    return true;
}

bool NativeMediaCtrl::Impl::CreateRenderingResources()
{
    const char* vertex_shader_code = R"(
        struct VS_OUTPUT {
            float4 position : SV_POSITION;
            float2 texcoord : TEXCOORD0;
        };

        VS_OUTPUT main(uint id : SV_VertexID) {
            VS_OUTPUT output;
            float2 positions[4] = {
                float2(-1.0, -1.0),
                float2(-1.0,  1.0),
                float2( 1.0, -1.0),
                float2( 1.0,  1.0)
            };
            float2 texcoords[4] = {
                float2(0.0, 1.0),
                float2(0.0, 0.0),
                float2(1.0, 1.0),
                float2(1.0, 0.0)
            };
            output.position = float4(positions[id], 0.0, 1.0);
            output.texcoord = texcoords[id];
            return output;
        }
    )";

    const char* pixel_shader_code = R"(
        Texture2D videoTexture : register(t0);
        SamplerState videoSampler : register(s0);

        struct PS_INPUT {
            float4 position : SV_POSITION;
            float2 texcoord : TEXCOORD0;
        };

        float4 main(PS_INPUT input) : SV_TARGET {
            float4 color = videoTexture.Sample(videoSampler, input.texcoord);
            return float4(color.bgr, color.a);
        }
    )";

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(
        vertex_shader_code,
        strlen(vertex_shader_code),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        0,
        0,
        &vs_blob,
        &error_blob
    );

    if (FAILED(hr)) {
        if (error_blob) {
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Vertex shader compilation failed: "
                                     << (char*)error_blob->GetBufferPointer();
        }
        return false;
    }

    hr = m_d3d_device->CreateVertexShader(
        vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(),
        nullptr,
        &m_vertex_shader
    );

    if (FAILED(hr)) return false;

    ComPtr<ID3DBlob> ps_blob;
    hr = D3DCompile(
        pixel_shader_code,
        strlen(pixel_shader_code),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "ps_5_0",
        0,
        0,
        &ps_blob,
        &error_blob
    );

    if (FAILED(hr)) {
        if (error_blob) {
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Pixel shader compilation failed: "
                                     << (char*)error_blob->GetBufferPointer();
        }
        return false;
    }

    hr = m_d3d_device->CreatePixelShader(
        ps_blob->GetBufferPointer(),
        ps_blob->GetBufferSize(),
        nullptr,
        &m_pixel_shader
    );

    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_d3d_device->CreateSamplerState(&sampler_desc, &m_sampler_state);
    if (FAILED(hr)) return false;

    return true;
}

bool NativeMediaCtrl::Impl::CreateVideoTexture(UINT width, UINT height, DXGI_FORMAT format)
{
    m_video_texture.Reset();
    m_video_srv.Reset();

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DYNAMIC;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_d3d_device->CreateTexture2D(&texture_desc, nullptr, &m_video_texture);
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Failed to create video texture: " << std::hex << hr;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    hr = m_d3d_device->CreateShaderResourceView(m_video_texture.Get(), &srv_desc, &m_video_srv);
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Failed to create shader resource view: " << std::hex << hr;
        return false;
    }

    m_video_width = width;
    m_video_height = height;

    return true;
}

void NativeMediaCtrl::Impl::RenderVideoFrame(const BYTE* data, DWORD length)
{
    if (!m_video_texture || !m_video_srv || !m_d3d_context || !m_render_target) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_d3d_context->Map(m_video_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Failed to map video texture: " << std::hex << hr;
        return;
    }

    UINT bytes_per_pixel = 4;
    UINT row_bytes = m_video_width * bytes_per_pixel;

    if (mapped.RowPitch == row_bytes) {
        memcpy(mapped.pData, data, std::min<DWORD>(length, row_bytes * m_video_height));
    } else {
        const BYTE* src = data;
        BYTE* dst = static_cast<BYTE*>(mapped.pData);
        for (UINT y = 0; y < m_video_height && src < data + length; ++y) {
            memcpy(dst, src, row_bytes);
            src += row_bytes;
            dst += mapped.RowPitch;
        }
    }

    m_d3d_context->Unmap(m_video_texture.Get(), 0);

    float clear_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_d3d_context->ClearRenderTargetView(m_render_target.Get(), clear_color);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_d3d_context->RSSetViewports(1, &viewport);

    m_d3d_context->OMSetRenderTargets(1, m_render_target.GetAddressOf(), nullptr);

    m_d3d_context->VSSetShader(m_vertex_shader.Get(), nullptr, 0);
    m_d3d_context->PSSetShader(m_pixel_shader.Get(), nullptr, 0);
    m_d3d_context->PSSetShaderResources(0, 1, m_video_srv.GetAddressOf());
    m_d3d_context->PSSetSamplers(0, 1, m_sampler_state.GetAddressOf());

    m_d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_d3d_context->Draw(4, 0);

    m_swap_chain->Present(1, 0);
}

void NativeMediaCtrl::Impl::ShutdownMediaFoundation()
{
    CleanupResources();

    m_video_srv.Reset();
    m_video_texture.Reset();
    m_sampler_state.Reset();
    m_input_layout.Reset();
    m_pixel_shader.Reset();
    m_vertex_shader.Reset();
    m_vertex_buffer.Reset();
    m_render_target.Reset();
    m_swap_chain.Reset();
    m_d3d_context.Reset();
    m_d3d_device.Reset();

    if (m_mf_initialized) {
        MFShutdown();
        m_mf_initialized = false;
    }
}

void NativeMediaCtrl::Impl::CleanupResources()
{
    m_running = false;

    if (m_render_thread.joinable()) {
        m_render_thread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_source_reader.Reset();
    m_media_source.Reset();
}

bool NativeMediaCtrl::Impl::Load(const wxString& url)
{
    CleanupResources();

    m_url = url;
    m_error = NativeMediaError::None;
    NotifyStateChanged(NativeMediaState::Loading);

    if (!m_mf_initialized) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Media Foundation not initialized";
        m_error = NativeMediaError::InternalError;
        NotifyStateChanged(NativeMediaState::Error);
        return false;
    }

    if (!CreateMediaSource(url)) {
        NotifyError(m_error);
        return false;
    }

    if (!CreateSourceReader()) {
        NotifyError(m_error);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: Loaded URL: " << url.ToStdString();
    return true;
}

bool NativeMediaCtrl::Impl::CreateMediaSource(const wxString& url)
{
    std::wstring wide_url = url.ToStdWstring();

    ComPtr<IMFSourceResolver> resolver;
    HRESULT hr = MFCreateSourceResolver(&resolver);
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: MFCreateSourceResolver failed: " << std::hex << hr;
        m_error = NativeMediaError::InternalError;
        return false;
    }

    MF_OBJECT_TYPE object_type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> source;

    hr = resolver->CreateObjectFromURL(
        wide_url.c_str(),
        MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ,
        nullptr,
        &object_type,
        &source
    );

    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: CreateObjectFromURL failed: " << std::hex << hr;
        m_error = MapHResultToError(hr);
        return false;
    }

    hr = source.As(&m_media_source);
    if (FAILED(hr)) {
        m_error = NativeMediaError::InternalError;
        return false;
    }

    return true;
}

bool NativeMediaCtrl::Impl::CreateSourceReader()
{
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 3);
    if (FAILED(hr)) return false;

    hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (FAILED(hr)) return false;

    hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(hr)) return false;

    if (m_d3d_device) {
        ComPtr<IMFDXGIDeviceManager> dxgi_manager;
        UINT reset_token = 0;
        hr = MFCreateDXGIDeviceManager(&reset_token, &dxgi_manager);
        if (SUCCEEDED(hr)) {
            hr = dxgi_manager->ResetDevice(m_d3d_device.Get(), reset_token);
            if (SUCCEEDED(hr)) {
                attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_manager.Get());
            }
        }
    }

    hr = MFCreateSourceReaderFromMediaSource(m_media_source.Get(), attributes.Get(), &m_source_reader);
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: MFCreateSourceReaderFromMediaSource failed: " << std::hex << hr;
        m_error = NativeMediaError::InternalError;
        return false;
    }

    ComPtr<IMFMediaType> video_type;
    hr = MFCreateMediaType(&video_type);
    if (FAILED(hr)) return false;

    video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

    hr = m_source_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, video_type.Get());
    if (FAILED(hr)) {
        BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: SetCurrentMediaType failed for RGB32: " << std::hex << hr;
        m_error = NativeMediaError::UnsupportedFormat;
        return false;
    }

    ComPtr<IMFMediaType> actual_type;
    hr = m_source_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual_type);
    if (SUCCEEDED(hr)) {
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(actual_type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (width > 0 && height > 0) {
            m_video_size = wxSize(width, height);
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: Video size: " << width << "x" << height;

            GUID subtype = GUID_NULL;
            actual_type->GetGUID(MF_MT_SUBTYPE, &subtype);
            m_video_format = subtype;

            if (!CreateVideoTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM)) {
                BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Failed to create video texture";
                return false;
            }
        }
    }

    return true;
}

void NativeMediaCtrl::Impl::Play()
{
    if (!m_source_reader) {
        BOOST_LOG_TRIVIAL(warning) << "NativeMediaCtrl_Win: Cannot play - no source reader";
        return;
    }

    m_paused = false;

    if (m_running) {
        NotifyStateChanged(NativeMediaState::Playing);
        return;
    }

    m_running = true;
    m_render_thread = std::thread(&Impl::RenderLoop, this);

    NotifyStateChanged(NativeMediaState::Playing);
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: Playback started";
}

void NativeMediaCtrl::Impl::Stop()
{
    m_running = false;
    m_paused = false;

    if (m_render_thread.joinable()) {
        m_render_thread.join();
    }

    NotifyStateChanged(NativeMediaState::Stopped);
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: Playback stopped";
}

void NativeMediaCtrl::Impl::Pause()
{
    m_paused = true;
    NotifyStateChanged(NativeMediaState::Paused);
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: Playback paused";
}

void NativeMediaCtrl::Impl::RenderLoop()
{
    while (m_running) {
        if (m_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        HRESULT hr;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_source_reader) break;

            hr = m_source_reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                nullptr,
                &flags,
                &timestamp,
                &sample
            );
        }

        if (FAILED(hr)) {
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: ReadSample failed: " << std::hex << hr;
            m_error = MapHResultToError(hr);
            NotifyError(m_error);
            m_owner->ScheduleRetry();
            break;
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl_Win: End of stream";
            NotifyStateChanged(NativeMediaState::Stopped);
            break;
        }

        if (flags & MF_SOURCE_READERF_ERROR) {
            BOOST_LOG_TRIVIAL(error) << "NativeMediaCtrl_Win: Stream error";
            m_error = NativeMediaError::DecoderError;
            NotifyError(m_error);
            m_owner->ScheduleRetry();
            break;
        }

        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->GetBufferByIndex(0, &buffer);
            if (SUCCEEDED(hr)) {
                BYTE* data = nullptr;
                DWORD length = 0;
                hr = buffer->Lock(&data, nullptr, &length);
                if (SUCCEEDED(hr) && data) {
                    RenderVideoFrame(data, length);
                    buffer->Unlock();
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void NativeMediaCtrl::Impl::UpdateLayout(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    m_width = width;
    m_height = height;

    if (m_swap_chain && m_d3d_device) {
        m_render_target.Reset();

        HRESULT hr = m_swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Texture2D> back_buffer;
            hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
            if (SUCCEEDED(hr)) {
                m_d3d_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &m_render_target);
            }
        }
    }
}

NativeMediaError NativeMediaCtrl::Impl::MapHResultToError(HRESULT hr)
{
    switch (hr) {
        case MF_E_NET_NOCONNECTION:
        case E_ACCESSDENIED:
            return NativeMediaError::NetworkUnreachable;
        case MF_E_NET_TIMEOUT:
            return NativeMediaError::ConnectionTimeout;
        case MF_E_UNSUPPORTED_FORMAT:
        case MF_E_INVALIDMEDIATYPE:
            return NativeMediaError::UnsupportedFormat;
        case MF_E_SOURCERESOLVER_MUTUALLY_EXCLUSIVE_FLAGS:
        case MF_E_UNSUPPORTED_SCHEME:
            return NativeMediaError::StreamNotFound;
        default:
            return NativeMediaError::InternalError;
    }
}

void NativeMediaCtrl::Impl::NotifyStateChanged(NativeMediaState state)
{
    m_state = state;
    wxCommandEvent event(EVT_NATIVE_MEDIA_STATE_CHANGED);
    event.SetEventObject(m_owner);
    event.SetInt(static_cast<int>(state));
    wxPostEvent(m_owner, event);
}

void NativeMediaCtrl::Impl::NotifyError(NativeMediaError error)
{
    m_state = NativeMediaState::Error;
    wxCommandEvent event(EVT_NATIVE_MEDIA_ERROR);
    event.SetEventObject(m_owner);
    event.SetInt(static_cast<int>(error));
    wxPostEvent(m_owner, event);
}

NativeMediaCtrl::NativeMediaCtrl(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_retry_enabled(true)
    , m_retry_count(0)
{
    SetBackgroundColour(*wxBLACK);
    m_retry_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &NativeMediaCtrl::OnRetryTimer, this, m_retry_timer.GetId());

    HWND hwnd = (HWND)GetHandle();
    m_impl = std::make_unique<Impl>(this, hwnd);
}

NativeMediaCtrl::~NativeMediaCtrl()
{
    m_retry_timer.Stop();
}

bool NativeMediaCtrl::Load(const wxString& url)
{
    if (!IsSupported(url)) {
        BOOST_LOG_TRIVIAL(warning) << "NativeMediaCtrl: Unsupported URL format: " << url.ToStdString();
        return false;
    }

    m_current_url = url;
    ResetRetryState();

    StreamType type = DetectStreamType(url);
    BOOST_LOG_TRIVIAL(info) << "NativeMediaCtrl: Loading " << StreamTypeToString(type).ToStdString()
                            << " stream: " << url.ToStdString();

    return m_impl->Load(url);
}

void NativeMediaCtrl::Play()
{
    if (m_impl) {
        m_impl->Play();
    }
}

void NativeMediaCtrl::Stop()
{
    m_retry_timer.Stop();
    ResetRetryState();
    if (m_impl) {
        m_impl->Stop();
    }
}

void NativeMediaCtrl::Pause()
{
    if (m_impl) {
        m_impl->Pause();
    }
}

NativeMediaState NativeMediaCtrl::GetState() const
{
    return m_impl ? m_impl->GetState() : NativeMediaState::Stopped;
}

wxSize NativeMediaCtrl::GetVideoSize() const
{
    return m_impl ? m_impl->GetVideoSize() : wxSize(1920, 1080);
}

NativeMediaError NativeMediaCtrl::GetLastError() const
{
    return m_impl ? m_impl->GetLastError() : NativeMediaError::None;
}

void NativeMediaCtrl::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (m_impl && width > 0 && height > 0) {
        m_impl->UpdateLayout(width, height);
    }
}

}} // namespace Slic3r::GUI

#endif // _WIN32
