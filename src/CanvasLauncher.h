#pragma once

#include "CanvasItem.h"
#include "Renderer.h"
#include <QPixmap>
#include <QString>

class CanvasLauncher : public CanvasItem
{
public:
    enum class TargetKind {
        File,
        Directory,
        Shortcut,
        Web          // 2026-10-11: http(s) のWebリンク。launch()は既定ブラウザで開く
    };

    CanvasLauncher(const QString &path, TargetKind kind, const QPixmap &icon,
                   const QPointF &pos, double scale);
    ~CanvasLauncher() override;

    QRectF itemRect() const override;
    void paintGL(Renderer &r, const QRectF &screenRect,
                 bool isActive, double pulseAlpha,
                 double screenScale,
                 double contentCornerFactor = 1.0) override;
    double boardBezelRatio() const override {
        return LAUNCHER_BEZEL_RATIO;
    }
    bool supportsChromeLayer() const override { return true; }
    QColor contentAverageColor() const override;
    void paintChromeGL(Renderer &r, const QRectF &screenRect, bool isActive,
                       double pulseAlpha, double screenScale,
                       double contentCornerFactor = 1.0) override;
    // 2026-10-11: グミのガラスがアイコンの色をにじませるために使う
    // （CanvasWidget はグミのときだけランチャーのこれをガラスへ渡す）。
    GlassContentSource glassContentSource() const override {
        if (m_texId == 0)
            return {};
        return {m_texId, QRectF(0.0, 0.0, 1.0, 1.0), 1.0f};
    }

    QString path() const { return m_path; }
    QString displayName() const { return m_displayName; }
    TargetKind targetKind() const { return m_kind; }
    const QPixmap &icon() const { return m_icon; }
    bool isWebLink() const { return m_kind == TargetKind::Web; }
    // Webリンクのfavicon取得完了時などにアイコンを差し替える（GLテクスチャは次回描画で作り直す）。
    void setIcon(const QPixmap &icon);

    bool launch() const;
    bool isLaunchable() const;

    static bool isLauncherCandidate(const QString &path);
    // ドロップ/ペーストされたURL文字列が「Webリンクバブルにすべきサイト」か。
    // http(s)であり、かつ動画サイト（プラグイン対応ホスト）や動画/音声ファイルの
    // 直リンクではないもの。動画系は従来どおり動画バブル側（または無視）に任せる。
    static bool isWebLinkCandidate(const QString &url);
    static bool validateTarget(const QString &path);
    static TargetKind targetKindForPath(const QString &path);
    static QPixmap iconForPath(const QString &path, int size = 128);
    static QString displayNameForPath(const QString &path);
    static QString resolveShortcutTarget(const QString &path);

private:
    QString m_path;
    QString m_displayName;
    TargetKind m_kind {TargetKind::File};
    QPixmap m_icon;
    // 2026-10-12: 平均色のキャッシュ（QPixmap::cacheKey が変わったら作り直す）
    mutable QColor m_avgColor;
    mutable qint64 m_avgColorKey = -1;
    GLuint m_texId {0};
    bool m_iconDirty {false};
};
