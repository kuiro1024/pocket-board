#pragma once
#include <QRectF>

struct ID3D11Device;
struct IDCompositionDevice;
struct IDCompositionVisual;
struct IDXGISwapChain1;

// ============================================================
//  engine/VideoLayerSurface.h
//
//  D3D11レイヤー分離方式: ステップ1（最小疎通確認）
//
//  1バブル＝1インスタンス想定（現時点ではステップ1のテスト用サーフェスとしてのみ使用）。
//
//  責務:
//    - IDXGISwapChain1（CreateSwapChainForComposition、
//      DXGI_ALPHA_MODE_PREMULTIPLIED、バッファ数2、DXGI_SCALING_STRETCH）
//    - 対応する IDCompositionVisual（親visualの子として追加）
//    - updateGeometry() で位置・サイズをvisual/スワップチェーンに反映
//
//  【このステップの範囲】
//    - 固定色（赤）で塗りつぶして Present するだけ
//    - mpv連携なし（renderToSwapChain() 相当はステップ2で追加）
// ============================================================
class VideoLayerSurface
{
public:
    VideoLayerSurface(ID3D11Device *device,
                       IDCompositionDevice *dcompDevice,
                       IDCompositionVisual *parentVisual);
    ~VideoLayerSurface();

    // screenRect: CanvasWidget（DirectCompositionターゲットのHWND）のクライアント座標系での矩形。
    // 初回呼び出しでスワップチェーンとvisualを生成し、以降は位置・サイズの変化のみ反映する。
    bool updateGeometry(const QRectF &screenRect);

    // ステップ1用: 赤色で塗りつぶしてPresentする。
    bool paintTestColor();

    void setVisible(bool visible);

    bool isValid() const { return m_swapChain != nullptr; }

private:
    bool ensureSwapChain(int widthPx, int heightPx);
    void releaseSwapChain();

    ID3D11Device         *m_device       = nullptr; // 非所有
    IDCompositionDevice  *m_dcompDevice  = nullptr; // 非所有
    IDCompositionVisual  *m_parentVisual = nullptr; // 非所有

    IDCompositionVisual  *m_visual    = nullptr; // 所有（Release必要）
    IDXGISwapChain1      *m_swapChain = nullptr; // 所有（Release必要）

    int m_widthPx  = 0;
    int m_heightPx = 0;
    QRectF m_lastRect;

    // IDCompositionVisual (このmingw環境で使えるバージョン) には
    // SetOpacity が無いため、可視/不可視は親visualへのAddVisual/RemoveVisualで代用する。
    bool m_visible = true;
};
