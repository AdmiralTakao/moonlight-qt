// For D3D11_DECODER_PROFILE values
#include <initguid.h>

#include "d3d11va.h"
#include "dxutil.h"
#include "path.h"
#include "utils.h"

#include "streaming/streamutils.h"
#include "streaming/session.h"

#include <SDL_syswm.h>

#include <dwmapi.h>

using Microsoft::WRL::ComPtr;

// Standard DXVA GUIDs for HEVC RExt profiles (redefined for compatibility with pre-24H2 SDKs)
DEFINE_GUID(k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN_444,   0x4008018f, 0xf537, 0x4b36, 0x98, 0xcf, 0x61, 0xaf, 0x8a, 0x2c, 0x1a, 0x33);
DEFINE_GUID(k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10_444, 0x0dabeffa, 0x4458, 0x4602, 0xbc, 0x03, 0x07, 0x95, 0x65, 0x9d, 0x61, 0x7c);

typedef struct _VERTEX
{
    float x, y;
    float tu, tv;
} VERTEX, *PVERTEX;

#define CSC_MATRIX_RAW_ELEMENT_COUNT 9
#define CSC_MATRIX_PACKED_ELEMENT_COUNT 12
#define OFFSETS_ELEMENT_COUNT 3

typedef struct _CSC_CONST_BUF
{
    // CscMatrix value from above but packed and scaled
    float cscMatrix[CSC_MATRIX_PACKED_ELEMENT_COUNT];

    // YUV offset values
    float offsets[OFFSETS_ELEMENT_COUNT];

    // Padding float to end 16-byte boundary
    float padding;

    // Chroma offset values
    float chromaOffset[2];

    // Max UV coordinates to avoid sampling alignment padding
    float chromaUVMax[2];

    // GFN fork: plane sizes in texels for the chroma reconstruction shader
    // (luma width, luma height, chroma width, chroma height)
    float planeSizes[4];

    // GFN fork: chroma reconstruction parameters (x = strength)
    float cflParams[4];
} CSC_CONST_BUF, *PCSC_CONST_BUF;
static_assert(sizeof(CSC_CONST_BUF) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

// GFN fork: AMD FSR 1 EASU constants (see FsrEasuCon() in ffx_fsr1.h)
typedef struct _EASU_CONST_BUF
{
    float con0[4];
    float con1[4];
    float con2[4];
    float con3[4];
} EASU_CONST_BUF;
static_assert(sizeof(EASU_CONST_BUF) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

// GFN fork: AMD FSR 1 RCAS constants
typedef struct _RCAS_CONST_BUF
{
    // x = sharpening amount (0..1)
    float params[4];

    // xy = top-left of the video rectangle in the render target, zw = input texture size
    float rect[4];
} RCAS_CONST_BUF;
static_assert(sizeof(RCAS_CONST_BUF) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

// GFN fork: vendor extensions for driver-based video super resolution
// NVIDIA RTX Video Super Resolution (stream extension)
static const GUID k_NvidiaPPEInterfaceGUID =
    { 0xd43ce1b3, 0x1f4b, 0x48ac, { 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 } };

// Intel Video Super Resolution (Intel VPE interface)
static const GUID k_IntelVpeInterfaceGUID =
    { 0xedd1d4b9, 0x8659, 0x4cbc, { 0xa4, 0xd6, 0x98, 0x97, 0xa9, 0x64, 0x91, 0x9a } };
#define INTEL_VPE_FN_VERSION 0x01
#define INTEL_VPE_FN_MODE 0x20
#define INTEL_VPE_FN_SCALING 0x37
#define INTEL_VPE_VERSION_3 0x0003
#define INTEL_VPE_MODE_PREPROC 0x01
#define INTEL_VPE_SCALING_SUPER_RESOLUTION 0x2

typedef struct _INTEL_VPE_EXT
{
    UINT function;
    void* param;
} INTEL_VPE_EXT;

#define PCI_VENDOR_NVIDIA 0x10DE
#define PCI_VENDOR_INTEL 0x8086

static const std::array<const char*, D3D11VARenderer::PixelShaders::_COUNT> k_VideoShaderNames =
{
    "d3d11_yuv420_pixel.fxc",
    "d3d11_ayuv_pixel.fxc",
    "d3d11_y410_pixel.fxc",
};

D3D11VARenderer::D3D11VARenderer(int decoderSelectionPass)
    : IFFmpegRenderer(RendererType::D3D11VA),
      m_DecoderSelectionPass(decoderSelectionPass),
      m_DevicesWithFL11Support(0),
      m_DevicesWithCodecSupport(0),
      m_SwapChainColorTrc(AVCOL_TRC_UNSPECIFIED),
      m_AllowTearing(false),
      m_OverlayLock(0),
      m_HwDeviceContext(nullptr),
      m_PpMode(PostProcessMode::None),
      m_PpDirty(true),
      m_PpDisabled(false),
      m_PpFrameWidth(0),
      m_PpFrameHeight(0),
      m_PpDstRect(),
      m_VideoDstRect(),
      m_VsrState(VsrState::Untested),
      m_AdapterVendorId(0),
      m_RaisedProcessPriority(false)
{
    m_ContextLock = SDL_CreateMutex();

    DwmEnableMMCSS(TRUE);
}

D3D11VARenderer::~D3D11VARenderer()
{
    DwmEnableMMCSS(FALSE);

    // GFN fork: restore the process priority if low-latency mode raised it
    if (m_RaisedProcessPriority) {
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    }

    SDL_DestroyMutex(m_ContextLock);

    // GFN fork: release post-processing resources
    releasePostProcessingTargets();
    m_CflPixelShader.Reset();
    m_EasuPixelShader.Reset();
    m_RcasPixelShader.Reset();
    m_CscConstantBuffer.Reset();
    m_VideoContext.Reset();
    m_VideoDevice.Reset();

    m_VideoVertexBuffer.Reset();
    for (auto& shader : m_VideoPixelShaders) {
        shader.Reset();
    }

    for (auto& textureSrvs : m_VideoTextureResourceViews) {
        for (auto& srv : textureSrvs) {
            srv.Reset();
        }
    }

    m_VideoTexture.Reset();

    for (auto& buffer : m_OverlayVertexBuffers) {
        buffer.Reset();
    }

    for (auto& srv : m_OverlayTextureResourceViews) {
        srv.Reset();
    }

    for (auto& texture : m_OverlayTextures) {
        texture.Reset();
    }

    m_OverlayPixelShader.Reset();

    m_OverlayBlendState.Reset();
    m_VideoBlendState.Reset();

    m_DecodeD2RFence.Reset();
    m_DecodeR2DFence.Reset();
    m_RenderD2RFence.Reset();
    m_RenderR2DFence.Reset();

    m_RenderTargetView.Reset();
    m_SwapChain.Reset();

    m_RenderSharedTextureArray.Reset();

    av_buffer_unref(&m_HwDeviceContext);
    m_DecodeDevice.Reset();
    m_DecodeDeviceContext.Reset();

    // Force destruction of the swapchain immediately
    if (m_RenderDeviceContext != nullptr) {
        m_RenderDeviceContext->ClearState();
        m_RenderDeviceContext->Flush();
    }

    m_RenderDevice.Reset();
    m_RenderDeviceContext.Reset();
    m_Factory.Reset();
}

bool D3D11VARenderer::createSharedFencePair(UINT64 initialValue, ID3D11Device5* dev1, ID3D11Device5* dev2, ComPtr<ID3D11Fence>& dev1Fence, ComPtr<ID3D11Fence>& dev2Fence)
{
    HRESULT hr;
    D3D11_FENCE_FLAG flags;

    flags = D3D11_FENCE_FLAG_SHARED;
    if (m_FenceType == SupportedFenceType::NonMonitored) {
        flags |= D3D11_FENCE_FLAG_NON_MONITORED;
    }

    hr = dev1->CreateFence(initialValue, flags, IID_PPV_ARGS(&dev1Fence));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device5::CreateFence() failed: %x",
                     hr);
        return false;
    }

    HANDLE fenceHandle;
    hr = dev1Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Fence::CreateSharedHandle() failed: %x",
                     hr);
        dev1Fence.Reset();
        return false;
    }

    hr = dev2->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&dev2Fence));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device5::OpenSharedFence() failed: %x",
                     hr);
        dev1Fence.Reset();
        return false;
    }

    return true;
}

bool D3D11VARenderer::setupSharedDevice(IDXGIAdapter1* adapter)
{
    const D3D_FEATURE_LEVEL supportedFeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    bool success = false;

    // We don't support cross-device sharing without fences
    if (m_FenceType == SupportedFenceType::None) {
        return false;
    }

    // If we're going to use separate devices for decoding and rendering, create the decoding device
    hr = D3D11CreateDevice(adapter,
                           D3D_DRIVER_TYPE_UNKNOWN,
                           nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT
                               | (m_DebugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0),
                           supportedFeatureLevels,
                           ARRAYSIZE(supportedFeatureLevels),
                           D3D11_SDK_VERSION,
                           &device,
                           &featureLevel,
                           &deviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11CreateDevice() failed: %x",
                     hr);
        return false;
    }

    hr = device.As(&m_DecodeDevice);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::QueryInterface(ID3D11Device1) failed: %x",
                     hr);
        goto Exit;
    }

    hr = deviceContext.As(&m_DecodeDeviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext1) failed: %x",
                     hr);
        goto Exit;
    }

    // Create our decode->render fence
    m_D2RFenceValue = 1;
    if (!createSharedFencePair(0, m_DecodeDevice.Get(), m_RenderDevice.Get(), m_DecodeD2RFence, m_RenderD2RFence)) {
        goto Exit;
    }

    // Create our render->decode fence
    m_R2DFenceValue = 1;
    if (!createSharedFencePair(0, m_DecodeDevice.Get(), m_RenderDevice.Get(), m_DecodeR2DFence, m_RenderR2DFence)) {
        goto Exit;
    }

    success = true;
Exit:
    if (!success) {
        m_DecodeD2RFence.Reset();
        m_RenderD2RFence.Reset();
        m_DecodeR2DFence.Reset();
        m_RenderR2DFence.Reset();
        m_DecodeDevice.Reset();
    }

    return success;
}

bool D3D11VARenderer::createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound)
{
    const D3D_FEATURE_LEVEL supportedFeatureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    bool success = false;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapterDesc;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    LARGE_INTEGER umdVersion;

    SDL_assert(!m_RenderDevice);
    SDL_assert(!m_RenderDeviceContext);
    SDL_assert(!m_DecodeDevice);
    SDL_assert(!m_DecodeDeviceContext);

    hr = m_Factory->EnumAdapters1(adapterIndex, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) {
        // Expected at the end of enumeration
        goto Exit;
    }
    else if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::EnumAdapters1() failed: %x",
                     hr);
        goto Exit;
    }

    hr = adapter->GetDesc1(&adapterDesc);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIAdapter::GetDesc() failed: %x",
                     hr);
        goto Exit;
    }

    if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        // Skip the WARP device. We know it will fail.
        goto Exit;
    }

    // Query the GPU driver version
    hr = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umdVersion);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIAdapter::CheckInterfaceSupport() failed: %x",
                     hr);
        goto Exit;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Detected GPU %d: %S (%x:%x) (driver: %u.%u.%u.%u)",
                adapterIndex,
                adapterDesc.Description,
                adapterDesc.VendorId,
                adapterDesc.DeviceId,
                HIWORD(umdVersion.HighPart),
                LOWORD(umdVersion.HighPart),
                HIWORD(umdVersion.LowPart),
                LOWORD(umdVersion.LowPart));

    hr = D3D11CreateDevice(adapter.Get(),
                           D3D_DRIVER_TYPE_UNKNOWN,
                           nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT
                               | (m_DebugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0),
                           supportedFeatureLevels,
                           ARRAYSIZE(supportedFeatureLevels),
                           D3D11_SDK_VERSION,
                           &device,
                           &featureLevel,
                           &deviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11CreateDevice() failed: %x",
                     hr);
        goto Exit;
    }
    else if (adapterDesc.VendorId == 0x8086 && featureLevel <= D3D_FEATURE_LEVEL_11_0 && !qEnvironmentVariableIntValue("D3D11VA_ENABLED")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Avoiding D3D11VA on old pre-FL11.1 Intel GPU. Set D3D11VA_ENABLED=1 to override.");
        goto Exit;
    }
    else if (featureLevel >= D3D_FEATURE_LEVEL_11_0) {
        // Remember that we found a non-software D3D11 devices with support for
        // feature level 11.0 or later (Fermi, Terascale 2, or Ivy Bridge and later)
        m_DevicesWithFL11Support++;
    }

    hr = device.As(&m_RenderDevice);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::QueryInterface(ID3D11Device1) failed: %x",
                     hr);
        goto Exit;
    }

    hr = deviceContext.As(&m_RenderDeviceContext);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext1) failed: %x",
                     hr);
        goto Exit;
    }

    // Check which fence types are supported by this GPU
    {
        m_FenceType = SupportedFenceType::None;

        ComPtr<IDXGIAdapter4> adapter4;
        if (SUCCEEDED(adapter.As(&adapter4))) {
            DXGI_ADAPTER_DESC3 desc3;
            if (SUCCEEDED(adapter4->GetDesc3(&desc3))) {
                if (desc3.Flags & DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES) {
                    // Monitored fences must be used when they are supported
                    m_FenceType = SupportedFenceType::Monitored;
                }
                else if (desc3.Flags & DXGI_ADAPTER_FLAG3_SUPPORT_NON_MONITORED_FENCES) {
                    // Non-monitored fences must only be used when monitored fences are unsupported
                    m_FenceType = SupportedFenceType::NonMonitored;
                }
            }
        }
    }

    bool separateDevices;
    if (Utils::getEnvironmentVariableOverride("D3D11VA_FORCE_SEPARATE_DEVICES", &separateDevices)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using D3D11VA_FORCE_SEPARATE_DEVICES to override default logic");
    }
    else {
        D3D11_FEATURE_DATA_D3D11_OPTIONS d3d11Options;

        // Check if cross-device sharing works for YUV textures and fences are supported
        hr = m_RenderDevice->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &d3d11Options, sizeof(d3d11Options));
        separateDevices = SUCCEEDED(hr) && d3d11Options.ExtendedResourceSharing && m_FenceType != SupportedFenceType::None;

        if (separateDevices) {
            // Use minimum precision support to differentiate Vega and later from Polaris and earlier
            D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT minPrecSupport;
            hr = m_RenderDevice->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &minPrecSupport, sizeof(minPrecSupport));
            if (FAILED(hr)) {
                minPrecSupport = {};
            }

            // This texture array sharing codepath is quite prone to driver bugs.
            //
            // Broken GPU vendors/cards/drivers include:
            // - Moore Threads (texture is all zero/green)
            // - Qualcomm (decoding is unstable/slow on QC710)
            // - AMD prior to Vega (Polaris cards display corrupt output - see #2003,
            //                      HD 5570 drivers deadlock with shared texture arrays)
            // - Nvidia drivers prior to ~471.11 (Earlier drivers display all zero/green,
            //                                    We approximate by requiring WDDM 3.0+)
            //
            // Due to all these issues, we will only use this path for Intel/AMD and NVIDIA where we know it
            // provides tangible benefits (performance for the former and VRR support for the latter).
            separateDevices = adapterDesc.VendorId == 0x8086 || // Intel
                              (adapterDesc.VendorId == 0x10DE && HIWORD(umdVersion.HighPart) >= 30) || // NVIDIA WDDM 3.0+ (PCI ID)
                              adapterDesc.VendorId == 'ADVN' || // NVIDIA (WoA)
                              (adapterDesc.VendorId == 0x1002 && (minPrecSupport.PixelShaderMinPrecision & D3D11_SHADER_MIN_PRECISION_16_BIT)); // AMD Vega+
        }
    }

    // If we're going to use separate devices for decoding and rendering, create the decoding device
    if (!separateDevices || !setupSharedDevice(adapter.Get())) {
        m_DecodeDevice = m_RenderDevice;
        m_DecodeDeviceContext = m_RenderDeviceContext;
        separateDevices = false;
    }

    if (Utils::getEnvironmentVariableOverride("D3D11VA_FORCE_BIND", &m_BindDecoderOutputTextures)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using D3D11VA_FORCE_BIND to override default bind/copy logic");
    }
    else {
        // Skip copying to our own internal texture on Intel GPUs due to
        // significant performance impact of the extra copy. See:
        // https://github.com/moonlight-stream/moonlight-qt/issues/1304
        //
        // Also bind SRVs as this improves render times by about 2x on
        // my Ryzen 3300U system.
        m_BindDecoderOutputTextures = adapterDesc.VendorId == 0x8086 ||
                                      separateDevices;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Decoder texture access: %s (fence: %s)",
                m_BindDecoderOutputTextures ? "bind" : "copy",
                 m_FenceType == SupportedFenceType::Monitored ? "monitored" :
                    (m_FenceType == SupportedFenceType::NonMonitored ? "non-monitored" : "unsupported"));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using %s device for decoding and rendering",
                separateDevices ? "separate" : "shared");

    if (!checkDecoderSupport(adapter.Get())) {
        goto Exit;
    }
    else {
        // Remember that we found a device with support for decoding this codec
        m_DevicesWithCodecSupport++;
    }

    // GFN fork: remember the GPU vendor for driver-based super resolution
    m_AdapterVendorId = adapterDesc.VendorId;

    success = true;

Exit:
    if (adapterNotFound != nullptr) {
        *adapterNotFound = !adapter;
    }
    if (!success) {
        m_RenderDeviceContext.Reset();
        m_RenderDevice.Reset();
        m_DecodeDeviceContext.Reset();
        m_DecodeDevice.Reset();
    }
    return success;
}

bool D3D11VARenderer::initialize(PDECODER_PARAMETERS params)
{
    int outputIndex;
    HRESULT hr;

    m_DecoderParams = *params;

    if (qgetenv("D3D11VA_ENABLED") == "0") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11VA is disabled by environment variable");
        return false;
    }

    if (Utils::getEnvironmentVariableOverride("D3D11VA_DEBUG_LAYER", &m_DebugLayer)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using D3D11VA_DEBUG_LAYER to override default debug layer behavior");
    }
    else {
#ifdef QT_DEBUG
        m_DebugLayer = true;
#else
        m_DebugLayer = false;
#endif
    }

    // Check if Graphics Tools are installed
    if (m_DebugLayer) {
        HMODULE dxgiDebug = LoadLibraryExW(L"DXGIDebug.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dxgiDebug) {
            FreeLibrary(dxgiDebug);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "DXGI/D3D11 debug layer unavailable. Enable 'Graphics Tools' optional feature!");
            m_DebugLayer = false;
        }
    }

    if (!SDL_DXGIGetOutputInfo(SDL_GetWindowDisplayIndex(params->window),
                               &m_AdapterIndex, &outputIndex)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_DXGIGetOutputInfo() failed: %s",
                     SDL_GetError());
        return false;
    }

    hr = CreateDXGIFactory2(
        m_DebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0,
        __uuidof(IDXGIFactory5),
        (void**)&m_Factory);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateDXGIFactory() failed: %x",
                     hr);
        return false;
    }

    // First try the adapter corresponding to the display where our window resides.
    // This will let us avoid a copy if the display GPU has the required decoder.
    if (!createDeviceByAdapterIndex(m_AdapterIndex)) {
        // If that didn't work, we'll try all GPUs in order until we find one
        // or run out of GPUs (DXGI_ERROR_NOT_FOUND from EnumAdapters())
        bool adapterNotFound = false;
        for (int i = 0; !adapterNotFound; i++) {
            if (i == m_AdapterIndex) {
                // Don't try the same GPU again
                continue;
            }

            if (createDeviceByAdapterIndex(i, &adapterNotFound)) {
                // This GPU worked! Continue initialization.
                break;
            }
        }

        if (adapterNotFound) {
            SDL_assert(!m_RenderDevice);
            SDL_assert(!m_RenderDeviceContext);
            return false;
        }
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = 0;

    // 3 front buffers (default GetMaximumFrameLatency() count)
    // + 1 back buffer
    // + 1 extra for DWM to hold on to for DirectFlip
    //
    // Even though we allocate 3 front buffers for pre-rendered frames,
    // they won't actually increase presentation latency because we
    // always use SyncInterval 0 which replaces the last one.
    //
    // IDXGIDevice1 has a SetMaximumFrameLatency() function, but counter-
    // intuitively we must avoid it to reduce latency. If we set our max
    // frame latency to 1 on thedevice, our SyncInterval 0 Present() calls
    // will block on DWM (acting like SyncInterval 1) rather than doing
    // the non-blocking present we expect.
    //
    // NB: 3 total buffers seems sufficient on NVIDIA hardware but
    // causes performance issues (buffer starvation) on AMD GPUs.
    swapChainDesc.BufferCount = 3 + 1 + 1;

    // Use the current window size as the swapchain size
    SDL_GetWindowSize(params->window, (int*)&swapChainDesc.Width, (int*)&swapChainDesc.Height);

    m_DisplayWidth = swapChainDesc.Width;
    m_DisplayHeight = swapChainDesc.Height;

    if (params->videoFormat & VIDEO_FORMAT_MASK_10BIT) {
        swapChainDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    else {
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // Use DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING with flip mode for non-vsync case, if possible.
    // NOTE: This is only possible in windowed or borderless windowed mode.
    if (!params->enableVsync) {
        BOOL allowTearing = FALSE;
        hr = m_Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                            &allowTearing,
                                            sizeof(allowTearing));
        if (SUCCEEDED(hr)) {
            if (allowTearing) {
                // Use flip discard with allow tearing mode if possible.
                swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
                m_AllowTearing = true;
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "OS/GPU doesn't support DXGI_FEATURE_PRESENT_ALLOW_TEARING");
            }
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGIFactory::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) failed: %x",
                         hr);
            // Non-fatal
        }

        // DXVA2 may let us take over for FSE V-sync off cases. However, if we don't have DXGI_FEATURE_PRESENT_ALLOW_TEARING
        // then we should not attempt to do this unless there's no other option (HDR, DXVA2 failed in pass 1, etc).
        if (!m_AllowTearing && m_DecoderSelectionPass == 0 && !(params->videoFormat & VIDEO_FORMAT_MASK_10BIT) &&
                (SDL_GetWindowFlags(params->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Defaulting to DXVA2 for FSE without DXGI_FEATURE_PRESENT_ALLOW_TEARING support");
            return false;
        }
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(params->window, &info);
    SDL_assert(info.subsystem == SDL_SYSWM_WINDOWS);

    // Always use windowed or borderless windowed mode.. SDL does mode-setting for us in
    // full-screen exclusive mode (SDL_WINDOW_FULLSCREEN), so this actually works out okay.
    ComPtr<IDXGISwapChain1> swapChain;
    hr = m_Factory->CreateSwapChainForHwnd(m_RenderDevice.Get(),
                                           info.info.win.window,
                                           &swapChainDesc,
                                           nullptr,
                                           nullptr,
                                           &swapChain);

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::CreateSwapChainForHwnd() failed: %x",
                     hr);
        return false;
    }

    hr = swapChain.As(&m_SwapChain);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::QueryInterface(IDXGISwapChain4) failed: %x",
                     hr);
        return false;
    }

    // Disable Alt+Enter, PrintScreen, and window message snooping. This makes
    // it safe to run the renderer on a separate rendering thread rather than
    // requiring the main (message loop) thread.
    hr = m_Factory->MakeWindowAssociation(info.info.win.window, DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIFactory::MakeWindowAssociation() failed: %x",
                     hr);
        return false;
    }

    {
        m_HwDeviceContext = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!m_HwDeviceContext) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to allocate D3D11VA device context");
            return false;
        }

        AVHWDeviceContext* deviceContext = (AVHWDeviceContext*)m_HwDeviceContext->data;
        AVD3D11VADeviceContext* d3d11vaDeviceContext = (AVD3D11VADeviceContext*)deviceContext->hwctx;

        // FFmpeg will take ownership of these pointers, so we use CopyTo() to bump the ref count
        m_DecodeDevice.CopyTo(&d3d11vaDeviceContext->device);
        m_DecodeDeviceContext.CopyTo(&d3d11vaDeviceContext->device_context);

        // Set lock functions that we will use to synchronize with FFmpeg's usage of our device context
        d3d11vaDeviceContext->lock = lockContext;
        d3d11vaDeviceContext->unlock = unlockContext;
        d3d11vaDeviceContext->lock_ctx = this;

        int err = av_hwdevice_ctx_init(m_HwDeviceContext);
        if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to initialize D3D11VA device context: %d",
                         err);
            return false;
        }
    }

    if (!setupRenderingResources()) {
        return false;
    }

    // GFN fork: optional post-processing shaders and low-latency tweaks.
    // Failures here are not fatal, we just fall back to the original behavior.
    if (!params->testOnly) {
        setupPostProcessingShaders();
        applyLowLatencyTweaks();
    }

    return true;
}

bool D3D11VARenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary**)
{
    context->hw_device_ctx = av_buffer_ref(m_HwDeviceContext);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using D3D11VA accelerated renderer");

    return true;
}

bool D3D11VARenderer::prepareDecoderContextInGetFormat(AVCodecContext *context, AVPixelFormat pixelFormat)
{
    // Create a new hardware frames context suitable for decoding our specified format
    av_buffer_unref(&context->hw_frames_ctx);
    int err = avcodec_get_hw_frames_parameters(context, m_HwDeviceContext, pixelFormat, &context->hw_frames_ctx);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to get hwframes context parameters: %d",
                     err);
        return false;
    }

    auto framesContext = (AVHWFramesContext*)context->hw_frames_ctx->data;
    auto d3d11vaFramesContext = (AVD3D11VAFramesContext*)framesContext->hwctx;

    // If we're binding output textures directly, we need to add the SRV bind flag
    if (m_BindDecoderOutputTextures) {
        d3d11vaFramesContext->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }

    // If we're using separate decode and render devices, we need to create shared textures
    if (m_DecodeDevice != m_RenderDevice) {
        d3d11vaFramesContext->MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    }

    // Mimic the logic in ff_decode_get_hw_frames_ctx() which adds an extra 3 frames
    if (framesContext->initial_pool_size) {
        framesContext->initial_pool_size += 3;
    }

    err = av_hwframe_ctx_init(context->hw_frames_ctx);
    if (err < 0) {
        av_buffer_unref(&context->hw_frames_ctx);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed initialize hwframes context: %d",
                     err);
        return false;
    }

    if (!setupFrameRenderingResources(framesContext)) {
        av_buffer_unref(&context->hw_frames_ctx);
        return false;
    }

    return true;
}

void D3D11VARenderer::renderFrame(AVFrame* frame)
{
    // Acquire the context lock for rendering to prevent concurrent
    // access from inside FFmpeg's decoding code
    if (m_DecodeDevice == m_RenderDevice) {
        lockContext(this);
    }

    // Clear the back buffer
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_RenderDeviceContext->ClearRenderTargetView(m_RenderTargetView.Get(), clearColor);

    // Bind the back buffer. This needs to be done each time,
    // because the render target view will be unbound by Present().
    m_RenderDeviceContext->OMSetRenderTargets(1, m_RenderTargetView.GetAddressOf(), nullptr);

    // Render our video frame with the aspect-ratio adjusted viewport
    renderVideo(frame);

    // Render overlays on top of the video stream
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        renderOverlay((Overlay::OverlayType)i);
    }

    UINT flags;

    if (m_AllowTearing) {
        SDL_assert(!m_DecoderParams.enableVsync);

        // If tearing is allowed, use DXGI_PRESENT_ALLOW_TEARING with syncInterval 0.
        // It is not valid to use any other syncInterval values in tearing mode.
        flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    else {
        // Otherwise, we'll submit as fast as possible and DWM will discard excess
        // frames for us. If frame pacing is also enabled or we're in full-screen,
        // our Vsync source will keep us in sync with VBlank.
        flags = 0;
    }

    HRESULT hr;

    if (frame->color_trc != m_SwapChainColorTrc) {
        if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
            // Switch to Rec 2020 PQ (SMPTE ST 2084) colorspace for HDR10 rendering
            hr = m_SwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) failed: %x",
                             hr);
            }
        }
        else {
            // Restore default sRGB colorspace
            hr = m_SwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) failed: %x",
                             hr);
            }
        }

        m_SwapChainColorTrc = frame->color_trc;
    }

    // Present according to the decoder parameters
    hr = m_SwapChain->Present(0, flags);

    if (m_DecodeDevice == m_RenderDevice) {
        // Release the context lock
        unlockContext(this);
    }

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::Present() failed: %x",
                     hr);

        // The card may have been removed or crashed. Reset the decoder.
        SDL_Event event;
        event.type = SDL_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
        return;
    }
}

void D3D11VARenderer::renderOverlay(Overlay::OverlayType type)
{
    if (!Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        return;
    }

    // If the overlay is being updated, just skip rendering it this frame
    if (!SDL_AtomicTryLock(&m_OverlayLock)) {
        return;
    }

    // Reference these objects so they don't immediately go away if the
    // overlay update thread tries to release them.
    ComPtr<ID3D11Texture2D> overlayTexture = m_OverlayTextures[type];
    ComPtr<ID3D11Buffer> overlayVertexBuffer = m_OverlayVertexBuffers[type];
    ComPtr<ID3D11ShaderResourceView> overlayTextureResourceView = m_OverlayTextureResourceViews[type];
    SDL_AtomicUnlock(&m_OverlayLock);

    if (!overlayTexture) {
        return;
    }

    // If there was a texture, there must also be a vertex buffer and SRV
    SDL_assert(overlayVertexBuffer);
    SDL_assert(overlayTextureResourceView);

    // Bind vertex buffer
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    m_RenderDeviceContext->IASetVertexBuffers(0, 1, overlayVertexBuffer.GetAddressOf(), &stride, &offset);

    // Bind pixel shader and resources
    m_RenderDeviceContext->PSSetShader(m_OverlayPixelShader.Get(), nullptr, 0);
    m_RenderDeviceContext->PSSetShaderResources(0, 1, overlayTextureResourceView.GetAddressOf());

    // Draw the overlay with alpha blending
    m_RenderDeviceContext->OMSetBlendState(m_OverlayBlendState.Get(), nullptr, 0xffffffff);
    m_RenderDeviceContext->DrawIndexed(6, 0, 0);
    m_RenderDeviceContext->OMSetBlendState(m_VideoBlendState.Get(), nullptr, 0xffffffff);
}

void D3D11VARenderer::bindVideoVertexBuffer(bool frameChanged, AVFrame* frame)
{
    if (frameChanged || !m_VideoVertexBuffer) {
        // Scale video to the window size while preserving aspect ratio
        SDL_Rect src, dst;
        src.x = src.y = 0;
        src.w = frame->width;
        src.h = frame->height;
        dst.x = dst.y = 0;
        dst.w = m_DisplayWidth;
        dst.h = m_DisplayHeight;
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // GFN fork: remember where the video is drawn for post-processing
        m_VideoDstRect = dst;

        // Convert screen space to normalized device coordinates
        SDL_FRect renderRect;
        StreamUtils::screenSpaceToNormalizedDeviceCoords(&dst, &renderRect, m_DisplayWidth, m_DisplayHeight);

        // Don't sample from the alignment padding area
        auto framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;
        float uMax = (float)frame->width / framesContext->width;
        float vMax = (float)frame->height / framesContext->height;

        VERTEX verts[] =
        {
            {renderRect.x, renderRect.y, 0, vMax},
            {renderRect.x, renderRect.y+renderRect.h, 0, 0},
            {renderRect.x+renderRect.w, renderRect.y, uMax, vMax},
            {renderRect.x+renderRect.w, renderRect.y+renderRect.h, uMax, 0},
        };

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = sizeof(verts);
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbDesc.CPUAccessFlags = 0;
        vbDesc.MiscFlags = 0;
        vbDesc.StructureByteStride = sizeof(VERTEX);

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = verts;

        HRESULT hr = m_RenderDevice->CreateBuffer(&vbDesc, &vbData, &m_VideoVertexBuffer);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return;
        }
    }

    // Bind video rendering vertex buffer
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    m_RenderDeviceContext->IASetVertexBuffers(0, 1, m_VideoVertexBuffer.GetAddressOf(), &stride, &offset);
}

void D3D11VARenderer::bindColorConversion(bool frameChanged, AVFrame* frame)
{
    bool yuv444 = (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_YUV444);
    auto framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;

    if (yuv444) {
        // We'll need to use one of the 4:4:4 shaders for this pixel format
        switch (m_TextureFormat)
        {
        case DXGI_FORMAT_AYUV:
            m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[PixelShaders::GENERIC_AYUV].Get(), nullptr, 0);
            break;
        case DXGI_FORMAT_Y410:
            m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[PixelShaders::GENERIC_Y410].Get(), nullptr, 0);
            break;
        default:
            SDL_assert(false);
        }
    }
    else if (m_DecoderParams.chromaReconstruction && m_CflPixelShader) {
        // GFN fork: 4:2:0 shader with luma-guided chroma reconstruction ("artificial 4:4:4")
        m_RenderDeviceContext->PSSetShader(m_CflPixelShader.Get(), nullptr, 0);
    }
    else {
        // We'll need to use the generic 4:2:0 shader for this colorspace and color range combo
        m_RenderDeviceContext->PSSetShader(m_VideoPixelShaders[PixelShaders::GENERIC_YUV_420].Get(), nullptr, 0);
    }

    // If nothing has changed since last frame, we just (re)bind our existing constants.
    // NB: Post-processing passes use a different constant buffer slot, but we rebind
    // anyway to be robust against frames that skipped color conversion (VSR).
    if (!frameChanged && m_CscConstantBuffer) {
        m_RenderDeviceContext->PSSetConstantBuffers(0, 1, m_CscConstantBuffer.GetAddressOf());
        return;
    }

    D3D11_BUFFER_DESC constDesc = {};
    constDesc.ByteWidth = sizeof(CSC_CONST_BUF);
    constDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constDesc.CPUAccessFlags = 0;
    constDesc.MiscFlags = 0;

    CSC_CONST_BUF constBuf = {};
    std::array<float, 9> cscMatrix;
    std::array<float, 3> yuvOffsets;
    getFramePremultipliedCscConstants(frame, cscMatrix, yuvOffsets);

    std::copy(yuvOffsets.cbegin(), yuvOffsets.cend(), constBuf.offsets);

    // We need to adjust our CSC matrix to be column-major and with float3 vectors
    // padded with a float in between each of them to adhere to HLSL requirements.
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            constBuf.cscMatrix[i * 4 + j] = cscMatrix[j * 3 + i];
        }
    }

    std::array<float, 2> chromaOffset;
    getFrameChromaCositingOffsets(frame, chromaOffset);
    constBuf.chromaOffset[0] = chromaOffset[0] / framesContext->width;
    constBuf.chromaOffset[1] = chromaOffset[1] / framesContext->height;

    // Limit chroma texcoords to avoid sampling from alignment texels
    constBuf.chromaUVMax[0] = frame->width != framesContext->width ?
                                  ((float)(frame->width - 1) / framesContext->width) : 1.0f;
    constBuf.chromaUVMax[1] = frame->height != (int)framesContext->height ?
                                  ((float)(frame->height - 1) / framesContext->height) : 1.0f;

    // GFN fork: texel sizes of the luma and (4:2:0) chroma planes
    constBuf.planeSizes[0] = (float)framesContext->width;
    constBuf.planeSizes[1] = (float)framesContext->height;
    constBuf.planeSizes[2] = (float)((framesContext->width + 1) / 2);
    constBuf.planeSizes[3] = (float)((framesContext->height + 1) / 2);

    // GFN fork: full-strength chroma reconstruction with a luma similarity
    // sigma of 0.05 (normalized luma), i.e. 1/(2*sigma^2) = 200
    constBuf.cflParams[0] = 1.0f;
    constBuf.cflParams[1] = 200.0f;

    D3D11_SUBRESOURCE_DATA constData = {};
    constData.pSysMem = &constBuf;

    m_CscConstantBuffer.Reset();
    HRESULT hr = m_RenderDevice->CreateBuffer(&constDesc, &constData, &m_CscConstantBuffer);
    if (SUCCEEDED(hr)) {
        m_RenderDeviceContext->PSSetConstantBuffers(0, 1, m_CscConstantBuffer.GetAddressOf());
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateBuffer() failed: %x",
                     hr);
        return;
    }
}

void D3D11VARenderer::renderVideo(AVFrame* frame)
{
    // Insert a fence to force the render context to wait for the decode context to finish writing
    if (m_DecodeDevice != m_RenderDevice) {
        SDL_assert(m_DecodeD2RFence);
        SDL_assert(m_RenderD2RFence);

        lockContext(this);
        if (SUCCEEDED(m_DecodeDeviceContext->Signal(m_DecodeD2RFence.Get(), m_D2RFenceValue))) {
            m_RenderDeviceContext->Wait(m_RenderD2RFence.Get(), m_D2RFenceValue++);
        }
        unlockContext(this);
    }

    bool frameChanged = hasFrameFormatChanged(frame);
    if (frameChanged) {
        // Force the CSC constants to be regenerated for the new format
        m_CscConstantBuffer.Reset();
    }

    // Bind our vertex buffer (this also computes m_VideoDstRect)
    bindVideoVertexBuffer(frameChanged, frame);

    // GFN fork: decide how this frame will be post-processed (upscaling/sharpening)
    PostProcessMode ppMode = preparePostProcessing(frame, frameChanged);

    if (ppMode == PostProcessMode::Vsr) {
        // The video processor reads directly from the decoder output texture array,
        // so no copy or SRV binding is required here.
        if (renderVideoProcessorFrame(frame)) {
            renderRcasPass(m_PpUpscaledSrv.Get());
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: Video processor super resolution failed. Falling back to FSR.");
            m_VsrState = VsrState::Failed;
            m_PpDirty = true;
        }
    }
    else {
        UINT srvIndex;
        if (m_BindDecoderOutputTextures) {
            // Our indexing logic depends on a direct mapping into m_VideoTextureResourceViews
            // based on the texture index provided by FFmpeg.
            srvIndex = (uintptr_t)frame->data[1];
            SDL_assert(srvIndex < m_VideoTextureResourceViews.size());
            if (srvIndex >= m_VideoTextureResourceViews.size()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unexpected texture index: %u",
                             srvIndex);
                return;
            }
        }
        else {
            // Copy this frame into our video texture
            m_RenderDeviceContext->CopySubresourceRegion1(m_VideoTexture.Get(), 0, 0, 0, 0,
                                                          m_RenderSharedTextureArray.Get(),
                                                          (int)(intptr_t)frame->data[1],
                                                          nullptr, D3D11_COPY_DISCARD);

            // SRV 0 is always mapped to the video texture
            srvIndex = 0;
        }

        if (ppMode == PostProcessMode::None) {
            // Bind our CSC shader (and constant buffer, if required)
            bindColorConversion(frameChanged, frame);

            // Bind SRVs for this frame
            ID3D11ShaderResourceView* frameSrvs[] = { m_VideoTextureResourceViews[srvIndex][0].Get(), m_VideoTextureResourceViews[srvIndex][1].Get() };
            m_RenderDeviceContext->PSSetShaderResources(0, 2, frameSrvs);

            // Draw the video
            m_RenderDeviceContext->DrawIndexed(6, 0, 0);

            // Unbind SRVs for this frame
            ID3D11ShaderResourceView* nullSrvs[2] = {};
            m_RenderDeviceContext->PSSetShaderResources(0, 2, nullSrvs);
        }
        else {
            // GFN fork: multi-pass rendering (YUV->RGB, FSR upscaling, RCAS sharpening)
            renderPostProcessedVideo(frame, srvIndex, frameChanged, ppMode);
        }
    }

    // Insert a fence to force the decode context to wait for the render context to finish reading
    if (m_DecodeDevice != m_RenderDevice) {
        SDL_assert(m_DecodeR2DFence);
        SDL_assert(m_RenderR2DFence);

        // Because Pacer keeps a reference to the current frame until the next frame is rendered,
        // we insert a wait for the previous frame's fence value rather than the current one.
        // This means the fence should generally not cause a pipeline bubble for the decoder
        // unless rendering is taking much longer than expected.
        if (SUCCEEDED(m_RenderDeviceContext->Signal(m_RenderR2DFence.Get(), m_R2DFenceValue))) {
            lockContext(this);
            SDL_assert(m_R2DFenceValue > 0);
            m_DecodeDeviceContext->Wait(m_DecodeR2DFence.Get(), m_R2DFenceValue - 1);
            unlockContext(this);
            m_R2DFenceValue++;
        }
    }
}

// This function must NOT use any DXGI or ID3D11DeviceContext methods
// since it can be called on an arbitrary thread!
void D3D11VARenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    HRESULT hr;

    SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    bool overlayEnabled = Session::get()->getOverlayManager().isOverlayEnabled(type);
    if (newSurface == nullptr && overlayEnabled) {
        // The overlay is enabled and there is no new surface. Leave the old texture alone.
        return;
    }

    SDL_AtomicLock(&m_OverlayLock);
    ComPtr<ID3D11Texture2D> oldTexture = std::move(m_OverlayTextures[type]);
    ComPtr<ID3D11Buffer> oldVertexBuffer = std::move(m_OverlayVertexBuffers[type]);
    ComPtr<ID3D11ShaderResourceView> oldTextureResourceView = std::move(m_OverlayTextureResourceViews[type]);
    SDL_AtomicUnlock(&m_OverlayLock);

    // If the overlay is disabled, we're done
    if (!overlayEnabled) {
        SDL_FreeSurface(newSurface);
        return;
    }

    // Create a texture with our pixel data
    SDL_assert(!SDL_MUSTLOCK(newSurface));
    SDL_assert(newSurface->format->format == SDL_PIXELFORMAT_ARGB8888);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = newSurface->w;
    texDesc.Height = newSurface->h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem = newSurface->pixels;
    texData.SysMemPitch = newSurface->pitch;

    ComPtr<ID3D11Texture2D> newTexture;
    hr = m_RenderDevice->CreateTexture2D(&texDesc, &texData, &newTexture);
    if (FAILED(hr)) {
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateTexture2D() failed: %x",
                     hr);
        return;
    }

    ComPtr<ID3D11ShaderResourceView> newTextureResourceView;
    hr = m_RenderDevice->CreateShaderResourceView((ID3D11Resource*)newTexture.Get(), nullptr, &newTextureResourceView);
    if (FAILED(hr)) {
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateShaderResourceView() failed: %x",
                     hr);
        return;
    }

    ComPtr<ID3D11Buffer> newVertexBuffer;
    if (!createOverlayVertexBuffer(type, newSurface->w, newSurface->h, newVertexBuffer)) {
        SDL_FreeSurface(newSurface);
        return;
    }

    // The surface is no longer required
    SDL_FreeSurface(newSurface);
    newSurface = nullptr;

    SDL_AtomicLock(&m_OverlayLock);
    m_OverlayVertexBuffers[type] = std::move(newVertexBuffer);
    m_OverlayTextures[type] = std::move(newTexture);
    m_OverlayTextureResourceViews[type] = std::move(newTextureResourceView);
    SDL_AtomicUnlock(&m_OverlayLock);
}

bool D3D11VARenderer::createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, ComPtr<ID3D11Buffer>& newVertexBuffer)
{
    SDL_FRect renderRect = {};

    if (type == Overlay::OverlayStatusUpdate) {
        // Bottom Left
        renderRect.x = 0;
        renderRect.y = 0;
    }
    else if (type == Overlay::OverlayDebug) {
        // Top left
        renderRect.x = 0;
        renderRect.y = m_DisplayHeight - height;
    }

    renderRect.w = width;
    renderRect.h = height;

    // Convert screen space to normalized device coordinates
    StreamUtils::screenSpaceToNormalizedDeviceCoords(&renderRect, m_DisplayWidth, m_DisplayHeight);

    VERTEX verts[] =
    {
        {renderRect.x, renderRect.y, 0, 1},
        {renderRect.x, renderRect.y+renderRect.h, 0, 0},
        {renderRect.x+renderRect.w, renderRect.y, 1, 1},
        {renderRect.x+renderRect.w, renderRect.y+renderRect.h, 1, 0},
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(verts);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = 0;
    vbDesc.MiscFlags = 0;
    vbDesc.StructureByteStride = sizeof(VERTEX);

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = verts;

    HRESULT hr = m_RenderDevice->CreateBuffer(&vbDesc, &vbData, &newVertexBuffer);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateBuffer() failed: %x",
                     hr);
        return false;
    }

    return true;
}

bool D3D11VARenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo)
{
    if (stateInfo->stateChangeFlags & WINDOW_STATE_CHANGE_DISPLAY) {
        int adapterIndex, outputIndex;
        if (!SDL_DXGIGetOutputInfo(stateInfo->displayIndex,
                                   &adapterIndex, &outputIndex)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_DXGIGetOutputInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        // If the window moved to a different GPU, recreate the renderer
        // to see if we can use that new GPU for decoding
        if (adapterIndex != m_AdapterIndex) {
            return false;
        }

        // If an adapter was added or removed, we can't trust that our
        // old indexes are still valid for comparison.
        if (!m_Factory->IsCurrent()) {
            return false;
        }

        // We've handled this state change
        stateInfo->stateChangeFlags &= ~WINDOW_STATE_CHANGE_DISPLAY;
    }

    if (stateInfo->stateChangeFlags & WINDOW_STATE_CHANGE_SIZE) {
        // Resize our swapchain and reconstruct size-dependent resources

        DXGI_SWAP_CHAIN_DESC1 swapchainDesc;
        m_SwapChain->GetDesc1(&swapchainDesc);

        // Lock the context to avoid concurrent decoding use. This is
        // already synchronized with rendering by the render lock, so
        // we only need to do this when not using separate devices.
        if (m_DecodeDevice == m_RenderDevice) {
            lockContext(this);
        }

        m_DisplayWidth = stateInfo->width;
        m_DisplayHeight = stateInfo->height;

        // Release the video vertex buffer so we will upload a new one after resize
        m_VideoVertexBuffer.Reset();

        // Create new vertex buffers for active overlays
        SDL_AtomicLock(&m_OverlayLock);
        for (size_t i = 0; i < m_OverlayVertexBuffers.size(); i++) {
            if (!m_OverlayTextures[i]) {
                continue;
            }

            D3D11_TEXTURE2D_DESC textureDesc;
            m_OverlayTextures[i]->GetDesc(&textureDesc);
            createOverlayVertexBuffer((Overlay::OverlayType)i, textureDesc.Width, textureDesc.Height, m_OverlayVertexBuffers[i]);
        }
        SDL_AtomicUnlock(&m_OverlayLock);

        // We must release all references to the back buffer
        m_RenderTargetView.Reset();
        m_RenderDeviceContext->Flush();

        HRESULT hr = m_SwapChain->ResizeBuffers(0, stateInfo->width, stateInfo->height, DXGI_FORMAT_UNKNOWN, swapchainDesc.Flags);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::ResizeBuffers() failed: %x",
                         hr);
            if (m_DecodeDevice == m_RenderDevice) {
                unlockContext(this);
            }
            return false;
        }

        // Reset swapchain-dependent resources (RTV, viewport, etc)
        if (!setupSwapchainDependentResources()) {
            if (m_DecodeDevice == m_RenderDevice) {
                unlockContext(this);
            }
            return false;
        }

        if (m_DecodeDevice == m_RenderDevice) {
            unlockContext(this);
        }

        // We've handled this state change
        stateInfo->stateChangeFlags &= ~WINDOW_STATE_CHANGE_SIZE;
    }

    // Check if we've handled all state changes
    return stateInfo->stateChangeFlags == 0;
}

bool D3D11VARenderer::checkDecoderSupport(IDXGIAdapter* adapter)
{
    HRESULT hr;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;

    DXGI_ADAPTER_DESC adapterDesc;
    hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGIAdapter::GetDesc() failed: %x",
                     hr);
        return false;
    }

    // Derive a ID3D11VideoDevice from our ID3D11Device.
    hr = m_RenderDevice.As(&videoDevice);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::QueryInterface(ID3D11VideoDevice) failed: %x",
                     hr);
        return false;
    }

    // Check if the format is supported by this decoder
    BOOL supported;
    switch (m_DecoderParams.videoFormat)
    {
    case VIDEO_FORMAT_H264:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support H.264 decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support H.264 decoding to NV12 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H264_HIGH8_444:
        // Unsupported by DXVA
        return false;

    case VIDEO_FORMAT_H265:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, DXGI_FORMAT_NV12, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC decoding to NV12 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H265_MAIN10:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, DXGI_FORMAT_P010, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main10 decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main10 decoding to P010 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H265_REXT8_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN_444, DXGI_FORMAT_AYUV, &supported)))
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 8-bit decoding via D3D11VA");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 8-bit decoding to AYUV format");
            return false;
        }
        break;

    case VIDEO_FORMAT_H265_REXT10_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&k_D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10_444, DXGI_FORMAT_Y410, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 10-bit decoding via D3D11VA");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support HEVC Main 444 10-bit decoding to Y410 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_MAIN8:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, DXGI_FORMAT_NV12, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 decoding to NV12 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_MAIN10:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, DXGI_FORMAT_P010, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 Main 10-bit decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 Main 10-bit decoding to P010 format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_HIGH8_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1, DXGI_FORMAT_AYUV, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 8-bit decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 8-bit decoding to AYUV format");
            return false;
        }
        break;

    case VIDEO_FORMAT_AV1_HIGH10_444:
        if (FAILED(videoDevice->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1, DXGI_FORMAT_Y410, &supported))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 10-bit decoding");
            return false;
        }
        else if (!supported) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GPU doesn't support AV1 High 444 10-bit decoding to Y410 format");
            return false;
        }
        break;

    default:
        SDL_assert(false);
        return false;
    }

    if (DXUtil::isFormatHybridDecodedByHardware(m_DecoderParams.videoFormat, adapterDesc.VendorId, adapterDesc.DeviceId)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GPU decoding for format %x is blocked due to hardware limitations",
                    m_DecoderParams.videoFormat);
        return false;
    }

    return true;
}

int D3D11VARenderer::getRendererAttributes()
{
    int attributes = 0;

    // This renderer supports HDR
    attributes |= RENDERER_ATTRIBUTE_HDR_SUPPORT;

    // This renderer requires frame pacing to synchronize with VBlank when we're in full-screen.
    // In windowed mode, we will render as fast we can and DWM will grab whatever is latest at the
    // time unless the user opts for pacing. We will use pacing in full-screen mode and normal DWM
    // sequencing in full-screen desktop mode to behave similarly to the DXVA2 renderer.
    if ((SDL_GetWindowFlags(m_DecoderParams.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        attributes |= RENDERER_ATTRIBUTE_FORCE_PACING;
    }

    return attributes;
}

int D3D11VARenderer::getDecoderCapabilities()
{
    return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
           CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
}

IFFmpegRenderer::InitFailureReason D3D11VARenderer::getInitFailureReason()
{
    // In the specific case where we found at least one D3D11 hardware device but none of the
    // enumerated devices have support for the specified codec, tell the FFmpeg decoder not to
    // bother trying other hwaccels. We don't want to try loading D3D9 if the device doesn't
    // even have hardware support for the codec.
    //
    // NB: We use feature level 11.0 support as a gate here because we want to avoid returning
    // this failure reason in cases where we might have an extremely old GPU with support for
    // DXVA2 on D3D9 but not D3D11VA on D3D11. I'm unsure if any such drivers/hardware exists,
    // but better be safe than sorry.
    //
    // NB2: We're also assuming that no GPU exists which lacks any D3D11 driver but has drivers
    // for non-DX APIs like Vulkan. I believe this is a Windows Logo requirement so it should be
    // safe to assume.
    //
    // NB3: Sigh, there *are* GPUs drivers with greater codec support available via Vulkan than
    // D3D11VA even when both D3D11 and Vulkan APIs are supported. This is the case for HEVC RExt
    // profiles that were not supported by Microsoft until the Windows 11 24H2 SDK. Don't report
    // that hardware support is missing for YUV444 profiles since the Vulkan driver may support it.
    if (m_DevicesWithFL11Support != 0 && m_DevicesWithCodecSupport == 0 && !(m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_YUV444)) {
        return InitFailureReason::NoHardwareSupport;
    }
    else {
        return InitFailureReason::Unknown;
    }
}

void D3D11VARenderer::lockContext(void *lock_ctx)
{
    auto me = (D3D11VARenderer*)lock_ctx;

    SDL_LockMutex(me->m_ContextLock);
}

void D3D11VARenderer::unlockContext(void *lock_ctx)
{
    auto me = (D3D11VARenderer*)lock_ctx;

    SDL_UnlockMutex(me->m_ContextLock);
}

bool D3D11VARenderer::setupRenderingResources()
{
    HRESULT hr;

    m_RenderDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // We use a common vertex shader for all pixel shaders
    {
        QByteArray vertexShaderBytecode = Path::readDataFile("d3d11_vertex.fxc");

        ComPtr<ID3D11VertexShader> vertexShader;
        hr = m_RenderDevice->CreateVertexShader(vertexShaderBytecode.constData(), vertexShaderBytecode.length(), nullptr, &vertexShader);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->VSSetShader(vertexShader.Get(), nullptr, 0);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateVertexShader() failed: %x",
                         hr);
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC vertexDesc[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ComPtr<ID3D11InputLayout> inputLayout;
        hr = m_RenderDevice->CreateInputLayout(vertexDesc, ARRAYSIZE(vertexDesc), vertexShaderBytecode.constData(), vertexShaderBytecode.length(), &inputLayout);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->IASetInputLayout(inputLayout.Get());
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateInputLayout() failed: %x",
                         hr);
            return false;
        }
    }

    {
        QByteArray overlayPixelShaderBytecode = Path::readDataFile("d3d11_overlay_pixel.fxc");

        hr = m_RenderDevice->CreatePixelShader(overlayPixelShaderBytecode.constData(), overlayPixelShaderBytecode.length(), nullptr, &m_OverlayPixelShader);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreatePixelShader() failed: %x",
                         hr);
            return false;
        }
    }

    for (int i = 0; i < PixelShaders::_COUNT; i++)
    {
        QByteArray videoPixelShaderBytecode = Path::readDataFile(k_VideoShaderNames[i]);

        hr = m_RenderDevice->CreatePixelShader(videoPixelShaderBytecode.constData(), videoPixelShaderBytecode.length(), nullptr, &m_VideoPixelShaders[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreatePixelShader() failed: %x",
                         hr);
            return false;
        }
    }

    // We use a common sampler for all pixel shaders
    {
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        ComPtr<ID3D11SamplerState> sampler;
        hr = m_RenderDevice->CreateSamplerState(&samplerDesc,  &sampler);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->PSSetSamplers(0, 1, sampler.GetAddressOf());
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateSamplerState() failed: %x",
                         hr);
            return false;
        }
    }

    // We use a common index buffer for all geometry
    {
        const int indexes[] = {0, 1, 2, 3, 2, 1};
        D3D11_BUFFER_DESC indexBufferDesc = {};
        indexBufferDesc.ByteWidth = sizeof(indexes);
        indexBufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        indexBufferDesc.CPUAccessFlags = 0;
        indexBufferDesc.MiscFlags = 0;
        indexBufferDesc.StructureByteStride = sizeof(int);

        D3D11_SUBRESOURCE_DATA indexBufferData = {};
        indexBufferData.pSysMem = indexes;
        indexBufferData.SysMemPitch = sizeof(int);

        ComPtr<ID3D11Buffer> indexBuffer;
        hr = m_RenderDevice->CreateBuffer(&indexBufferDesc, &indexBufferData, &indexBuffer);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return false;
        }
    }

    // Create our overlay blend state
    {
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = m_RenderDevice->CreateBlendState(&blendDesc, &m_OverlayBlendState);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBlendState() failed: %x",
                         hr);
            return false;
        }
    }

    // Create and bind our video blend state
    {
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = FALSE;
        blendDesc.IndependentBlendEnable = FALSE;
        blendDesc.RenderTarget[0].BlendEnable = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = m_RenderDevice->CreateBlendState(&blendDesc, &m_VideoBlendState);
        if (SUCCEEDED(hr)) {
            m_RenderDeviceContext->OMSetBlendState(m_VideoBlendState.Get(), nullptr, 0xffffffff);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateBlendState() failed: %x",
                         hr);
            return false;
        }
    }

    if (!setupSwapchainDependentResources()) {
        return false;
    }

    return true;
}

bool D3D11VARenderer::setupSwapchainDependentResources()
{
    HRESULT hr;

    // Create our render target view
    {
        ComPtr<ID3D11Resource> backBufferResource;
        hr = m_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferResource));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGISwapChain::GetBuffer() failed: %x",
                         hr);
            return false;
        }

        hr = m_RenderDevice->CreateRenderTargetView(backBufferResource.Get(), nullptr, &m_RenderTargetView);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateRenderTargetView() failed: %x",
                         hr);
            return false;
        }
    }

    // Set a viewport that fills the window
    {
        D3D11_VIEWPORT viewport;

        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.Width = m_DisplayWidth;
        viewport.Height = m_DisplayHeight;
        viewport.MinDepth = 0;
        viewport.MaxDepth = 1;

        m_RenderDeviceContext->RSSetViewports(1, &viewport);
    }

    return true;
}

// NB: This can be called more than once (and with different frame dimensions!)
bool D3D11VARenderer::setupFrameRenderingResources(AVHWFramesContext* framesContext)
{
    auto d3d11vaFramesContext = (AVD3D11VAFramesContext*)framesContext->hwctx;

    // GFN fork: the decoder texture pool is changing, so post-processing
    // resources (video processor input views, etc.) must be recreated.
    m_PpDirty = true;

    // Open the decoder texture array on the renderer device if we're using separate devices
    if (m_DecodeDevice != m_RenderDevice) {
        ComPtr<IDXGIResource1> dxgiDecoderResource;

        HRESULT hr = d3d11vaFramesContext->texture_infos->texture->QueryInterface(IID_PPV_ARGS(&dxgiDecoderResource));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Texture2D::QueryInterface(IDXGIResource1) failed: %x",
                         hr);
            return false;
        }

        HANDLE sharedHandle;
        hr = dxgiDecoderResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedHandle);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "IDXGIResource1::CreateSharedHandle() failed: %x",
                         hr);
            return false;
        }

        hr = m_RenderDevice->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&m_RenderSharedTextureArray));
        CloseHandle(sharedHandle);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device1::OpenSharedResource1() failed: %x",
                         hr);
            return false;
        }
    }
    else {
        d3d11vaFramesContext->texture_infos->texture->AddRef();
        m_RenderSharedTextureArray.Attach(d3d11vaFramesContext->texture_infos->texture);
    }

    // Query the format of the underlying texture array
    D3D11_TEXTURE2D_DESC textureDesc;
    m_RenderSharedTextureArray->GetDesc(&textureDesc);
    m_TextureFormat = textureDesc.Format;

    if (m_BindDecoderOutputTextures) {
        // Create SRVs for all textures in the decoder pool
        if (!setupTexturePoolViews(framesContext)) {
            return false;
        }
    }
    else {
        // Create our internal texture to copy and render
        if (!setupVideoTexture(framesContext)) {
            return false;
        }
    }

    return true;
}

std::vector<DXGI_FORMAT> D3D11VARenderer::getVideoTextureSRVFormats()
{
    if (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_YUV444) {
        // YUV 4:4:4 formats don't use a second SRV
        return { (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_10BIT) ?
                    DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM };
    }
    else if (m_DecoderParams.videoFormat & VIDEO_FORMAT_MASK_10BIT) {
        return { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM };
    }
    else {
        return { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM };
    }
}

bool D3D11VARenderer::setupVideoTexture(AVHWFramesContext* framesContext)
{
    SDL_assert(!m_BindDecoderOutputTextures);

    HRESULT hr;
    D3D11_TEXTURE2D_DESC texDesc = {};

    texDesc.Width = framesContext->width;
    texDesc.Height = framesContext->height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = m_TextureFormat;
    texDesc.SampleDesc.Quality = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    hr = m_RenderDevice->CreateTexture2D(&texDesc, nullptr, &m_VideoTexture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateTexture2D() failed: %x",
                     hr);
        return false;
    }

    // We will only have one set of SRVs
    m_VideoTextureResourceViews.resize(1);

    // Create SRVs for the texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    size_t srvIndex = 0;
    for (DXGI_FORMAT srvFormat : getVideoTextureSRVFormats()) {
        SDL_assert(srvIndex < m_VideoTextureResourceViews[0].size());

        srvDesc.Format = srvFormat;
        hr = m_RenderDevice->CreateShaderResourceView(m_VideoTexture.Get(), &srvDesc, &m_VideoTextureResourceViews[0][srvIndex]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateShaderResourceView() failed: %x",
                         hr);
            return false;
        }

        srvIndex++;
    }

    return true;
}

bool D3D11VARenderer::setupTexturePoolViews(AVHWFramesContext* framesContext)
{
    AVD3D11VAFramesContext* d3d11vaFramesContext = (AVD3D11VAFramesContext*)framesContext->hwctx;

    SDL_assert(m_BindDecoderOutputTextures);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = 1;

    m_VideoTextureResourceViews.resize(framesContext->initial_pool_size);

    // Create luminance and chrominance SRVs for each texture in the pool
    for (int i = 0; i < framesContext->initial_pool_size; i++) {
        HRESULT hr;

        // Our rendering logic depends on the texture index working to map into our SRV array
        SDL_assert(i == d3d11vaFramesContext->texture_infos[i].index);

        srvDesc.Texture2DArray.FirstArraySlice = d3d11vaFramesContext->texture_infos[i].index;

        size_t srvIndex = 0;
        for (DXGI_FORMAT srvFormat : getVideoTextureSRVFormats()) {
            SDL_assert(srvIndex < m_VideoTextureResourceViews[i].size());

            srvDesc.Format = srvFormat;
            hr = m_RenderDevice->CreateShaderResourceView(m_RenderSharedTextureArray.Get(),
                                                          &srvDesc,
                                                          &m_VideoTextureResourceViews[i][srvIndex]);
            if (FAILED(hr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "ID3D11Device::CreateShaderResourceView() failed: %x",
                             hr);
                return false;
            }

            srvIndex++;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// GFN fork: post-processing (upscaling, sharpening, chroma reconstruction)
// ---------------------------------------------------------------------------

void D3D11VARenderer::setupPostProcessingShaders()
{
    auto loadShader = [this](const char* name, ComPtr<ID3D11PixelShader>& shader) {
        QByteArray bytecode = Path::readDataFile(name);
        if (bytecode.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: Shader %s is missing. The feature using it is disabled.",
                        name);
            return;
        }

        HRESULT hr = m_RenderDevice->CreatePixelShader(bytecode.constData(), bytecode.length(), nullptr, &shader);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11Device::CreatePixelShader(%s) failed: %x",
                         name,
                         hr);
            shader.Reset();
        }
    };

    if (m_DecoderParams.chromaReconstruction) {
        loadShader("d3d11_yuv420_cfl_pixel.fxc", m_CflPixelShader);
    }

    if (m_DecoderParams.upscalingMode != StreamingPreferences::UM_OFF || m_DecoderParams.sharpness > 0) {
        loadShader("d3d11_fsr_rcas_pixel.fxc", m_RcasPixelShader);
    }

    if (m_DecoderParams.upscalingMode != StreamingPreferences::UM_OFF) {
        loadShader("d3d11_fsr_easu_pixel.fxc", m_EasuPixelShader);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GFN: Upscaling mode: %d | Sharpness: %d%% | Chroma reconstruction: %s | Low-latency mode: %s",
                (int)m_DecoderParams.upscalingMode,
                m_DecoderParams.sharpness,
                m_CflPixelShader ? "on" : "off",
                m_DecoderParams.lowLatencyMode ? "on" : "off");
}

void D3D11VARenderer::applyLowLatencyTweaks()
{
    if (!m_DecoderParams.lowLatencyMode) {
        return;
    }

    // Ask the GPU scheduler to prioritize our rendering work over other applications
    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(m_RenderDevice.As(&dxgiDevice))) {
        HRESULT hr = dxgiDevice->SetGPUThreadPriority(7);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: IDXGIDevice::SetGPUThreadPriority() failed: %x",
                        hr);
        }
    }

    // Raise our CPU scheduling priority a bit while streaming
    if (GetPriorityClass(GetCurrentProcess()) == NORMAL_PRIORITY_CLASS) {
        if (SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
            m_RaisedProcessPriority = true;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GFN: Low-latency tweaks applied");
}

void D3D11VARenderer::setViewport(int x, int y, int width, int height)
{
    D3D11_VIEWPORT viewport;

    viewport.TopLeftX = (FLOAT)x;
    viewport.TopLeftY = (FLOAT)y;
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MinDepth = 0;
    viewport.MaxDepth = 1;

    m_RenderDeviceContext->RSSetViewports(1, &viewport);
}

D3D11VARenderer::PostProcessMode D3D11VARenderer::choosePostProcessMode(AVFrame* frame)
{
    // The RCAS pass is the final pass of every post-processing mode
    if (m_PpDisabled || !m_RcasPixelShader) {
        return PostProcessMode::None;
    }

    int dstW = m_VideoDstRect.w;
    int dstH = m_VideoDstRect.h;
    if (dstW <= 0 || dstH <= 0 || frame->width <= 0 || frame->height <= 0) {
        return PostProcessMode::None;
    }

    bool upscaling = dstW >= frame->width && dstH >= frame->height &&
                     (dstW > frame->width || dstH > frame->height);
    bool oneToOne = dstW == frame->width && dstH == frame->height;

    if (upscaling && m_DecoderParams.upscalingMode != StreamingPreferences::UM_OFF) {
        // Driver-based AI upscaling only works for SDR 8-bit 4:2:0 content
        if (m_DecoderParams.upscalingMode == StreamingPreferences::UM_AI &&
                m_VsrState != VsrState::Failed &&
                !(m_DecoderParams.videoFormat & (VIDEO_FORMAT_MASK_10BIT | VIDEO_FORMAT_MASK_YUV444)) &&
                (m_AdapterVendorId == PCI_VENDOR_NVIDIA || m_AdapterVendorId == PCI_VENDOR_INTEL)) {
            return PostProcessMode::Vsr;
        }

        if (m_EasuPixelShader) {
            return PostProcessMode::Fsr;
        }
    }

    if (oneToOne && m_DecoderParams.sharpness > 0) {
        return PostProcessMode::SharpenOnly;
    }

    return PostProcessMode::None;
}

D3D11VARenderer::PostProcessMode D3D11VARenderer::preparePostProcessing(AVFrame* frame, bool frameChanged)
{
    PostProcessMode mode = choosePostProcessMode(frame);

    if (mode == PostProcessMode::None) {
        if (m_PpMode != PostProcessMode::None) {
            releasePostProcessingTargets();
        }
        return PostProcessMode::None;
    }

    bool needsSetup = m_PpDirty || frameChanged || mode != m_PpMode ||
                      frame->width != m_PpFrameWidth || frame->height != m_PpFrameHeight ||
                      m_VideoDstRect.x != m_PpDstRect.x || m_VideoDstRect.y != m_PpDstRect.y ||
                      m_VideoDstRect.w != m_PpDstRect.w || m_VideoDstRect.h != m_PpDstRect.h;
    if (!needsSetup) {
        return mode;
    }

    if (setupPostProcessingTargets(frame, mode)) {
        if (mode == PostProcessMode::Vsr && m_VsrState != VsrState::Active) {
            m_VsrState = VsrState::Active;
        }
        return mode;
    }

    releasePostProcessingTargets();

    if (mode == PostProcessMode::Vsr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GFN: Driver super resolution is unavailable on this GPU/driver. Using FSR instead.");
        m_VsrState = VsrState::Failed;

        mode = choosePostProcessMode(frame);
        if (mode != PostProcessMode::None && setupPostProcessingTargets(frame, mode)) {
            return mode;
        }

        releasePostProcessingTargets();
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "GFN: Post-processing setup failed. Using the standard renderer.");
    m_PpDisabled = true;
    return PostProcessMode::None;
}

void D3D11VARenderer::releasePostProcessingTargets()
{
    m_PassVertexBuffer.Reset();
    m_EasuConstantBuffer.Reset();
    m_RcasConstantBuffer.Reset();

    m_PpSourceSrv.Reset();
    m_PpSourceRtv.Reset();
    m_PpSourceTexture.Reset();

    m_PpUpscaledSrv.Reset();
    m_PpUpscaledRtv.Reset();
    m_PpUpscaledTexture.Reset();

    m_VideoProcessorInputViews.clear();
    m_VideoProcessorOutputView.Reset();
    m_VideoProcessor.Reset();
    m_VideoProcessorEnumerator.Reset();

    m_PpMode = PostProcessMode::None;
    m_PpFrameWidth = 0;
    m_PpFrameHeight = 0;
    m_PpDstRect = {};
}

bool D3D11VARenderer::createRenderTexture(int width, int height, DXGI_FORMAT format,
                                          ComPtr<ID3D11Texture2D>& texture,
                                          ComPtr<ID3D11RenderTargetView>& rtv,
                                          ComPtr<ID3D11ShaderResourceView>& srv)
{
    HRESULT hr;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    hr = m_RenderDevice->CreateTexture2D(&texDesc, nullptr, &texture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11Device::CreateTexture2D(%dx%d) failed: %x",
                     width, height, hr);
        return false;
    }

    hr = m_RenderDevice->CreateRenderTargetView(texture.Get(), nullptr, &rtv);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11Device::CreateRenderTargetView() failed: %x",
                     hr);
        return false;
    }

    hr = m_RenderDevice->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11Device::CreateShaderResourceView() failed: %x",
                     hr);
        return false;
    }

    return true;
}

bool D3D11VARenderer::setupPostProcessingTargets(AVFrame* frame, PostProcessMode mode)
{
    HRESULT hr;

    releasePostProcessingTargets();

    if (mode == PostProcessMode::None) {
        return true;
    }

    const int dstW = m_VideoDstRect.w;
    const int dstH = m_VideoDstRect.h;

    // Full-target quad for intermediate passes. The texture coordinates exclude
    // the decoder's alignment padding, just like the regular video vertex buffer.
    {
        auto framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;
        float uMax = (float)frame->width / framesContext->width;
        float vMax = (float)frame->height / framesContext->height;

        VERTEX verts[] =
        {
            {-1.0f, -1.0f, 0, vMax},
            {-1.0f, 1.0f, 0, 0},
            {1.0f, -1.0f, uMax, vMax},
            {1.0f, 1.0f, uMax, 0},
        };

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = sizeof(verts);
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbDesc.StructureByteStride = sizeof(VERTEX);

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = verts;

        hr = m_RenderDevice->CreateBuffer(&vbDesc, &vbData, &m_PassVertexBuffer);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return false;
        }
    }

    // RGB image at stream resolution
    if (mode == PostProcessMode::SharpenOnly || mode == PostProcessMode::Fsr) {
        if (!createRenderTexture(frame->width, frame->height, DXGI_FORMAT_R10G10B10A2_UNORM,
                                 m_PpSourceTexture, m_PpSourceRtv, m_PpSourceSrv)) {
            return false;
        }
    }

    // RGB image at output resolution
    if (mode == PostProcessMode::Fsr || mode == PostProcessMode::Vsr) {
        if (!createRenderTexture(dstW, dstH,
                                 mode == PostProcessMode::Vsr ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM,
                                 m_PpUpscaledTexture, m_PpUpscaledRtv, m_PpUpscaledSrv)) {
            return false;
        }
    }

    // FSR EASU constants (FsrEasuCon)
    if (mode == PostProcessMode::Fsr) {
        const float inViewW = (float)frame->width;
        const float inViewH = (float)frame->height;
        const float inSizeW = (float)frame->width;
        const float inSizeH = (float)frame->height;
        const float outW = (float)dstW;
        const float outH = (float)dstH;

        EASU_CONST_BUF easu = {};
        easu.con0[0] = inViewW / outW;
        easu.con0[1] = inViewH / outH;
        easu.con0[2] = 0.5f * inViewW / outW - 0.5f;
        easu.con0[3] = 0.5f * inViewH / outH - 0.5f;
        easu.con1[0] = 1.0f / inSizeW;
        easu.con1[1] = 1.0f / inSizeH;
        easu.con1[2] = 1.0f / inSizeW;
        easu.con1[3] = -1.0f / inSizeH;
        easu.con2[0] = -1.0f / inSizeW;
        easu.con2[1] = 2.0f / inSizeH;
        easu.con2[2] = 1.0f / inSizeW;
        easu.con2[3] = 2.0f / inSizeH;
        easu.con3[0] = 0.0f;
        easu.con3[1] = 4.0f / inSizeH;
        easu.con3[2] = 0.0f;
        easu.con3[3] = 0.0f;

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(easu);
        cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        D3D11_SUBRESOURCE_DATA cbData = {};
        cbData.pSysMem = &easu;

        hr = m_RenderDevice->CreateBuffer(&cbDesc, &cbData, &m_EasuConstantBuffer);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return false;
        }
    }

    // RCAS constants (final pass into the back buffer)
    {
        int inW = (mode == PostProcessMode::SharpenOnly) ? frame->width : dstW;
        int inH = (mode == PostProcessMode::SharpenOnly) ? frame->height : dstH;

        // The driver upscalers already sharpen, so we apply our sharpening at half strength there
        float amount = m_DecoderParams.sharpness / 100.0f;
        if (mode == PostProcessMode::Vsr) {
            amount *= 0.5f;
        }

        RCAS_CONST_BUF rcas = {};
        rcas.params[0] = SDL_max(0.0f, SDL_min(amount, 1.0f));
        rcas.rect[0] = (float)m_VideoDstRect.x;
        // m_VideoDstRect uses a bottom-left origin, but SV_Position uses a top-left origin
        rcas.rect[1] = (float)(m_DisplayHeight - (m_VideoDstRect.y + m_VideoDstRect.h));
        rcas.rect[2] = (float)inW;
        rcas.rect[3] = (float)inH;

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(rcas);
        cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        D3D11_SUBRESOURCE_DATA cbData = {};
        cbData.pSysMem = &rcas;

        hr = m_RenderDevice->CreateBuffer(&cbDesc, &cbData, &m_RcasConstantBuffer);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11Device::CreateBuffer() failed: %x",
                         hr);
            return false;
        }
    }

    if (mode == PostProcessMode::Vsr && !setupVideoProcessor(frame)) {
        return false;
    }

    m_PpMode = mode;
    m_PpFrameWidth = frame->width;
    m_PpFrameHeight = frame->height;
    m_PpDstRect = m_VideoDstRect;
    m_PpDirty = false;

    const char* modeName =
        mode == PostProcessMode::Vsr ? (m_AdapterVendorId == PCI_VENDOR_NVIDIA ? "NVIDIA RTX Video Super Resolution" : "Intel Video Super Resolution") :
        mode == PostProcessMode::Fsr ? "AMD FSR 1 (EASU + RCAS)" :
                                       "RCAS sharpening";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "GFN: Post-processing active: %s (%dx%d -> %dx%d)",
                modeName, frame->width, frame->height, dstW, dstH);

    return true;
}

bool D3D11VARenderer::setupVideoProcessor(AVFrame* frame)
{
    HRESULT hr;
    auto framesContext = (AVHWFramesContext*)frame->hw_frames_ctx->data;

    if (m_AdapterVendorId != PCI_VENDOR_NVIDIA && m_AdapterVendorId != PCI_VENDOR_INTEL) {
        return false;
    }

    if (!m_VideoDevice) {
        hr = m_RenderDevice.As(&m_VideoDevice);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11Device::QueryInterface(ID3D11VideoDevice) failed: %x",
                         hr);
            return false;
        }
    }

    if (!m_VideoContext) {
        hr = m_RenderDeviceContext.As(&m_VideoContext);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11DeviceContext::QueryInterface(ID3D11VideoContext1) failed: %x",
                         hr);
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate.Numerator = m_DecoderParams.frameRate > 0 ? m_DecoderParams.frameRate : 60;
    contentDesc.InputFrameRate.Denominator = 1;
    contentDesc.InputWidth = framesContext->width;
    contentDesc.InputHeight = framesContext->height;
    contentDesc.OutputFrameRate = contentDesc.InputFrameRate;
    contentDesc.OutputWidth = m_VideoDstRect.w;
    contentDesc.OutputHeight = m_VideoDstRect.h;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = m_VideoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_VideoProcessorEnumerator);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11VideoDevice::CreateVideoProcessorEnumerator() failed: %x",
                     hr);
        return false;
    }

    UINT formatSupport = 0;
    hr = m_VideoProcessorEnumerator->CheckVideoProcessorFormat(m_TextureFormat, &formatSupport);
    if (FAILED(hr) || !(formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GFN: Video processor doesn't support input format %d",
                    (int)m_TextureFormat);
        return false;
    }

    formatSupport = 0;
    hr = m_VideoProcessorEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_R8G8B8A8_UNORM, &formatSupport);
    if (FAILED(hr) || !(formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GFN: Video processor doesn't support RGBA8 output");
        return false;
    }

    hr = m_VideoDevice->CreateVideoProcessor(m_VideoProcessorEnumerator.Get(), 0, &m_VideoProcessor);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11VideoDevice::CreateVideoProcessor() failed: %x",
                     hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;
    hr = m_VideoDevice->CreateVideoProcessorOutputView(m_PpUpscaledTexture.Get(),
                                                       m_VideoProcessorEnumerator.Get(),
                                                       &outputViewDesc,
                                                       &m_VideoProcessorOutputView);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11VideoDevice::CreateVideoProcessorOutputView() failed: %x",
                     hr);
        return false;
    }

    // Input views are created lazily for each decoder surface we see
    {
        D3D11_TEXTURE2D_DESC arrayDesc;
        m_RenderSharedTextureArray->GetDesc(&arrayDesc);
        m_VideoProcessorInputViews.clear();
        m_VideoProcessorInputViews.resize(arrayDesc.ArraySize);
    }

    ID3D11VideoProcessor* vp = m_VideoProcessor.Get();

    RECT srcRect = { 0, 0, frame->width, frame->height };
    RECT dstRect = { 0, 0, m_VideoDstRect.w, m_VideoDstRect.h };
    m_VideoContext->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_VideoContext->VideoProcessorSetStreamOutputRate(vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
    m_VideoContext->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &srcRect);
    m_VideoContext->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dstRect);
    m_VideoContext->VideoProcessorSetOutputTargetRect(vp, TRUE, &dstRect);
    m_VideoContext->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    // Color spaces
    {
        bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
        DXGI_COLOR_SPACE_TYPE inputColorSpace;

        switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            inputColorSpace = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            inputColorSpace = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
            break;
        default:
            inputColorSpace = fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
            break;
        }

        m_VideoContext->VideoProcessorSetStreamColorSpace1(vp, 0, inputColorSpace);
        m_VideoContext->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    // Enable the vendor-specific super resolution scaler
    if (m_AdapterVendorId == PCI_VENDOR_NVIDIA) {
        struct {
            UINT version;
            UINT method;
            UINT enable;
        } nvidiaExtension = { 0x1, 0x2, 0x1 };

        hr = m_VideoContext->VideoProcessorSetStreamExtension(vp, 0, &k_NvidiaPPEInterfaceGUID,
                                                              sizeof(nvidiaExtension), &nvidiaExtension);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: NVIDIA RTX Video Super Resolution is not available: %x",
                        hr);
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GFN: NVIDIA RTX Video Super Resolution requested. Make sure 'RTX Video Enhancement > Super resolution' "
                    "is enabled in the NVIDIA App or NVIDIA Control Panel.");
    }
    else {
        INTEL_VPE_EXT intelExtension = {};

        UINT version = INTEL_VPE_VERSION_3;
        intelExtension.function = INTEL_VPE_FN_VERSION;
        intelExtension.param = &version;
        hr = m_VideoContext->VideoProcessorSetOutputExtension(vp, &k_IntelVpeInterfaceGUID,
                                                              sizeof(intelExtension), &intelExtension);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: Intel VPE version negotiation failed: %x",
                        hr);
            return false;
        }

        UINT vpeMode = INTEL_VPE_MODE_PREPROC;
        intelExtension.function = INTEL_VPE_FN_MODE;
        intelExtension.param = &vpeMode;
        hr = m_VideoContext->VideoProcessorSetOutputExtension(vp, &k_IntelVpeInterfaceGUID,
                                                              sizeof(intelExtension), &intelExtension);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: Intel VPE mode selection failed: %x",
                        hr);
            return false;
        }

        UINT scaling = INTEL_VPE_SCALING_SUPER_RESOLUTION;
        intelExtension.function = INTEL_VPE_FN_SCALING;
        intelExtension.param = &scaling;
        hr = m_VideoContext->VideoProcessorSetStreamExtension(vp, 0, &k_IntelVpeInterfaceGUID,
                                                              sizeof(intelExtension), &intelExtension);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GFN: Intel Video Super Resolution is not available: %x",
                        hr);
            return false;
        }
    }

    return true;
}

bool D3D11VARenderer::renderVideoProcessorFrame(AVFrame* frame)
{
    HRESULT hr;
    UINT index = (UINT)(uintptr_t)frame->data[1];

    if (!m_VideoProcessor || !m_VideoProcessorOutputView || index >= m_VideoProcessorInputViews.size()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: Video processor not ready for texture index %u",
                     index);
        return false;
    }

    if (!m_VideoProcessorInputViews[index]) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
        inputViewDesc.FourCC = 0;
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice = index;

        hr = m_VideoDevice->CreateVideoProcessorInputView(m_RenderSharedTextureArray.Get(),
                                                          m_VideoProcessorEnumerator.Get(),
                                                          &inputViewDesc,
                                                          &m_VideoProcessorInputViews[index]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GFN: ID3D11VideoDevice::CreateVideoProcessorInputView() failed: %x",
                         hr);
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = m_VideoProcessorInputViews[index].Get();

    hr = m_VideoContext->VideoProcessorBlt(m_VideoProcessor.Get(), m_VideoProcessorOutputView.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GFN: ID3D11VideoContext::VideoProcessorBlt() failed: %x",
                     hr);
        return false;
    }

    return true;
}

void D3D11VARenderer::renderPostProcessedVideo(AVFrame* frame, UINT srvIndex, bool frameChanged, PostProcessMode mode)
{
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    ID3D11ShaderResourceView* nullSrvs[2] = {};

    // Pass 1: YUV -> RGB at stream resolution
    m_RenderDeviceContext->OMSetRenderTargets(1, m_PpSourceRtv.GetAddressOf(), nullptr);
    setViewport(0, 0, frame->width, frame->height);
    m_RenderDeviceContext->IASetVertexBuffers(0, 1, m_PassVertexBuffer.GetAddressOf(), &stride, &offset);

    bindColorConversion(frameChanged, frame);

    ID3D11ShaderResourceView* frameSrvs[] = { m_VideoTextureResourceViews[srvIndex][0].Get(), m_VideoTextureResourceViews[srvIndex][1].Get() };
    m_RenderDeviceContext->PSSetShaderResources(0, 2, frameSrvs);
    m_RenderDeviceContext->DrawIndexed(6, 0, 0);
    m_RenderDeviceContext->PSSetShaderResources(0, 2, nullSrvs);

    ID3D11ShaderResourceView* rcasInput = m_PpSourceSrv.Get();

    if (mode == PostProcessMode::Fsr) {
        // Pass 2: FSR EASU upscaling to the output resolution
        m_RenderDeviceContext->OMSetRenderTargets(1, m_PpUpscaledRtv.GetAddressOf(), nullptr);
        setViewport(0, 0, m_PpDstRect.w, m_PpDstRect.h);

        m_RenderDeviceContext->PSSetShader(m_EasuPixelShader.Get(), nullptr, 0);
        m_RenderDeviceContext->PSSetConstantBuffers(1, 1, m_EasuConstantBuffer.GetAddressOf());
        m_RenderDeviceContext->PSSetShaderResources(0, 1, m_PpSourceSrv.GetAddressOf());
        m_RenderDeviceContext->DrawIndexed(6, 0, 0);
        m_RenderDeviceContext->PSSetShaderResources(0, 1, nullSrvs);

        rcasInput = m_PpUpscaledSrv.Get();
    }

    // Pass 3: RCAS sharpening into the back buffer
    renderRcasPass(rcasInput);
}

void D3D11VARenderer::renderRcasPass(ID3D11ShaderResourceView* inputSrv)
{
    UINT stride = sizeof(VERTEX);
    UINT offset = 0;
    ID3D11ShaderResourceView* nullSrvs[1] = {};

    // Draw into the video rectangle of the back buffer
    m_RenderDeviceContext->OMSetRenderTargets(1, m_RenderTargetView.GetAddressOf(), nullptr);
    setViewport(0, 0, m_DisplayWidth, m_DisplayHeight);
    m_RenderDeviceContext->IASetVertexBuffers(0, 1, m_VideoVertexBuffer.GetAddressOf(), &stride, &offset);

    m_RenderDeviceContext->PSSetShader(m_RcasPixelShader.Get(), nullptr, 0);
    m_RenderDeviceContext->PSSetConstantBuffers(1, 1, m_RcasConstantBuffer.GetAddressOf());
    m_RenderDeviceContext->PSSetShaderResources(0, 1, &inputSrv);
    m_RenderDeviceContext->DrawIndexed(6, 0, 0);
    m_RenderDeviceContext->PSSetShaderResources(0, 1, nullSrvs);
}
