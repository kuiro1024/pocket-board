#include "VideoLayerHost.h"
#include "VideoLayerSurface.h"

#include <QDebug>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

namespace {

const wchar_t *kHostWindowClassName = L"PocketBoardVideoLayerHostWnd";

// 専用子HWND用のウィンドウクラスを1回だけ登録する。
bool ensureHostWindowClassRegistered(HINSTANCE hInstance)
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kHostWindowClassName;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            qWarning() << "VideoLayerHost: RegisterClassExW failed err=" << err;
            return false;
        }
    }
    registered = true;
    return true;
}

} // namespace

VideoLayerHost::VideoLayerHost(QObject *parent)
    : QObject(parent)
{
}

VideoLayerHost::~VideoLayerHost()
{
    shutdown();
}

bool VideoLayerHost::createHostWindow(void *parentHwnd, int widthPx, int heightPx)
{
    HWND parent = static_cast<HWND>(parentHwnd);
    HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));

    if (!ensureHostWindowClassRegistered(hInstance))
        return false;

    if (widthPx <= 0) widthPx = 1;
    if (heightPx <= 0) heightPx = 1;

    // WS_EX_NOREDIRECTIONBITMAP: DWMのリダイレクションサーフェスを持たせず、
    //   DirectCompositionのビジュアルツリーのみで描画させる（公式サンプルと同じ構成）。
    // WS_EX_LAYERED は付けない（DirectCompositionと非互換のため）。
    // WS_EX_TRANSPARENT: マウス/タッチ入力をこの子ウィンドウで奪わず、下（実際には重なる
    //   対象がないため実質親）に透過させる。CanvasWidgetの既存のマウス操作を邪魔しないため。
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT;
    DWORD style   = WS_CHILD | WS_VISIBLE;

    HWND hostHwnd = CreateWindowExW(
        exStyle,
        kHostWindowClassName,
        L"",
        style,
        0, 0, widthPx, heightPx,
        parent,
        nullptr,
        hInstance,
        nullptr);

    if (!hostHwnd) {
        qWarning() << "VideoLayerHost: CreateWindowExW failed err=" << GetLastError();
        return false;
    }

    m_hostHwnd = hostHwnd;
    return true;
}

void VideoLayerHost::destroyHostWindow()
{
    if (m_hostHwnd) {
        DestroyWindow(static_cast<HWND>(m_hostHwnd));
        m_hostHwnd = nullptr;
    }
}

bool VideoLayerHost::initialize(void *parentHwnd, int parentWidthPx, int parentHeightPx)
{
    if (m_initialized)
        return true;

    if (!parentHwnd) {
        qWarning() << "VideoLayerHost::initialize: parentHwnd is null";
        return false;
    }

    // ---- 専用子HWNDの作成 ----
    // CanvasWidget（Qt::WA_TranslucentBackgroundの透過ウィンドウ）のHWNDに直接
    // CreateTargetForHwndすると、Qt側のレイヤードウィンドウ的透過と競合し、
    // ウィンドウ全体の透過が失われる（実機検証で確認済み）。
    // そのため専用の子HWNDを作り、そちらにDirectCompositionターゲットを作る。
    //
    // 【重要・未解決】実機切り分けの結果、この子HWND化やBGRA_SUPPORTフラグの有無に
    // 関わらず、D3D11CreateDevice(D3D_DRIVER_TYPE_HARDWARE)を呼び出すことそのものが
    // Qtアプリ全体の透過を破壊することが判明した（単一GPU環境のためアダプター不一致説は
    // 除外済み）。原因未特定。詳細は d3d11_layer_separation_design_2026-07-01.md を参照。
    // このファイルは現在 CanvasWidget から呼ばれていない（無効化中）。
    if (!createHostWindow(parentHwnd, parentWidthPx, parentHeightPx)) {
        shutdown();
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // pAdapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // Software
        flags,
        nullptr, 0,                 // FeatureLevels（既定順）
        D3D11_SDK_VERSION,
        &m_device,
        nullptr,                    // pFeatureLevel（不要）
        &m_context);

    if (FAILED(hr) || !m_device) {
        qWarning() << "VideoLayerHost: D3D11CreateDevice failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        qWarning() << "VideoLayerHost: QueryInterface(IDXGIDevice) failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice),
                                   reinterpret_cast<void**>(&m_dcompDevice));
    dxgiDevice->Release();
    if (FAILED(hr) || !m_dcompDevice) {
        qWarning() << "VideoLayerHost: DCompositionCreateDevice failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    hr = m_dcompDevice->CreateTargetForHwnd(static_cast<HWND>(m_hostHwnd), TRUE, &m_dcompTarget);
    if (FAILED(hr) || !m_dcompTarget) {
        qWarning() << "VideoLayerHost: CreateTargetForHwnd failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    hr = m_dcompDevice->CreateVisual(&m_rootVisual);
    if (FAILED(hr) || !m_rootVisual) {
        qWarning() << "VideoLayerHost: CreateVisual(root) failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    hr = m_dcompTarget->SetRoot(m_rootVisual);
    if (FAILED(hr)) {
        qWarning() << "VideoLayerHost: SetRoot failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    hr = m_dcompDevice->Commit();
    if (FAILED(hr)) {
        qWarning() << "VideoLayerHost: initial Commit failed hr=" << Qt::hex << hr;
        shutdown();
        return false;
    }

    m_initialized = true;
    qInfo() << "VideoLayerHost: initialized (D3D11 + DirectComposition, dedicated child HWND)";
    return true;
}

void VideoLayerHost::resizeHostWindow(int widthPx, int heightPx)
{
    if (!m_hostHwnd)
        return;
    if (widthPx <= 0) widthPx = 1;
    if (heightPx <= 0) heightPx = 1;

    SetWindowPos(static_cast<HWND>(m_hostHwnd), nullptr, 0, 0, widthPx, heightPx,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

VideoLayerSurface *VideoLayerHost::createTestSurface(const QRectF &screenRect)
{
    if (!m_initialized)
        return nullptr;

    if (!m_testSurface)
        m_testSurface = std::make_unique<VideoLayerSurface>(m_device, m_dcompDevice, m_rootVisual);

    m_testSurface->updateGeometry(screenRect);
    m_testSurface->paintTestColor();
    return m_testSurface.get();
}

void VideoLayerHost::destroyTestSurface()
{
    m_testSurface.reset();
}

void VideoLayerHost::shutdown()
{
    m_testSurface.reset();

    if (m_rootVisual) {
        m_rootVisual->Release();
        m_rootVisual = nullptr;
    }
    if (m_dcompTarget) {
        m_dcompTarget->Release();
        m_dcompTarget = nullptr;
    }
    if (m_dcompDevice) {
        m_dcompDevice->Release();
        m_dcompDevice = nullptr;
    }
    if (m_context) {
        m_context->Release();
        m_context = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }

    destroyHostWindow();

    m_initialized = false;
}
