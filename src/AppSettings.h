#pragma once
#include <QObject>
#include <QSettings>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

#include "AppLanguage.h"

// ---- AppSettings -----------------------------------------------
// アプリ全体の設定をQSettingsで永続化するシングルトン。
// SourceBubble / CanvasVideo から参照する。
// ----------------------------------------------------------------
class AppSettings : public QObject
{
    Q_OBJECT
public:
    static AppSettings &instance() {
        static AppSettings s;
        return s;
    }

    // ---- 表示言語（2026-09-18）----
    // Windows の表示言語による自動切り替えは廃止し、ここに保存した値だけで決める。
    // 既定は日本語。値を変えると AppLanguage へ即反映し、以後に作られる文言が
    // 新しい言語になる（すでに作られている一部の文言は再起動後に入れ替わる）。
    AppLanguage::Language language() const { return m_language; }
    void setLanguage(AppLanguage::Language v) {
        if (m_language == v) return;
        m_language = v;
        AppLanguage::setLanguage(v);
        save(); emit changed();
    }

    // 2026-07-12: 中ボタンドラッグパンの慣性（離した後も少し動き続けて
    // 減速して止まる挙動）は好みが大きく分かれるため、オン/オフを設定で
    // 選べるようにする。フォーカス中パン（focus_image_pan）・通常キャンバス
    // パン（camera_pan）を別々に切れるよう4値にしてある
    // （CameraPanInertia自体は両者の区別を知らない汎用クラスなので、この
    // 判定はCanvasWidget側で行う）。
    enum class CameraPanInertiaMode {
        Off,        // 常に無効
        CanvasOnly, // 通常キャンバスのパンのみ有効
        FocusOnly,  // フォーカス中パンのみ有効
        Always,     // 常に有効
    };

    // 2026-07-19: バブルの疑似ガラスハイライト（GlassHighlightEffect）の
    // 品質設定。島の設定パネルから3択で選ぶ。
    //   Performance: ガラス描画なし（一番素の状態）
    //   Medium:      現在のライティング（法線ベース照明＋色収差）※既定
    //   Quality:     Medium＋フレームバッファキャプチャによる疑似屈折
    //                （屈折は未実装。実装されるまでMediumと同じ動作にする）
    enum class GlassEffectQuality {
        Performance,
        Medium,
        Quality,
    };

    // 2026-09-16: ガラスの見た目のスタイル（品質が「中間」以上のときだけ効く）。
    //   Standard: これまでの見た目
    //   Carved:   削り出し（外周と内側に面取りの明暗を足す）
    //   Gummy:    2026-10-11 グミ（乳白の厚いゲル。Claude outputs/gummy-glass-lab.html で調整）
    enum class GlassStyle {
        Standard,
        Carved,
        Gummy,
    };

    // 2026-10-11: バブル全画面（フォーカス）時にウィンドウをどう扱うか。
    // 旧「ウィンドウサイズを固定」(bool)を3択にしたもの。
    //   Off:         何も固定しない（全画面中はウィンドウが動画の対比に変わり、解除後もそのまま）
    //   CanvasFixed: 解除後、ウィンドウサイズを全画面前に戻す（旧ONと同じ。位置は戻さない）
    //   WindowFixed: 全画面中もウィンドウを一切変えない（動画は余白（黒）つきで収める）
    enum class FocusWindowMode {
        Off,
        CanvasFixed,
        WindowFixed,
    };

    // 2026-07-17: マウスホイール1コリあたりのズーム変化量（感度、%）。
    // CanvasWidget::handleWheel()がここから倍率ステップを引いて
    // factor = 1+step/100（拡大）/ 1/(1+step/100)（縮小）を作る。
    // 当初は「標準/高感度」の2択にしていたが、段階的すぎて細かい好みに
    // 合わせられないため、スライダーで連続的に調整できるようにした
    // （島の設定パネル、registerIslandSettingsControls()参照）。
    static constexpr int kZoomWheelSensitivityMin     = 5;   // 1コリ5%（最も鈍感）
    static constexpr int kZoomWheelSensitivityMax     = 40;  // 1コリ40%（最も敏感）
    static constexpr int kZoomWheelSensitivityDefault = 10;  // 旧「標準」相当

    // 2026-09-17: 元に戻す（Ctrl+Z）/やり直し（Ctrl+Y）で遡れる最大回数。
    // 島の設定パネル「操作」から変更する（CanvasWidget の Undo 履歴が参照）。
    static constexpr int kUndoHistoryLimitMin     = 1;
    static constexpr int kUndoHistoryLimitMax     = 64;
    static constexpr int kUndoHistoryLimitDefault = 32;

    // 動画解像度 (144/360/480/720/1080)。
    // 2026-07-03: 以前はバブル表示時(bubbleQuality)とフォーカス(全画面)時
    // (focusQuality)を別々に持っていたが、設定項目が2つあることで島の
    // 設定パネルが複雑になるだけで実用上のメリットが薄いとの判断で1本化した。
    // バブルであろうと全画面であろうと常にこの値を使う。
    int  quality()  const { return m_quality; }
    bool showGrid() const { return m_showGrid; }
    int gridSize() const { return m_gridSize; }
    FocusWindowMode focusWindowMode() const { return m_focusWindowMode; }
    // 2026-10-11: ONの間、キャンバス固定でバブル全画面を解除したとき、
    // ウィンドウの「サイズ」に加えて「位置」も全画面前に戻す。
    bool rememberWindowPosition() const { return m_rememberWindowPosition; }
    // 2026-09-17: 右ボタンでドラッグしてウィンドウを移動（島以外でも掴める）。
    // 動かさずに離した右クリックは従来どおりアクティブ化。
    bool rightDragWindowMoveEnabled() const { return m_rightDragWindowMoveEnabled; }
    // 2026-09-17: ON＝右下のリサイズハンドルを消し、どの辺・角からでもリサイズ。
    // OFF（既定）＝従来どおり右下のハンドルからのみ。
    bool edgeResizeEnabled() const { return m_edgeResizeEnabled; }
    bool streamOffscreenResourceSavingEnabled() const { return m_streamOffscreenResourceSavingEnabled; }
    uint hotkeyModifiers() const { return m_hotkeyModifiers; }
    uint hotkeyVirtualKey() const { return m_hotkeyVirtualKey; }
    // 2026-09-16: 島の設定パネルの「呼出しショートカットキー」は3つの入力欄に
    // 1キーずつ入れる。どの欄に何を入れたか（空欄=0）をそのまま覚えておき、
    // 開き直しても同じ並びで出す。中身（修飾キー＋キー）は hotkeyModifiers /
    // hotkeyVirtualKey が正で、こちらは表示の並びだけ。
    QList<uint> hotkeySlots() const { return m_hotkeySlots; }
    void setHotkeySlots(const QList<uint> &keySlots) {
        if(m_hotkeySlots==keySlots)return;
        m_hotkeySlots=keySlots; save(); emit changed();
    }

    // 2026-09-16: 島の色。既定は Windows のアクセントカラー。オフにすると
    // customAccentColor（"#RRGGBB"）を使う。
    bool useWindowsAccentColor() const { return m_useWindowsAccentColor; }
    void setUseWindowsAccentColor(bool v) {
        if(m_useWindowsAccentColor==v)return;
        m_useWindowsAccentColor=v; save(); emit changed();
    }
    QString customAccentColor() const { return m_customAccentColor; }
    void setCustomAccentColor(const QString &v) {
        if(m_customAccentColor==v)return;
        m_customAccentColor=v; save(); emit changed();
    }
    CameraPanInertiaMode cameraPanInertiaMode() const { return m_cameraPanInertiaMode; }
    // 呼び出し側（CanvasWidget）が「今の状況で慣性を有効にすべきか」を
    // enumの分岐を書かずに済むようにした便利ヘルパー。
    bool cameraPanInertiaEnabledForFocus() const {
        return m_cameraPanInertiaMode == CameraPanInertiaMode::Always
            || m_cameraPanInertiaMode == CameraPanInertiaMode::FocusOnly;
    }
    bool cameraPanInertiaEnabledForCanvas() const {
        return m_cameraPanInertiaMode == CameraPanInertiaMode::Always
            || m_cameraPanInertiaMode == CameraPanInertiaMode::CanvasOnly;
    }

    static QString glassStyleToString(GlassStyle v) {
        switch (v) {
        case GlassStyle::Carved: return QStringLiteral("carved");
        case GlassStyle::Gummy:  return QStringLiteral("gummy");
        case GlassStyle::Standard: break;
        }
        return QStringLiteral("standard");
    }
    static GlassStyle glassStyleFromString(const QString &s) {
        if (s == QStringLiteral("carved")) return GlassStyle::Carved;
        if (s == QStringLiteral("gummy"))  return GlassStyle::Gummy;
        // 2026-10-13: 試した「グミ（新）」は廃止（保存値はグミとして読む）
        if (s == QStringLiteral("gummyNew")) return GlassStyle::Gummy;
        // 2026-10-11: 比較用だった「グミ（改）」はグミへ一本化した（保存値はグミとして読む）
        if (s == QStringLiteral("gummyLite")) return GlassStyle::Gummy;
        return GlassStyle::Standard;
    }

    // 2026-10-11: グミが実際に描かれているか（スタイルはガラス表現が「中間」以上でだけ効く）。
    // 影・枠などシェーダー以外のグミ用の値を切り替えるときに使う。
    bool gummyGlassActive() const {
        return m_glassStyle == GlassStyle::Gummy
            && m_glassEffectQuality != GlassEffectQuality::Performance;
    }
    // グミのときの非アクティブ枠（drawRect(board, …)）の塗り・線のアルファ。
    // 標準は 塗り46 / 線130。ラボで決めた値。
    static constexpr int kGummyChromeFillAlpha   = 0;
    static constexpr int kGummyChromeStrokeAlpha = 118;

    GlassStyle glassStyle() const { return m_glassStyle; }
    void setGlassStyle(GlassStyle v) {
        if (m_glassStyle == v) return;
        m_glassStyle = v; save(); emit changed();
    }
    GlassEffectQuality glassEffectQuality() const { return m_glassEffectQuality; }
    void setGlassEffectQuality(GlassEffectQuality v) {
        if(m_glassEffectQuality==v)return;
        m_glassEffectQuality=v; save(); emit changed();
    }

    // 2026-09-17: キャンバス操作のショートカット（島の設定パネル「操作」で変更）。
    // 1つの割り当て＝KeyInput 3欄ぶんの Windows 仮想キーコード（空欄=0）。
    // 修飾キー（Ctrl/Shift/Alt/Win）0〜2個＋普通のキーちょうど1個で有効。
    // 1つの操作に「メイン」と「サブ」の2つを割り当てられる（例: 削除= Delete / Ctrl+X）。
    static const QStringList &canvasShortcutIds() {
        static const QStringList ids = {
            QStringLiteral("undo"),   QStringLiteral("undoAlt"),
            QStringLiteral("redo"),   QStringLiteral("redoAlt"),
            QStringLiteral("delete"), QStringLiteral("deleteAlt"),
        };
        return ids;
    }
    static QList<uint> defaultCanvasShortcut(const QString &id) {
        constexpr uint kCtrl = 0x11, kShift = 0x10, kDelete = 0x2E;
        if (id == QLatin1String("undo"))      return { kCtrl, 'Z', 0 };
        if (id == QLatin1String("redo"))      return { kCtrl, 'Y', 0 };
        if (id == QLatin1String("redoAlt"))   return { kCtrl, kShift, 'Z' };
        if (id == QLatin1String("delete"))    return { kDelete, 0, 0 };
        if (id == QLatin1String("deleteAlt")) return { kCtrl, 'X', 0 };
        return { 0, 0, 0 };                   // undoAlt など: 既定なし
    }
    QList<uint> canvasShortcut(const QString &id) const {
        return m_canvasShortcuts.value(id, defaultCanvasShortcut(id));
    }
    void setCanvasShortcut(const QString &id, QList<uint> keySlots) {
        while (keySlots.size() < 3) keySlots << 0;
        while (keySlots.size() > 3) keySlots.removeLast();
        if (canvasShortcut(id) == keySlots) return;
        m_canvasShortcuts.insert(id, keySlots); save(); emit changed();
    }

    int undoHistoryLimit() const { return m_undoHistoryLimit; }
    void setUndoHistoryLimit(int v) {
        v = qBound(kUndoHistoryLimitMin, v, kUndoHistoryLimitMax);
        if(m_undoHistoryLimit==v)return;
        m_undoHistoryLimit=v; save(); emit changed();
    }

    // スライダーの生値（%、5〜40）。
    int zoomWheelSensitivityPercent() const { return m_zoomWheelSensitivityPercent; }
    // 呼び出し側（CanvasWidget::handleWheel）が直接使える倍率ステップ。
    // 0.10 → 1コリでfactor=1.10（拡大）/ 1/1.10（縮小）。
    double zoomWheelFactorStep() const {
        return m_zoomWheelSensitivityPercent / 100.0;
    }

    void setQuality(int q)        { if(m_quality==q)return; m_quality=q; save(); emit changed(); }
    void setShowGrid(bool v) { if(m_showGrid==v)return; m_showGrid=v; save(); emit changed(); }
    void setGridSize(int v) {
        v = qBound(10, v, 400);
        if(m_gridSize==v)return;
        m_gridSize=v; save(); emit changed();
    }
    void setRightDragWindowMoveEnabled(bool v) {
        if(m_rightDragWindowMoveEnabled==v)return;
        m_rightDragWindowMoveEnabled=v; save(); emit changed();
    }
    void setEdgeResizeEnabled(bool v) {
        if(m_edgeResizeEnabled==v)return;
        m_edgeResizeEnabled=v; save(); emit changed();
    }
    void setRememberWindowPosition(bool v) {
        if(m_rememberWindowPosition==v)return;
        m_rememberWindowPosition=v; save(); emit changed();
    }
    void setFocusWindowMode(FocusWindowMode v) {
        if(m_focusWindowMode==v)return;
        m_focusWindowMode=v; save(); emit changed();
    }
    void setStreamOffscreenResourceSavingEnabled(bool v) {
        if(m_streamOffscreenResourceSavingEnabled==v)return;
        m_streamOffscreenResourceSavingEnabled=v; save(); emit changed();
    }
    void setHotkey(uint modifiers, uint virtualKey) {
        if(m_hotkeyModifiers==modifiers && m_hotkeyVirtualKey==virtualKey)return;
        m_hotkeyModifiers=modifiers; m_hotkeyVirtualKey=virtualKey; save(); emit changed();
    }
    void setCameraPanInertiaMode(CameraPanInertiaMode v) {
        if(m_cameraPanInertiaMode==v)return;
        m_cameraPanInertiaMode=v; save(); emit changed();
    }
    void setZoomWheelSensitivityPercent(int v) {
        v = qBound(kZoomWheelSensitivityMin, v, kZoomWheelSensitivityMax);
        if(m_zoomWheelSensitivityPercent==v)return;
        m_zoomWheelSensitivityPercent=v; save(); emit changed();
    }

    static const QList<int> &qualityLevels() {
        static QList<int> lv = {144, 360, 480, 720, 1080};
        return lv;
    }

    // 現時点でのアプリ全体の画質上限。
    // 2026-07-11: DASH側の不具合（MpvPlayer::open()のaudio-file渡し方を
    // EDL(new_stream)方式に変更）を検証するため、いったん1080pに引き上げて
    // 実機テストする（v4移行、下記コンストラクタ参照）。島のQuality選択UI
    // 自体は既に削除済み（v3参照）でquality()を直接変更する手段が無いため、
    // この値と後述のv4移行だけが実際に効く経路になる。
    // （AppSettings::qualityLevels() は変更せず1080を含めたままにしてある）。
    static constexpr int qualityCeiling() { return 1080; }

signals:
    void changed();

private:
    // ini上は将来的な手動編集・可読性のため文字列で保存する
    // （数値だとenumの並び順を変えたくなった時に値がずれる事故もある）。
    static QString cameraPanInertiaModeToString(CameraPanInertiaMode m) {
        switch (m) {
        case CameraPanInertiaMode::Off:        return QStringLiteral("off");
        case CameraPanInertiaMode::CanvasOnly: return QStringLiteral("canvasOnly");
        case CameraPanInertiaMode::FocusOnly:  return QStringLiteral("focusOnly");
        case CameraPanInertiaMode::Always:     return QStringLiteral("always");
        }
        return QStringLiteral("always");
    }
    static CameraPanInertiaMode cameraPanInertiaModeFromString(const QString &s) {
        if (s == QStringLiteral("off"))        return CameraPanInertiaMode::Off;
        if (s == QStringLiteral("canvasOnly")) return CameraPanInertiaMode::CanvasOnly;
        if (s == QStringLiteral("focusOnly"))  return CameraPanInertiaMode::FocusOnly;
        return CameraPanInertiaMode::Always;
    }
    static QString glassEffectQualityToString(GlassEffectQuality q) {
        switch (q) {
        case GlassEffectQuality::Performance: return QStringLiteral("performance");
        case GlassEffectQuality::Medium:      return QStringLiteral("medium");
        case GlassEffectQuality::Quality:     return QStringLiteral("quality");
        }
        return QStringLiteral("medium");
    }
    static GlassEffectQuality glassEffectQualityFromString(const QString &s) {
        if (s == QStringLiteral("performance")) return GlassEffectQuality::Performance;
        if (s == QStringLiteral("quality"))     return GlassEffectQuality::Quality;
        return GlassEffectQuality::Medium;
    }

    AppSettings() {
        QSettings s;
        // schemaVersionによる段階移行。値を書き換えるたびに古い環境の
        // s.value()デフォルト引数が効かなくなる問題を避けるため、
        // バージョンごとに一度だけ強制移行する（>=現バージョンの環境では
        // 以後一切触らない。ユーザーが手動で選び直した値を勝手に
        // 上書きしないため）。
        //   v1: Bubble/Focus Qualityの既定値を480/720固定 → 480/360固定へ変更
        //   v2 (2026-07-03): Bubble Quality/Focus Qualityの2項目を
        //       quality() 1本に統合（島の設定パネル簡略化のため）。
        //       既存環境では「実際に見ていた時（フォーカス時）」の画質を
        //       優先して引き継ぐ。
        //   v3 (2026-07-03): DASHのシーク詰まり調査の結論として、当面は
        //       画質切り替え動作そのものをやめて720p固定運用とする方針に
        //       なった（島UIのQuality選択欄は削除。DynamicIsland.cpp参照）。
        //       既存環境の保存値に関わらず720pへ強制移行する。
        //       quality()/setQuality()/qualityLevels()/qualityCeiling()の
        //       仕組み自体は、将来1080p等を再度試したくなった時のために
        //       あえて残してある（qualityCeiling()を上げてUIを復活させれば
        //       すぐ試せる）。
        //   v4 (2026-07-11): MpvPlayer::open()のDASH音声結合をEDL(new_stream)
        //       方式へ変更した効果を実機検証するため、いったん1080pへ
        //       強制移行する。既存環境の保存値（720固定）に関わらず上書きする。
        //       検証の結果不安定なままなら720へ戻す想定（その場合はv5で
        //       再度720へ強制移行すること）。
        const int schemaVersion = s.value("video/qualitySchemaVersion", 0).toInt();
        if (schemaVersion < 1) {
            s.setValue("video/bubbleQuality", 360);
            s.setValue("video/focusQuality", 480);
        }
        if (schemaVersion < 2) {
            const int migratedQuality = s.value("video/focusQuality", 480).toInt();
            s.setValue("video/quality", migratedQuality);
            s.remove("video/bubbleQuality");
            s.remove("video/focusQuality");
        }
        if (schemaVersion < 2)
            s.setValue("video/qualitySchemaVersion", 2);
        if (schemaVersion < 3)
            s.setValue("video/quality", 720);
        if (schemaVersion < 3)
            s.setValue("video/qualitySchemaVersion", 3);
        if (schemaVersion < 4)
            s.setValue("video/quality", 1080);
        if (schemaVersion < 4)
            s.setValue("video/qualitySchemaVersion", 4);
        m_quality       = s.value("video/quality", 1080).toInt();
        m_showGrid      = s.value("canvas/showGrid", false).toBool();
        m_gridSize      = qBound(10, s.value("canvas/gridSize", 50).toInt(), 400);
        m_rememberWindowPosition = s.value("settings/rememberWindowPosition", false).toBool();
        {
            // 旧設定(bool: settings/windowSizeLockEnabled)からの移行:
            // 新キーが無ければ、旧ONはキャンバス固定、旧OFFはオフとして引き継ぐ。
            const QString mode = s.value("settings/focusWindowMode").toString();
            if (mode == QLatin1String("canvas"))
                m_focusWindowMode = FocusWindowMode::CanvasFixed;
            else if (mode == QLatin1String("window"))
                m_focusWindowMode = FocusWindowMode::WindowFixed;
            else if (mode == QLatin1String("off"))
                m_focusWindowMode = FocusWindowMode::Off;
            else
                m_focusWindowMode = s.value("settings/windowSizeLockEnabled", false).toBool()
                    ? FocusWindowMode::CanvasFixed : FocusWindowMode::Off;
        }
        m_rightDragWindowMoveEnabled = s.value("settings/rightDragWindowMoveEnabled", true).toBool();
        m_edgeResizeEnabled = s.value("settings/edgeResizeEnabled", false).toBool();
        m_streamOffscreenResourceSavingEnabled =
            s.value("stream/offscreenResourceSavingEnabled", false).toBool();
        m_hotkeyModifiers = s.value("hotkey/modifiers", 0x0002 | 0x0004).toUInt();
        m_hotkeyVirtualKey = s.value("hotkey/virtualKey", 0x20).toUInt();
        m_hotkeySlots.clear();
        for (const QVariant &v : s.value("hotkey/keySlots").toList())
            m_hotkeySlots.append(v.toUInt());
        m_useWindowsAccentColor = s.value("appearance/useWindowsAccentColor", true).toBool();
        m_customAccentColor = s.value("appearance/customAccentColor", QStringLiteral("#4A90E2")).toString();
        // 2026-07-12: 既定は"always"にしてある。フォーカス中パンの慣性は
        // 既にこの設定無しで常時有効な状態でリリース済みだったため、
        // 新規に設定を追加したことで既存ユーザーの体感が変わらないよう
        // （黙ってオフになったりしないよう）現状維持をデフォルトにした。
        m_cameraPanInertiaMode = cameraPanInertiaModeFromString(
            s.value("camera/panInertiaMode", "always").toString());
        // 2026-07-17: 当初「標準/高感度」の文字列2値だったのを、スライダーの
        // 連続値（%）へ作り直した。開発中に一度だけ触った環境向けの簡易移行
        // として、旧キーが残っていれば変換して読み替える（"high"→20、それ以外
        // →10）。旧キー自体は削除しない（万一の切り戻し調査用に残す）。
        if (s.contains("camera/zoomWheelSensitivityPercent")) {
            m_zoomWheelSensitivityPercent = qBound(
                kZoomWheelSensitivityMin,
                s.value("camera/zoomWheelSensitivityPercent", kZoomWheelSensitivityDefault).toInt(),
                kZoomWheelSensitivityMax);
        } else if (s.contains("camera/zoomWheelSensitivity")) {
            m_zoomWheelSensitivityPercent =
                s.value("camera/zoomWheelSensitivity").toString() == QStringLiteral("high") ? 20 : 10;
        } else {
            m_zoomWheelSensitivityPercent = kZoomWheelSensitivityDefault;
        }
        for (const QString &id : canvasShortcutIds()) {
            const QString key = QStringLiteral("shortcuts/") + id;
            if (!s.contains(key))
                continue;   // 未設定なら既定（defaultCanvasShortcut）
            QList<uint> keySlots;
            for (const QVariant &v : s.value(key).toList())
                keySlots.append(v.toUInt());
            while (keySlots.size() < 3) keySlots << 0;
            while (keySlots.size() > 3) keySlots.removeLast();
            m_canvasShortcuts.insert(id, keySlots);
        }
        m_undoHistoryLimit = qBound(
            kUndoHistoryLimitMin,
            s.value("edit/undoHistoryLimit", kUndoHistoryLimitDefault).toInt(),
            kUndoHistoryLimitMax);
        // 既定はMedium（この設定が無かった頃の見た目＝ライティングあり）。
        m_glassEffectQuality = glassEffectQualityFromString(
            s.value("glass/effectQuality", "medium").toString());
        m_glassStyle = glassStyleFromString(s.value("glass/style", "standard").toString());
        m_language = s.value("app/language", "ja").toString() == QStringLiteral("en")
            ? AppLanguage::Language::English : AppLanguage::Language::Japanese;
        AppLanguage::setLanguage(m_language);
    }
    void save() {
        QSettings s;
        s.setValue("video/quality", m_quality);
        s.setValue("canvas/showGrid", m_showGrid);
        s.setValue("canvas/gridSize", m_gridSize);
        s.setValue("settings/rememberWindowPosition", m_rememberWindowPosition);
        s.setValue("settings/focusWindowMode",
                   m_focusWindowMode == FocusWindowMode::CanvasFixed ? "canvas"
                   : m_focusWindowMode == FocusWindowMode::WindowFixed ? "window" : "off");
        s.setValue("settings/rightDragWindowMoveEnabled", m_rightDragWindowMoveEnabled);
        s.setValue("settings/edgeResizeEnabled", m_edgeResizeEnabled);
        s.setValue("stream/offscreenResourceSavingEnabled", m_streamOffscreenResourceSavingEnabled);
        s.setValue("hotkey/modifiers", m_hotkeyModifiers);
        s.setValue("hotkey/virtualKey", m_hotkeyVirtualKey);
        {
            QVariantList slotList;
            for (uint vk : m_hotkeySlots)
                slotList.append(vk);
            s.setValue("hotkey/keySlots", slotList);
        }
        s.setValue("appearance/useWindowsAccentColor", m_useWindowsAccentColor);
        s.setValue("appearance/customAccentColor", m_customAccentColor);
        s.setValue("camera/panInertiaMode", cameraPanInertiaModeToString(m_cameraPanInertiaMode));
        s.setValue("camera/zoomWheelSensitivityPercent", m_zoomWheelSensitivityPercent);
        s.setValue("edit/undoHistoryLimit", m_undoHistoryLimit);
        for (auto it = m_canvasShortcuts.constBegin(); it != m_canvasShortcuts.constEnd(); ++it) {
            QVariantList slotList;
            for (uint vk : it.value())
                slotList.append(vk);
            s.setValue(QStringLiteral("shortcuts/") + it.key(), slotList);
        }
        s.setValue("glass/effectQuality", glassEffectQualityToString(m_glassEffectQuality));
        s.setValue("glass/style", glassStyleToString(m_glassStyle));
        s.setValue("app/language", m_language == AppLanguage::Language::English
                                       ? QStringLiteral("en") : QStringLiteral("ja"));
    }
    int  m_quality {1080};
    bool m_showGrid {false};
    int  m_gridSize {50};
    FocusWindowMode m_focusWindowMode {FocusWindowMode::Off};
    bool m_rememberWindowPosition {false};
    bool m_rightDragWindowMoveEnabled {true};
    bool m_edgeResizeEnabled {false};
    bool m_streamOffscreenResourceSavingEnabled {false};
    uint m_hotkeyModifiers {0x0002 | 0x0004};
    uint m_hotkeyVirtualKey {0x20};
    QList<uint> m_hotkeySlots;
    bool m_useWindowsAccentColor {true};
    QString m_customAccentColor {QStringLiteral("#4A90E2")};
    CameraPanInertiaMode m_cameraPanInertiaMode {CameraPanInertiaMode::Always};
    int m_zoomWheelSensitivityPercent {kZoomWheelSensitivityDefault};
    int m_undoHistoryLimit {kUndoHistoryLimitDefault};
    QHash<QString, QList<uint>> m_canvasShortcuts; // 変更したものだけ入る（無ければ既定）
    GlassEffectQuality m_glassEffectQuality {GlassEffectQuality::Medium};
    GlassStyle m_glassStyle {GlassStyle::Standard};
    AppLanguage::Language m_language {AppLanguage::Language::Japanese};
};
