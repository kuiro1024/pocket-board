#include "MpvTeardown.h"
#include "engine/MpvPlayer.h"
#include "CanvasWidget.h"
#include "MainWindow.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QApplication>
#include <QWidget>

// 詳細な経緯は MpvTeardown.h のコメント参照。
void destroyMpvPlayer(QObject *owner, MpvPlayer *&player, bool releaseGlBeforeDelete)
{
    if (!player)
        return;

    MpvPlayer *doomed = player;
    player = nullptr;

    // 2026-09-15 非同期化 P2: 非同期プレイヤーの terminate_destroy はワーカーで
    // 後から完了する。完了時にも OLE ドロップターゲット修復を走らせる（下の即時
    // 修復は従来どおり残す）。フックは一度だけ設定すればよい。
    static bool asyncHookInstalled = false;
    if (!asyncHookInstalled) {
        asyncHookInstalled = true;
        MpvPlayer::setAfterAsyncTeardownHook([]() {
#ifdef Q_OS_WIN
            const auto tops = QApplication::topLevelWidgets();
            for (QWidget *w : tops) {
                if (auto *mw = qobject_cast<MainWindow*>(w))
                    mw->refreshDropTargets();
            }
#endif
        });
    }

    // 2026-09-15: [MpvLifecycle] 計測。破棄1回の内訳
    // （FBO解放 / delete本体 / D&D修復）を出す。delete本体の内訳は
    // ~MpvPlayer() の [MpvLifecycle][dtor] 行を参照。
    QElapsedTimer __total;
    __total.start();
    QElapsedTimer __clock;

    qint64 __releaseMs = -1; // -1 = releaseGlBeforeDelete=false（呼んでいない）
    if (releaseGlBeforeDelete) {
        __clock.start();
        doomed->releaseVideoResources();
        __releaseMs = __clock.elapsed();
    }

    __clock.start();
    delete doomed; // ~MpvPlayer() 側で破棄前後の診断ログを出力する。
    const qint64 __deleteMs = __clock.elapsed();

    qint64 __refreshMs = -1; // -1 = 修復処理を呼んでいない
#ifdef Q_OS_WIN
    // OLEアパートメントが巻き添えで壊れていた場合の修復。破棄経路ごとに
    // 個別対応する運用に戻さないよう、ここで一元的に必ず呼ぶ。
    if (auto *canvas = qobject_cast<CanvasWidget*>(owner ? owner->parent() : nullptr)) {
        if (auto *mw = qobject_cast<MainWindow*>(canvas->window())) {
            __clock.start();
            mw->refreshDropTargets();
            __refreshMs = __clock.elapsed();
        }
    }
#else
    Q_UNUSED(owner);
#endif

    qInfo("[MpvLifecycle][teardown] player=%p releaseVideoResources=%lldms delete=%lldms refreshDropTargets=%lldms total=%lldms",
          static_cast<void*>(doomed), (long long)__releaseMs, (long long)__deleteMs,
          (long long)__refreshMs, (long long)__total.elapsed());
}
