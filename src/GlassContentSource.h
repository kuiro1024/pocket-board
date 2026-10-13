#pragma once

#include <QRectF>

// GlassHighlightEffect が「現在バブル本体に表示されているコンテンツ」を
// 直接参照するための軽量な受け渡し情報。テクスチャの所有権は元の
// CanvasItem / player にあり、ガラス側は参照するだけで破棄しない。
struct GlassContentSource
{
    unsigned int textureId {0};
    QRectF uvRect {0.0, 0.0, 1.0, 1.0};
    float opacity {1.0f};

    bool isValid() const
    {
        return textureId != 0
            && uvRect.width() > 0.0
            && uvRect.height() > 0.0
            && opacity > 0.001f;
    }
};
