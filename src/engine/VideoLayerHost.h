#pragma once
#include <QObject>
#include <QRectF>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;

class VideoLayerSurface;

// ============================================================
//  engine/VideoLayerHost.h
//
//  D3D11レイヤー分離方式: ステップ1（最小疎通確認）
//
//  アプリ全体で共有する ID3D11Device / ID3D11DeviceContext /
//  IDCompositionDevice / IDCompositionTarget を1箇所に集約するクラス。
//  CanvasWidget::initializeGL() 相当のタイミングで1回だけ initialize() する。
//
//  【重要: CanvasWidgetのHWNDには直接CreateTargetForHwndしない】
//  CanvasWidget（Qt::WA_TranslucentBackgroundの透過ウィンドウ）のHWNDに対して
//  直接 CreateTargetForHwnd すると、そのHWNDの描画パイプラインがDWMのリダイレクション
//  サーフェス経由からDirectCompositionのビジュアルツリー経由に置き換わり、
//  Qtが使っているレイヤードウィンドウ的な透過が失われてウィンドウ全体が不透明になる
//  （実機検証で確認済み）。
//  そのため、CanvasWidgetの子として WS_EX_NOREDIRECTIONBITMAP 付き・
//  WS_EX_LAYERED なしの専用HWNDを新規作成し、そちらに CreateTargetForHwnd する。
//  この子HWNDは常にCanvasWidgetのクライアント領域全体を覆うように追従させる
//  （位置・サイズはCanvasWidget側からのresize通知で更新する）。
//
//  【このステップの範囲】
//    - D3D11デバイス生成
//    - 専用子HWNDの作成とDirectComposition Device/Target生成
//    - VideoLayerSurface の生成・破棄の窓口
//
//  【このステップでやらないこと】
//    - mpv連携（ステップ2以降）
//    - 複数バブル対応（ステップ4）
//    - 状態遷移との連携（ステップ5）
// ============================================================
class VideoLayerHost : public QObject
{
    Q_OBJECT

public:
    explicit VideoLayerHost(QObject *parent = nullptr);
    ~VideoLayerHost() override;

    // parentHwnd: CanvasWidget::winId() を想定。この子として専用HWNDを作成し、
    // そちらに対してDirectCompositionターゲットを作る。
    // parentWidthPx/parentHeightPx: 専用HWNDの初期サイズ（CanvasWidgetの物理ピクセルサイズ）。
    // 成功したら true を返す。失敗時はログを出して false を返す（呼び出し側は
    // 「D3D11レイヤーなし」で継続動作できるようにする）。
    bool initialize(void *parentHwnd, int parentWidthPx, int parentHeightPx);
    bool isInitialized() const { return m_initialized; }

    // CanvasWidgetのリサイズに合わせて専用子HWNDのサイズも追従させる。
    void resizeHostWindow(int widthPx, int heightPx);

    ID3D11Device        *device()        const { return m_device; }
    ID3D11DeviceContext  *context()       const { return m_context; }
    IDCompositionDevice  *compositionDevice() const { return m_dcompDevice; }
    IDCompositionVisual  *rootVisual()    const { return m_rootVisual; }

    // ステップ1用: 固定矩形1枚のテスト用サーフェスを作成する。
    // 生成済みなら既存のものを返す。
    VideoLayerSurface *createTestSurface(const QRectF &screenRect);
    void destroyTestSurface();

    void shutdown();

private:
    bool createHostWindow(void *parentHwnd, int widthPx, int heightPx);
    void destroyHostWindow();

    void *m_hostHwnd     = nullptr; // 専用子HWND（所有）。実体はHWND。

    ID3D11Device         *m_device      = nullptr;
    ID3D11DeviceContext  *m_context     = nullptr;
    IDCompositionDevice  *m_dcompDevice = nullptr;
    IDCompositionTarget  *m_dcompTarget = nullptr;
    IDCompositionVisual  *m_rootVisual  = nullptr;

    std::unique_ptr<VideoLayerSurface> m_testSurface;

    bool m_initialized = false;
};
