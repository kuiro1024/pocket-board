#pragma once

#include <QString>

enum class MediaKind
{
    LocalVideo,
    LocalAudio,
    LocalImage,
    WebImage,
    Shortcut,
    // Phase 8: Plugin API 経由で解決されるメディア。
    // originalPathOrUrl は「元のページURL」（例: 動画サイトのページURL）を保持し、
    // 実際の再生用ストリームURLはプラグインが非同期に解決する。
    PluginMedia,
    Unknown,
    // 2026-10-12: Web上の動画ファイル直リンク（素材サイト等の .mp4 等）。
    // originalPathOrUrl は http(s) のURL。再生はURLから直接（ダウンロードしながら）行い、
    // 並行して webvideo/ フォルダへ保存する（WebVideoCache.h 参照）。
    // プラグインAPI（PbResolvedMedia::kind）でも使われる列挙なので、既存の値の
    // 番号が変わらないよう末尾（Unknown の後）に足している。
    WebVideo
};

struct MediaSource
{
    MediaKind kind = MediaKind::Unknown;
    QString originalPathOrUrl;
    QString title;
    QString thumbnailPathOrUrl;

    // Phase 8: kind == PluginMedia の場合、実際に解決に成功した
    // プラグインの pluginId()（例: "com.example.myplugin"）。
    // あくまで任意のヒント情報であり、本体側はこれを見て処理を
    // 分岐したりはしない。セッション復元時に対応プラグインが
    // 見つからなかった場合のエラーメッセージ表示にのみ使う。
    QString resolverPluginId;
};

inline QString mediaKindToString(MediaKind kind)
{
    switch (kind) {
    case MediaKind::LocalVideo: return QStringLiteral("localVideo");
    case MediaKind::LocalAudio: return QStringLiteral("localAudio");
    case MediaKind::LocalImage: return QStringLiteral("localImage");
    case MediaKind::WebImage:   return QStringLiteral("webImage");
    case MediaKind::Shortcut:   return QStringLiteral("shortcut");
    case MediaKind::PluginMedia: return QStringLiteral("pluginMedia");
    case MediaKind::WebVideo:   return QStringLiteral("webVideo");
    case MediaKind::Unknown:    return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

inline MediaKind mediaKindFromString(const QString &value)
{
    if (value == QStringLiteral("localVideo"))
        return MediaKind::LocalVideo;
    if (value == QStringLiteral("localAudio"))
        return MediaKind::LocalAudio;
    if (value == QStringLiteral("localImage"))
        return MediaKind::LocalImage;
    if (value == QStringLiteral("webImage"))
        return MediaKind::WebImage;
    if (value == QStringLiteral("shortcut"))
        return MediaKind::Shortcut;
    if (value == QStringLiteral("pluginMedia"))
        return MediaKind::PluginMedia;
    if (value == QStringLiteral("webVideo"))
        return MediaKind::WebVideo;
    return MediaKind::Unknown;
}
