#include "VideoLayerSurface.h"

#include <QDebug>
#include <QtGlobal>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

namespace {
inline int toPx(qreal v) { return static_cast<int>(qRound(v)); }
}

VideoLayerSurface::VideoLayerSurface(ID3D11Device *device,
                                     IDCompositionDevice *dcompDevice,
                                     IDCompositionVisual *parentVisual)
    : m_device(device)
    , m_dcompDevice(dcompDevice)
    , m_parentVisual(parentVisual)
{
    if (m_dcompDevice) {
        HRESULT hr = m_dcompDevice->CreateVisual(&m_visual);
        if (FAILED(hr)) {
            qWarning() << "VideoLayerSurface: CreateVisual failed hr=" << Qt::hex << hr;
            m_visual = nullptr;
        } else if (m_parentVisual) {
            m_parentVisual->AddVisual(m_visual, FALSE, nullptr);
        }
    }
}

VideoLayerSurface::~VideoLayerSurface()
{
    releaseSwapChain();
    if (m_visual) {
        if (m_parentVisual)
            m_parentVisual->RemoveVisual(m_visual);
        m_visual->Release();
        m_visual = nullptr;
    }
}

void VideoLayerSurface::releaseSwapChain()
{
    if (m_swapChain) {
        m_swapChain->Release();
        m_swapChain = nullptr;
    }
    m_widthPx = 0;
    m_heightPx = 0;
}

bool VideoLayerSurface::ensureSwapChain(int widthPx, int heightPx)
{
    if (widthPx <= 0 || heightPx <= 0)
        return false;

    if (m_swapChain && widthPx == m_widthPx && heightPx == m_heightPx)
        return true;

    if (m_swapChain) {
        // 既存スワップチェーンのサイズだけ変える（ResizeBuffers）
        HRESULT hr = m_swapChain->ResizeBuffers(
            2, static_cast<UINT>(widthPx), static_cast<UINT>(heightPx),
            DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (SUCCEEDED(hr)) {
            m_widthPx = widthPx;
            m_heightPx = heightPx;
            return true;
        }
        qWarning() << "VideoLayerSurface: ResizeBuffers failed hr=" << Qt::hex << hr
                   << "; recreating swap chain";
        releaseSwapChain();
    }

    if (!m_device) {
        qWarning() << "VideoLayerSurface: no D3D11 device";
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    HRESULT hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        qWarning() << "VideoLayerSurface: QueryInterface(IDXGIDevice) failed hr=" << Qt::hex << hr;
        return false;
    }

    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) {
        qWarning() << "VideoLayerSurface: GetAdapter failed hr=" << Qt::hex << hr;
        dxgiDevice->Release();
        return false;
    }

    IDXGIFactory2 *factory2 = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2));
    adapter->Release();
    dxgiDevice->Release();
    if (FAILED(hr) || !factory2) {
        qWarning() << "VideoLayerSurface: GetParent(IDXGIFactory2) failed hr=" << Qt::hex << hr;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width       = static_cast<UINT>(widthPx);
    desc.Height      = static_cast<UINT>(heightPx);
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo      = FALSE;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags       = 0;

    IDXGISwapChain1 *swapChain = nullptr;
    hr = factory2->CreateSwapChainForComposition(m_device, &desc, nullptr, &swapChain);
    factory2->Release();
    if (FAILED(hr) || !swapChain) {
        qWarning() << "VideoLayerSurface: CreateSwapChainForComposition failed hr=" << Qt::hex << hr;
        return false;
    }

    m_swapChain = swapChain;
    m_widthPx = widthPx;
    m_heightPx = heightPx;

    if (m_visual) {
        HRESULT hrContent = m_visual->SetContent(m_swapChain);
        if (FAILED(hrContent))
            qWarning() << "VideoLayerSurface: SetContent failed hr=" << Qt::hex << hrContent;
    }

    return true;
}

bool VideoLayerSurface::updateGeometry(const QRectF &screenRect)
{
    if (m_lastRect == screenRect && m_swapChain)
        return true;

    const int w = toPx(screenRect.width());
    const int h = toPx(screenRect.height());
    if (!ensureSwapChain(w, h))
        return false;

    if (m_visual) {
        m_visual->SetOffsetX(static_cast<float>(screenRect.left()));
        m_visual->SetOffsetY(static_cast<float>(screenRect.top()));
    }

    if (m_dcompDevice)
        m_dcompDevice->Commit();

    m_lastRect = screenRect;
    return true;
}

bool VideoLayerSurface::paintTestColor()
{
    if (!m_swapChain || !m_device)
        return false;

    ID3D11DeviceContext *context = nullptr;
    m_device->GetImmediateContext(&context);
    if (!context)
        return false;

    ID3D11Texture2D *backBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        qWarning() << "VideoLayerSurface: GetBuffer failed hr=" << Qt::hex << hr;
        context->Release();
        return false;
    }

    ID3D11RenderTargetView *rtv = nullptr;
    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    if (FAILED(hr) || !rtv) {
        qWarning() << "VideoLayerSurface: CreateRenderTargetView failed hr=" << Qt::hex << hr;
        context->Release();
        return false;
    }

    // ステップ1確認用: 赤（不透明）で塗りつぶす
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(rtv, red);
    rtv->Release();
    context->Release();

    hr = m_swapChain->Present(1, 0);
    if (FAILED(hr)) {
        qWarning() << "VideoLayerSurface: Present failed hr=" << Qt::hex << hr;
        return false;
    }

    return true;
}

void VideoLayerSurface::setVisible(bool visible)
{
    // このmingw環境のdcomp.hはIDCompositionVisual2までしか定義されておらず、
    // SetOpacity（IDCompositionVisual3以降）が使えない。
    // 代わりに親visualへのAddVisual/RemoveVisualで可視/不可視を代用する。
    if (!m_visual || !m_parentVisual || visible == m_visible)
        return;

    if (visible) {
        m_parentVisual->AddVisual(m_visual, FALSE, nullptr);
    } else {
        m_parentVisual->RemoveVisual(m_visual);
    }
    m_visible = visible;
}
