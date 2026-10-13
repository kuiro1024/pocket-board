#include "ThumbnailLoader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QSize>
#include <QtGlobal>
#include <QDebug>
#include <QThreadPool>
#include <QMetaObject>
#include <QDir>
#include <QPointer>
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <shobjidl.h>
#include <windows.h>
#endif

namespace {
// サムネイル（Sleep/AudioOnly/Hidden系のfallback表示・セッション保存用の
// プレビュー画像）は、動画サイトから取得すると1280x720前後で降ってくることが
// 多いが、バブルの実表示サイズはそれよりずっと小さいことがほとんど。
// 長辺がこれを超える場合のみ縮小し、CPU側・GPU側双方のメモリと、
// セッションJSONへ保存する際のサイズを削減する。
// 480pのquality()既定値より少し余裕を持たせた値。
// （CanvasVideo.cpp の kMaxThumbnailDimension から移設。値は同一。）
constexpr int kMaxThumbnailDimension = 640;
}

ThumbnailLoader::ThumbnailLoader(QObject *parent)
    : QObject(parent)
{
}

QImage ThumbnailLoader::downscaleThumbnailIfNeeded(const QImage &src)
{
    const int longSide = qMax(src.width(), src.height());
    if (longSide <= kMaxThumbnailDimension || longSide <= 0)
        return src;
    return src.scaled(QSize(kMaxThumbnailDimension, kMaxThumbnailDimension),
                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage ThumbnailLoader::fetchLocalThumbnailBlocking(const QString &filePath)
{
#ifdef Q_OS_WIN
    return filePath.isEmpty() ? QImage() : fetchWindowsShellThumbnail(filePath);
#else
    Q_UNUSED(filePath);
    return QImage();
#endif
}

void ThumbnailLoader::requestThumbnail(const QString &url)
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QUrl qurl(url);
    QNetworkRequest req{qurl};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("[Thumb] DL失敗: %s", qPrintable(reply->errorString()));
            return;
        }
        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            qWarning("[Thumb] 画像パース失敗");
            return;
        }
        const QImage scaled = downscaleThumbnailIfNeeded(img);
        const QImage converted = scaled.convertToFormat(QImage::Format_RGBA8888);
        qDebug("[Thumb] DL完了: %dx%d → 保持サイズ %dx%d",
               img.width(), img.height(), scaled.width(), scaled.height());
        emit thumbnailReady(converted);
    });
}

// ================================================================
//  2026-07-08: ローカル動画ファイル用サムネイル（Windows Explorer共有）
// ================================================================
void ThumbnailLoader::requestLocalThumbnail(const QString &filePath)
{
    if (filePath.isEmpty())
        return;
#ifdef Q_OS_WIN
    // GetImage()はブロッキング呼び出しで、Explorer未参照のファイルだと
    // 初回サムネイル生成のため数百ms～数秒かかりうる。UIスレッド/GLスレッドを
    // 塞がないよう QThreadPool のワーカースレッドで実行する。
    //
    // 2026-07-08: 生の this ポインタをワーカースレッド側の終了処理まで
    // 持ち越さない。fetchWindowsShellThumbnail()自体はstatic（ThumbnailLoader
    // の状態に一切触れない）なので直接呼べるが、その後のコールバック発行に
    // thisを使う場合は要注意: バックグラウンド処理中（数百ms～数秒）に
    // CanvasVideo/ThumbnailLoaderがバブル削除等で破棄される可能性がある。
    // QMetaObject::invokeMethod(context, ...)の「contextが破棄済みなら
    // 自動キャンセル」という安全機構は、あくまで呼び出しをキューに積んだ後に
    // 破棄された場合の話であり、積む時点で既にcontextが破棄済みだと
    // そのポインタを読むこと自体が未定義動作になる。そこでQPointer
    // （CanvasVideo::resolveViaPlugin()のguardと同じ既存パターン）で弱参照し、
    // 生存確認は必ずGUIスレッド側（qApp/QCoreApplication::instance()を
    // contextにしたキュー済みラムダの中）で行う。
    QPointer<ThumbnailLoader> guard(this);
    QThreadPool::globalInstance()->start([guard, filePath]() {
        const QImage img = ThumbnailLoader::fetchWindowsShellThumbnail(filePath);
        if (img.isNull())
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, img]() {
            if (guard)
                emit guard->thumbnailReady(img);
        }, Qt::QueuedConnection);
    });
#else
    Q_UNUSED(filePath);
#endif
}

#ifdef Q_OS_WIN
namespace {
// HBITMAP（32bpp DIB、Explorerサムネイルは通常アルファ乗算済み）を
// QImageへ変換する。GetDIBits()でトップダウンDIBとして直接
// QImage::bits()へ書き込む。
QImage hbitmapToImage(HBITMAP hBitmap)
{
    BITMAP bmpInfo{};
    if (!GetObject(hBitmap, sizeof(BITMAP), &bmpInfo))
        return QImage();
    const int w = bmpInfo.bmWidth;
    const int h = bmpInfo.bmHeight;
    if (w <= 0 || h <= 0)
        return QImage();

    BITMAPINFOHEADER bi{};
    bi.biSize        = sizeof(BITMAPINFOHEADER);
    bi.biWidth        = w;
    bi.biHeight       = -h; // 負値 = トップダウンDIB（QImageと同じ走査順）
    bi.biPlanes       = 1;
    bi.biBitCount     = 32;
    bi.biCompression  = BI_RGB;

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
        return QImage();

    HDC hdc = GetDC(nullptr);
    const int copied = GetDIBits(hdc, hBitmap, 0, h, img.bits(),
                                  reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (copied == 0)
        return QImage();

    // Format_ARGB32_Premultiplied → 非乗算のRGBA8888へ正規化しておく
    // （downscaleThumbnailIfNeeded()のscaled()やGL側テクスチャアップロードで
    // 非乗算前提の他経路（ダウンロードサムネイル等）と扱いを揃えるため）。
    return img.convertToFormat(QImage::Format_RGBA8888);
}
}

QImage ThumbnailLoader::fetchWindowsShellThumbnail(const QString &filePath)
{
    // Windowsのパス区切りに正規化しておく（QDir::toNativeSeparatorsは
    // "/" → "\" 変換のみ行い、それ以外はそのまま）。
    const QString nativePath = QDir::toNativeSeparators(filePath);

    // QThreadPoolのワーカースレッドにはCOMが初期化されていないため、
    // このタスクの間だけ明示的に初期化する。RPC_E_CHANGED_MODEは
    // 「このスレッドは既に別の並行モデルで初期化済み」を意味し、その場合は
    // 自分でCoUninitialize()を呼んではいけない（呼び出し順のペアが崩れる）。
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        qWarning("[Thumb][Local] CoInitializeEx失敗: 0x%08lX", static_cast<unsigned long>(initHr));
        return QImage();
    }

    QImage result;
    IShellItem *item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(
        reinterpret_cast<const wchar_t*>(nativePath.utf16()), nullptr,
        IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr) && item) {
        IShellItemImageFactory *factory = nullptr;
        hr = item->QueryInterface(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr) && factory) {
            HBITMAP hBitmap = nullptr;
            const SIZE size{256, 256};
            // SIIGBF_THUMBNAILONLY: 汎用アイコンへのフォールバックを禁止し、
            // 「本物のサムネイルが無いなら失敗させる」。ここで拾えなかった
            // 場合、呼び出し側（ThumbnailLoader::requestLocalThumbnail）は
            // 何もせず終わる＝既存のSleep等のプレースホルダ表示（サムネ無し
            // なら真っ黒等）にフォールバックする形になる。
            hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hBitmap);
            if (SUCCEEDED(hr) && hBitmap) {
                result = hbitmapToImage(hBitmap);
                DeleteObject(hBitmap);
            } else {
                qDebug("[Thumb][Local] GetImage失敗またはサムネイル無し: %s hr=0x%08lX",
                       qPrintable(filePath), static_cast<unsigned long>(hr));
            }
            factory->Release();
        }
        item->Release();
    } else {
        qDebug("[Thumb][Local] SHCreateItemFromParsingName失敗: %s hr=0x%08lX",
               qPrintable(filePath), static_cast<unsigned long>(hr));
    }

    if (needUninit)
        CoUninitialize();

    if (result.isNull())
        return result;

    const QImage scaled = ThumbnailLoader::downscaleThumbnailIfNeeded(result);
    qDebug("[Thumb][Local] 取得完了: %dx%d → 保持サイズ %dx%d",
           result.width(), result.height(), scaled.width(), scaled.height());
    return scaled;
}
#endif
