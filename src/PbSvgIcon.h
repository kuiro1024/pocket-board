#pragma once

#include <QColor>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>

// ============================================================
//  PbSvgIcon（2026-09-18）
//
//  SVG（:/icons/…）を「一度だけ焼いて使い回す」ための共有ヘルパー。
//  DynamicIsland.cpp の cachedTintedSvgPixmap() と同じ考え方で、
//  設定タブ／ヘルプの操作ガイドから使う。
//
//  フレーム更新への配慮:
//    タブは横スクロールのアニメ中（60Hz）に毎フレーム描き直される。
//    その都度 SVG をラスタライズすると DOM のパースは不要でもラスタ化の
//    コストが乗るため、論理px＋DPR＋色をキーにした QPixmap をキャッシュし、
//    描画側は貼るだけにする。拡大方向のスケーリングでボケないよう、
//    キャッシュは 2倍で焼いて縮小して貼る。
// ============================================================
namespace PbSvgIcon {

inline QSvgRenderer *renderer(const QString &resourcePath)
{
    static QHash<QString, QSvgRenderer *> cache;
    auto it = cache.find(resourcePath);
    if (it == cache.end())
        it = cache.insert(resourcePath, new QSvgRenderer(resourcePath));
    return it.value();
}

// color が無効（既定）なら SVG の色のまま。有効なら単色でティントする。
inline const QPixmap &pixmap(const QString &resourcePath, int logicalPx, qreal dpr,
                             const QColor &color = QColor())
{
    constexpr int kSuperSample = 2;
    const int phys = qMax(1, qRound(logicalPx * dpr * kSuperSample));
    const QString key = resourcePath + QLatin1Char('|') + QString::number(phys)
                      + QLatin1Char('|') + (color.isValid() ? color.name(QColor::HexArgb)
                                                            : QStringLiteral("raw"));
    static QHash<QString, QPixmap> cache;
    auto it = cache.find(key);
    if (it != cache.end())
        return it.value();

    QPixmap pm(phys, phys);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        renderer(resourcePath)->render(&p);
        if (color.isValid()) {
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(pm.rect(), color);
        }
    }
    pm.setDevicePixelRatio(dpr * kSuperSample);
    return *cache.insert(key, pm);
}

// rect（論理px）の中央へ収まるように描く。
inline void draw(QPainter &p, const QRectF &rect, const QString &resourcePath,
                 const QColor &color = QColor())
{
    if (resourcePath.isEmpty() || rect.isEmpty())
        return;
    const int side = qMax(1, static_cast<int>(qMin(rect.width(), rect.height())));
    const QPixmap &pm = pixmap(resourcePath, side, p.device()->devicePixelRatioF(), color);
    const QSizeF s(pm.width() / pm.devicePixelRatio(), pm.height() / pm.devicePixelRatio());
    p.drawPixmap(QRectF(rect.center().x() - s.width() / 2.0,
                        rect.center().y() - s.height() / 2.0,
                        s.width(), s.height()),
                 pm, QRectF(QPointF(0, 0), QSizeF(pm.size())));
}

} // namespace PbSvgIcon
