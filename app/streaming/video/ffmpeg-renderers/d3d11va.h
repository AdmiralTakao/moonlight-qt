#pragma once

#include "renderer.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <vector>

class D3D11VARenderer : public IFFmpegRenderer
{
public:
    D3D11VARenderer(int decoderSelectionPass);
    virtual ~D3D11VARenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderCapabilities() override;
    virtual InitFailureReason getInitFailureReason() override;
    virtual bool getImageEnhancementStatus(char* buffer, int length) override;

    enum PixelShaders {
        GENERIC_YUV_420,
        GENERIC_AYUV,
        GENERIC_Y410,
        _COUNT
    };

private:
    // GFN fork: post-processing modes for the video image
    enum class PostProcessMode {
        None,        // Original Moonlight rendering path
        SharpenOnly, // YUV->RGB at stream resolution, then RCAS sharpening 1:1
        Fsr,         // YUV->RGB, FSR 1 EASU upscaling, then RCAS sharpening
        Vsr,         // D3D11 video processor with NVIDIA/Intel super resolution, then RCAS
        Mwsr,        // YUV->RGB, then MWSR (one pass, up- or downscaling) into the back buffer
    };

    enum class VsrState {
        Untested,
        Active,
        Failed,
    };

    static void lockContext(void* lock_ctx);
    static void unlockContext(void* lock_ctx);

    // GFN fork: post-processing helpers
    void setupPostProcessingShaders();
    PostProcessMode choosePostProcessMode(AVFrame* frame);
    PostProcessMode preparePostProcessing(AVFrame* frame, bool frameChanged);
    bool setupPostProcessingTargets(AVFrame* frame, PostProcessMode mode);
    void releasePostProcessingTargets();
    bool createRenderTexture(int width, int height, DXGI_FORMAT format,
                             Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                             Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv,
                             Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
    bool setupVideoProcessor(AVFrame* frame);
    bool renderVideoProcessorFrame(AVFrame* frame);
    void renderPostProcessedVideo(AVFrame* frame, UINT srvIndex, bool frameChanged, PostProcessMode mode);
    void renderRcasPass(ID3D11ShaderResourceView* inputSrv);
    void setViewport(int x, int y, int width, int height);
    void applyLowLatencyTweaks();

    bool setupRenderingResources();
    std::vector<DXGI_FORMAT> getVideoTextureSRVFormats();
    bool setupFrameRenderingResources(AVHWFramesContext* framesContext);
    bool setupSwapchainDependentResources();
    bool setupVideoTexture(AVHWFramesContext* framesContext); // for !m_BindDecoderOutputTextures
    bool setupTexturePoolViews(AVHWFramesContext* framesContext); // for m_BindDecoderOutputTextures
    void renderOverlay(Overlay::OverlayType type);
    bool createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, Microsoft::WRL::ComPtr<ID3D11Buffer>& newVertexBuffer);
    void bindColorConversion(bool frameChanged, AVFrame* frame);
    void bindVideoVertexBuffer(bool frameChanged, AVFrame* frame);
    void renderVideo(AVFrame* frame);
    bool checkDecoderSupport(IDXGIAdapter* adapter);
    bool createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound = nullptr);
    bool setupSharedDevice(IDXGIAdapter1* adapter);
    bool createSharedFencePair(UINT64 initialValue,
                               ID3D11Device5* dev1, ID3D11Device5* dev2,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev1Fence,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev2Fence);

    int m_DecoderSelectionPass;
    int m_DevicesWithFL11Support;
    int m_DevicesWithCodecSupport;

    enum class SupportedFenceType {
        None,
        NonMonitored,
        Monitored,
    };

    bool m_DebugLayer;
    Microsoft::WRL::ComPtr<IDXGIFactory5> m_Factory;
    int m_AdapterIndex;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_RenderDevice, m_DecodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_RenderDeviceContext, m_DecodeDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderSharedTextureArray;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_VideoBlendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_OverlayBlendState;

    SupportedFenceType m_FenceType;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeD2RFence, m_RenderD2RFence;
    UINT64 m_D2RFenceValue;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeR2DFence, m_RenderR2DFence;
    UINT64 m_R2DFenceValue;
    SDL_mutex* m_ContextLock;
    bool m_BindDecoderOutputTextures;

    DECODER_PARAMETERS m_DecoderParams;
    DXGI_FORMAT m_TextureFormat;
    int m_DisplayWidth;
    int m_DisplayHeight;
    AVColorTransferCharacteristic m_SwapChainColorTrc;

    bool m_AllowTearing;

    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, PixelShaders::_COUNT> m_VideoPixelShaders;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_VideoVertexBuffer;

    // Only valid if !m_BindDecoderOutputTextures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_VideoTexture;

    // Only index 0 is valid if !m_BindDecoderOutputTextures
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>> m_VideoTextureResourceViews;

    SDL_SpinLock m_OverlayLock;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, Overlay::OverlayMax> m_OverlayVertexBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, Overlay::OverlayMax> m_OverlayTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Overlay::OverlayMax> m_OverlayTextureResourceViews;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_OverlayPixelShader;

    AVBufferRef* m_HwDeviceContext;

    // GFN fork: color conversion constants (kept so they can be rebound after post-processing)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_CscConstantBuffer;

    // GFN fork: post-processing state
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_CflPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_EasuPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasPixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_PassVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_EasuConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_RcasConstantBuffer;

    // GFN fork: MWSR (MoonlightWeb Super Resolution) pixel shaders: upscale and the
    // 2x2, 3x3 and 4x4 reduction grids, plus the state of the active regime
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_MwsrUpPixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_MwsrDown4PixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_MwsrDown9PixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_MwsrDown16PixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_MwsrActivePixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_MwsrConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_MwsrVertexBuffer;
    int m_MwsrTaps; // 0 = upscale, otherwise the reduction grid's samples
    ID3D11PixelShader* getMwsrPixelShader(int frameWidth, int frameHeight, int* taps);

    // RGB image at stream resolution (input of FSR/RCAS)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_PpSourceTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_PpSourceRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_PpSourceSrv;

    // RGB image at output resolution (output of FSR EASU or the video processor)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_PpUpscaledTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_PpUpscaledRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_PpUpscaledSrv;

    PostProcessMode m_PpMode;
    bool m_PpDirty;
    bool m_PpDisabled;

    // GFN fork: image enhancement status for the performance overlay
    void updateImageEnhancementStatus(PostProcessMode mode, AVFrame* frame);
    SDL_SpinLock m_PpStatusLock;
    char m_PpStatus[192];
    unsigned long long m_PpStatusKey;
    int m_PpFrameWidth;
    int m_PpFrameHeight;
    SDL_Rect m_PpDstRect;
    SDL_Rect m_VideoDstRect; // Screen space (bottom-left origin) video rectangle in the window

    // GFN fork: NVIDIA RTX Video Super Resolution / Intel VSR via the D3D11 video processor
    VsrState m_VsrState;
    UINT m_AdapterVendorId;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_VideoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> m_VideoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_VideoProcessorEnumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_VideoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_VideoProcessorOutputView;
    std::vector<Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> m_VideoProcessorInputViews;

    bool m_RaisedProcessPriority;
};

