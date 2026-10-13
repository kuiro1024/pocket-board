// ============================================================
//  元に戻す / やり直し（2026-09-17）
//
//  方式: 「操作の前のキャンバスの形」をスナップショットとして積む。
//    - 形 = 各バブルの位置/スケール/重なり順。軽いので操作ごとに丸ごと取る。
//      （フレーム・Flowのつなぎ線は 2026-09-23 に本体から外した。old_fn/ 参照）
//    - バブルは生ポインタではなく履歴用の通し番号で持つ。
//      削除→作り直しでポインタが変わっても同じ番号に結び直す。
//    - 作り直しに必要な中身（動画URL・再生位置・サムネ等）はセッション保存と
//      同じ JSON（sessionEntryForItem）で、destroyItem() の直前に1回だけ取り、
//      番号ごとに1つだけ持つ（スナップショットごとに複製しない）。
//
//  適用は差分: 今あって目標に無いバブルは destroyItem()、目標にあって今
//  無いバブルは createItemFromSessionEntry() で作り直し、残りは位置と
//  スケールを書き戻すだけ。移動の取り消しで再生中の動画が止まったり
//  作り直されたりはしない。
//
//  ステップの区切り:
//    - マウス: 最初のボタンを押した瞬間に開始、全ボタンを離した時点で確定。
//      形が変わっていなければ（クリックだけ・カメラ移動だけ）積まない。
//    - 追加系（ドロップ等）: UndoAutoStep で関数の入口/出口を囲む。
//  ステップ中に走り始めた押し出しアニメは「行き着く先」で記録する。
// ============================================================
#include "CanvasWidget.h"
#include "AppSettings.h"
#include "CanvasImage.h"
#include "MainWindow.h"

#include <QDebug>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QSet>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <ole2.h>
#endif

namespace {

bool nearlyEqual(double a, double b)
{
    return qAbs(a - b) <= 0.01;
}

bool nearlyEqual(const QPointF &a, const QPointF &b)
{
    return nearlyEqual(a.x(), b.x()) && nearlyEqual(a.y(), b.y());
}

bool nearlyEqual(const QRectF &a, const QRectF &b)
{
    return nearlyEqual(a.x(), b.x()) && nearlyEqual(a.y(), b.y())
        && nearlyEqual(a.width(), b.width()) && nearlyEqual(a.height(), b.height());
}

} // namespace

// ------------------------------------------------------------
//  RAII
// ------------------------------------------------------------
CanvasWidget::UndoAutoStep::UndoAutoStep(CanvasWidget *w)
    : widget(w)
    , began(false)
{
    if (!widget || widget->m_undoSuppressDepth > 0)
        return;
    // 外部ドラッグ（Shift+ドラッグで書き出し）などで release を取りこぼし、
    // マウスはもう離れているのにステップが開いたままなら、先に確定させる。
    if (widget->m_undoStepOpen && QGuiApplication::mouseButtons() == Qt::NoButton
        && widget->m_activeMode.isEmpty())
        widget->commitUndoStep();
    if (widget->m_undoStepOpen)
        return;   // マウス操作中など、外側のステップにまとめる
    began = true;
    widget->beginUndoStep();
}

CanvasWidget::UndoAutoStep::~UndoAutoStep()
{
    if (began)
        widget->commitUndoStep();
}

// ------------------------------------------------------------
//  番号
// ------------------------------------------------------------
quint64 CanvasWidget::undoIdForItem(CanvasItem *item)
{
    if (!item)
        return 0;
    auto it = m_undoIdByItem.constFind(item);
    if (it != m_undoIdByItem.constEnd())
        return it.value();
    const quint64 id = m_undoNextId++;
    bindUndoItemId(item, id);
    return id;
}

void CanvasWidget::bindUndoItemId(CanvasItem *item, quint64 id)
{
    if (!item || id == 0)
        return;
    m_undoIdByItem.insert(item, id);
    m_undoItemById.insert(id, item);
}


void CanvasWidget::forgetUndoItem(CanvasItem *item)
{
    auto it = m_undoIdByItem.find(item);
    if (it == m_undoIdByItem.end())
        return;
    const quint64 id = it.value();
    // 作り直し用の中身をここで取る（削除直前＝再生位置なども最新）。
    // 保存対象外のバブル（空）は作り直せないので持たない。
    const QJsonObject entry = sessionEntryForItem(item);
    if (!entry.isEmpty())
        m_undoItemBlobs.insert(id, entry);
    else
        m_undoItemBlobs.remove(id);
    m_undoItemById.remove(id);
    m_undoIdByItem.erase(it);
}


// ------------------------------------------------------------
//  スナップショット
// ------------------------------------------------------------
CanvasWidget::UndoSnapshot CanvasWidget::captureUndoSnapshot()
{
    UndoSnapshot snap;

    // 押し出しアニメの途中なら、行き着く先の位置で記録する。
    // （途中の位置を記録すると、戻したときに中途半端な位置で止まる）
    QHash<CanvasItem*, QPointF> itemFinalPos;
    for (auto it = m_overlapAnims.constBegin(); it != m_overlapAnims.constEnd(); ++it) {
        if (it.key() && it.value())
            itemFinalPos.insert(it.key(), it.value()->endValue().toPointF());
    }

    snap.items.reserve(m_items.size());
    for (CanvasItem *item : m_items) {
        if (!item)
            continue;
        UndoItemState st;
        st.id = undoIdForItem(item);
        st.pos = itemFinalPos.value(item, item->pos());
        // 2026-09-23: バブル融合で吸い込まれている途中のバブルは、掴む前の
        // 位置で記録する。離した時点の「移動」を1ステップにせず、吸い込みを
        // 終えたときの1ステップ（finishFusionAbsorb）だけで元に戻せるように。
        if (m_fusion.absorbing() && item == m_fusion.pairDragged())
            st.pos = m_fusionAbsorbRestorePos;
        st.scale = item->scale();
        if (auto *video = canvasItemAsVideo(item)) {
            st.coreListKey = video->coreListIdentityKey();
            if (!st.coreListKey.isEmpty())
                st.coreList = video->coreListToJson(/*includeIndex=*/false);
            // 2026-09-23: ラベルの長押しによるURLの切り替え（Solo → Mix 等）。
            st.holdEpoch = video->labelHoldEpoch();
            st.holdUrl   = video->sourceUrl();
            st.holdLabel = video->pluginLabelToJson();
        }
        snap.items.append(st);
    }

    return snap;
}

bool CanvasWidget::undoSnapshotsEqual(const UndoSnapshot &a, const UndoSnapshot &b)
{
    // 重なり順だけの変化（クリックで前面へ出しただけ）はステップにしない。
    // そのため items は並びではなく番号で突き合わせる。
    if (a.items.size() != b.items.size())
        return false;
    QHash<quint64, const UndoItemState*> bItems;
    bItems.reserve(b.items.size());
    for (const UndoItemState &st : b.items)
        bItems.insert(st.id, &st);
    for (const UndoItemState &st : a.items) {
        const UndoItemState *other = bItems.value(st.id, nullptr);
        if (!other || !nearlyEqual(st.pos, other->pos) || qAbs(st.scale - other->scale) > 1e-6)
            return false;
        if (st.coreListKey != other->coreListKey)   // 2026-09-23: 本体のリスト
            return false;
        if (st.holdEpoch != other->holdEpoch)       // 2026-09-23: ラベルの長押し
            return false;
    }
    return true;
}

// ------------------------------------------------------------
//  ステップの開始 / 確定
// ------------------------------------------------------------
void CanvasWidget::beginUndoStep()
{
    if (m_undoSuppressDepth > 0)
        return;
    if (m_undoStepOpen)
        commitUndoStep();
    m_undoPendingBefore = captureUndoSnapshot();
    m_undoStepOpen = true;
}

void CanvasWidget::commitUndoStep()
{
    if (!m_undoStepOpen)
        return;
    m_undoStepOpen = false;
    UndoSnapshot before = std::move(m_undoPendingBefore);
    m_undoPendingBefore = UndoSnapshot();
    if (m_undoSuppressDepth > 0)
        return;

    const UndoSnapshot after = captureUndoSnapshot();
    if (undoSnapshotsEqual(before, after)) {
        pruneUndoBlobs();
        return;
    }
    m_undoStack.append(std::move(before));
    m_redoStack.clear();
    trimUndoHistory();
    pruneUndoBlobs();
    emit sessionContentChanged();
}

void CanvasWidget::clearUndoHistory()
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_undoStepOpen = false;
    m_undoPendingBefore = UndoSnapshot();
    m_undoIdByItem.clear();
    m_undoItemById.clear();
    m_undoItemBlobs.clear();
}

void CanvasWidget::trimUndoHistory()
{
    const int limit = AppSettings::instance().undoHistoryLimit();
    if (m_undoStack.size() > limit)
        m_undoStack.remove(0, m_undoStack.size() - limit);
    if (m_redoStack.size() > limit)
        m_redoStack.remove(0, m_redoStack.size() - limit);
}

// どのスナップショットからも参照されなくなった「作り直し用の中身」を捨てる。
void CanvasWidget::pruneUndoBlobs()
{
    if (m_undoItemBlobs.isEmpty())
        return;
    QSet<quint64> referenced;
    auto collect = [&referenced](const UndoSnapshot &snap) {
        for (const UndoItemState &st : snap.items)
            referenced.insert(st.id);
    };
    for (const UndoSnapshot &snap : m_undoStack) collect(snap);
    for (const UndoSnapshot &snap : m_redoStack) collect(snap);
    if (m_undoStepOpen) collect(m_undoPendingBefore);

    for (auto it = m_undoItemBlobs.begin(); it != m_undoItemBlobs.end();) {
        if (referenced.contains(it.key()))
            ++it;
        else
            it = m_undoItemBlobs.erase(it);
    }
}

// ------------------------------------------------------------
//  Undo / Redo
// ------------------------------------------------------------
bool CanvasWidget::canApplyUndoNow()
{
    // releaseを取りこぼして開きっぱなしのステップは、ボタンが全部離れて
    // いればここで確定させてしまう。
    if (m_undoStepOpen && QGuiApplication::mouseButtons() == Qt::NoButton
        && m_activeMode.isEmpty())
        commitUndoStep();

    // フォーカス（全画面）中・その遷移中、何かを掴んでいる最中は触らない。
    if (m_focusedItem || m_pendingFocusItem)
        return false;
    if (!m_activeMode.isEmpty() || m_undoStepOpen)
        return false;
    // 2026-09-23: バブル融合の吸い込み中（約0.5秒）は待つ。終わった時点で
    // 1ステップとして積まれるので、その後なら1回で戻せる。
    if (m_fusion.absorbing())
        return false;
    return true;
}

bool CanvasWidget::undo()
{
    if (!canApplyUndoNow() || m_undoStack.isEmpty())
        return false;
    UndoSnapshot target = m_undoStack.takeLast();
    m_redoStack.append(captureUndoSnapshot());
    applyUndoSnapshot(target);
    trimUndoHistory();
    pruneUndoBlobs();
    qDebug() << "[Undo] undo applied. remaining undo=" << m_undoStack.size()
             << "redo=" << m_redoStack.size();
    emit sessionContentChanged();
    return true;
}

bool CanvasWidget::redo()
{
    if (!canApplyUndoNow() || m_redoStack.isEmpty())
        return false;
    UndoSnapshot target = m_redoStack.takeLast();
    m_undoStack.append(captureUndoSnapshot());
    applyUndoSnapshot(target);
    trimUndoHistory();
    pruneUndoBlobs();
    qDebug() << "[Undo] redo applied. undo=" << m_undoStack.size()
             << "remaining redo=" << m_redoStack.size();
    emit sessionContentChanged();
    return true;
}

void CanvasWidget::applyUndoSnapshot(const UndoSnapshot &snap)
{
    UndoSuppressScope suppress(this);

    // 進行中の演出・一時状態を止める（ポインタを握っているものがあるため）。
    cancelAllOverlapAnims();
    clearSelection();
    m_multiDragStartPositions.clear();

    QSet<quint64> wantedItems;
    for (const UndoItemState &st : snap.items)
        wantedItems.insert(st.id);

    // ---- 1) 目標に無いバブルを消す ----
    QList<CanvasItem*> toDestroy;
    for (CanvasItem *item : m_items) {
        const quint64 id = m_undoIdByItem.value(item, 0);
        if (id == 0 || !wantedItems.contains(id))
            toDestroy.append(item);
    }
    if (!toDestroy.isEmpty()) {
        // destroyItem() は GL コンテキストが current である前提
        // （deleteSelectedItems() と同じ扱い）。
        const bool glReady = (context() != nullptr);
        if (glReady) makeCurrent();
        for (CanvasItem *item : toDestroy)
            destroyItem(item);
        if (glReady) doneCurrent();
#ifdef Q_OS_WIN
        {
            const HRESULT oleHr = OleInitialize(nullptr);
            qDebug() << "[Undo][CanvasWidget] items destroyed. OleInitialize hr=0x"
                     << Qt::hex << static_cast<unsigned long>(oleHr) << Qt::dec;
            if (auto *mw = qobject_cast<MainWindow*>(window()))
                mw->refreshDropTargets();
        }
#endif
    }

    // ---- 2) 目標にあって今無いバブルを作り直す ----
    for (const UndoItemState &st : snap.items) {
        if (m_undoItemById.contains(st.id))
            continue;
        const QJsonObject entry = m_undoItemBlobs.value(st.id);
        if (entry.isEmpty()) {
            qWarning() << "[Undo] no restore data for item id" << st.id << "- skipped";
            continue;
        }
        const quint64 id = st.id;
        const QPointF pos = st.pos;
        const double scale = st.scale;
        CanvasItem *created = createItemFromSessionEntry(
            entry,
            [this, id, pos, scale](CanvasItem *asyncItem) {
                // キャッシュに無いWeb画像は取得後にここへ来る。
                if (!asyncItem || m_undoItemById.contains(id) || !m_items.contains(asyncItem))
                    return;
                bindUndoItemId(asyncItem, id);
                asyncItem->setScale(scale);
                asyncItem->setPos(pos);
                update();
            });
        if (created)
            bindUndoItemId(created, id);
        else
            qWarning() << "[Undo] item could not be recreated synchronously, id" << st.id;
    }
    // 作り直しの経路（addLauncher等）が押し出しアニメを始めるので止める。
    cancelAllOverlapAnims();

    // ---- 3) 並び（重なり順）と位置・スケール ----
    {
        QList<CanvasItem*> ordered;
        ordered.reserve(m_items.size());
        QSet<CanvasItem*> placed;
        for (const UndoItemState &st : snap.items) {
            CanvasItem *item = m_undoItemById.value(st.id, nullptr);
            if (!item || !m_items.contains(item) || placed.contains(item))
                continue;
            item->setScale(st.scale);
            item->setPos(st.pos);
            ordered.append(item);
            placed.insert(item);
        }
        for (CanvasItem *item : m_items) {
            if (!placed.contains(item))
                ordered.append(item);
        }
        m_items = ordered;
    }

    // ---- 3.5) 本体のリスト（2026-09-23、バブル融合・並べ替え） ----
    // 作り直したバブルも含め、並びが目標と違えば書き戻す。今どこを再生して
    // いるかは記録していないので、今の動画のまま（restoreCoreListFromJson）。
    for (const UndoItemState &st : snap.items) {
        auto *video = canvasItemAsVideo(m_undoItemById.value(st.id, nullptr));
        if (!video || !m_items.contains(video))
            continue;
        if (video->coreListIdentityKey() != st.coreListKey)
            video->restoreCoreListFromJson(st.coreList);
        // 2026-09-23: ラベルの長押しで切り替えたURLも、その回数が違えば戻す
        // （連続再生で進んだだけのURLの違いは回数が同じなので触らない）。
        if (video->labelHoldEpoch() != st.holdEpoch)
            video->restoreLabelHoldState(st.holdUrl, st.holdEpoch, st.holdLabel);
    }

    update();
}

// ------------------------------------------------------------
//  ショートカット（2026-09-17）
// ------------------------------------------------------------
namespace {

constexpr uint kVkShift   = 0x10;
constexpr uint kVkControl = 0x11;
constexpr uint kVkMenu    = 0x12;   // Alt
constexpr uint kVkLWin    = 0x5B;

enum ShortcutModifier : uint {
    ModCtrl  = 0x1,
    ModShift = 0x2,
    ModAlt   = 0x4,
    ModWin   = 0x8,
};

uint modifierBitForVirtualKey(uint vk)
{
    switch (vk) {
    case kVkControl: return ModCtrl;
    case kVkShift:   return ModShift;
    case kVkMenu:    return ModAlt;
    case kVkLWin:    return ModWin;
    default:         return 0;
    }
}

// KeyInput と同じ規則で、左右のある修飾キーを片方に寄せる。
uint normalizedVirtualKey(uint vk)
{
    switch (vk) {
    case 0xA0: case 0xA1: return kVkShift;
    case 0xA2: case 0xA3: return kVkControl;
    case 0xA4: case 0xA5: return kVkMenu;
    case 0x5C:            return kVkLWin;
    default:              return vk;
    }
}

// Windows 以外（仮想キーコードが取れない環境）向けの最低限の読み替え。
uint virtualKeyFromQtKey(int key)
{
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
        return static_cast<uint>(key);   // 'A'〜'Z' / '0'〜'9' は VK と同じ値
    switch (key) {
    case Qt::Key_Delete:    return 0x2E;
    case Qt::Key_Backspace: return 0x08;
    case Qt::Key_Space:     return 0x20;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return 0x0D;
    case Qt::Key_Escape:    return 0x1B;
    case Qt::Key_Tab:       return 0x09;
    case Qt::Key_Shift:     return kVkShift;
    case Qt::Key_Control:   return kVkControl;
    case Qt::Key_Alt:       return kVkMenu;
    case Qt::Key_Meta:      return kVkLWin;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
            return 0x70 + static_cast<uint>(key - Qt::Key_F1);
        return 0;
    }
}

} // namespace

bool CanvasWidget::matchesCanvasShortcut(const QKeyEvent *event, const QString &id)
{
    if (!event)
        return false;

    // 割り当て側: 修飾キー0〜2個＋普通のキーちょうど1個、重複なし、でなければ無効。
    uint bindMods = 0;
    uint bindKey = 0;
    int plainKeys = 0;
    QList<uint> seen;
    for (uint raw : AppSettings::instance().canvasShortcut(id)) {
        const uint vk = normalizedVirtualKey(raw);
        if (vk == 0)
            continue;
        if (seen.contains(vk))
            return false;
        seen << vk;
        if (const uint bit = modifierBitForVirtualKey(vk)) {
            bindMods |= bit;
        } else {
            bindKey = vk;
            ++plainKeys;
        }
    }
    if (plainKeys != 1)
        return false;

    // 押された側
    uint pressedKey = normalizedVirtualKey(event->nativeVirtualKey());
    if (pressedKey == 0)
        pressedKey = virtualKeyFromQtKey(event->key());
    if (pressedKey == 0 || modifierBitForVirtualKey(pressedKey) != 0)
        return false;
    uint pressedMods = 0;
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (mods & Qt::ControlModifier) pressedMods |= ModCtrl;
    if (mods & Qt::ShiftModifier)   pressedMods |= ModShift;
    if (mods & Qt::AltModifier)     pressedMods |= ModAlt;
    if (mods & Qt::MetaModifier)    pressedMods |= ModWin;

    return pressedKey == bindKey && pressedMods == bindMods;
}

bool CanvasWidget::deleteSelectedBubbles()
{
    // 全画面（フォーカス）中・その遷移中、何かを掴んでいる最中は消さない。
    if (m_focusedItem || m_pendingFocusItem || !m_activeMode.isEmpty())
        return false;

    // 複数選択が無ければ、アクティブなバブル（直近クリック/選択した単体）を対象にする。
    if (m_selectedItems.isEmpty() && m_activeItem && m_items.contains(m_activeItem))
        m_selectedItems.insert(m_activeItem);
    if (m_selectedItems.isEmpty())
        return false;

    const int count = m_selectedItems.size();
    UndoAutoStep undoStep(this);
    deleteSelectedItems();
    qDebug() << "[Shortcut] deleted" << count << "bubble(s)";
    return true;
}
