#pragma once

#include <QObject>

class QWidget;
class QFrame;
class QPushButton;
class AnimatedButton;

// ============================================================
//  UI (新デザイン検討用の土台実装)
//
//  2026-08-26: 島(DynamicIsland)を全面刷新するにあたり、既存の
//  DynamicIsland側のコードには一切手を入れず、新デザインは本クラスへ
//  ゼロから作っていく方針にした。MainWindowが新旧どちらも生成し、
//  Ctrl+←/→ で表示を切り替えて見比べられるようにする
//  （MainWindow::keyPressEvent参照。プレーンな←/→は動画シークで
//  既に使われているため、衝突を避けてCtrl併用にしている）。
//
//  現時点で決まっている方針（詳細は要相談のまま据え置き）:
//    - 中央配置＋カーソルトリガー表示はやめ、常時表示の固定配置にする
//    - 左上: 丸い+ボタン単体（設定への入口。将来的にはクリックで
//      丸自体が広がって別画面に遷移するアニメーションにする予定だが、
//      本段階ではシグナルを発火するだけの土台）
//    - 右上: -□×をまとめた横長ピル1個（常時表示・固定サイズの暫定実装。
//      見た目の詳細は保留中）
//    - ウィンドウのドラッグ移動（島の内外問わず掴める化）は保留
//
//  ボタンの見た目・出現アニメーションは既存のAnimatedButton
//  （DynamicIsland.h宣言。DynamicIsland本体のロジックとは無関係の
//  汎用パーツ）をそのまま再利用している（二重実装を避けるため）。
// ============================================================
class UI : public QObject
{
    Q_OBJECT

public:
    // host: ボタン類を子ウィジェットとして生成する先（MainWindow想定）。
    explicit UI(QWidget *host);

    // hostのリサイズに合わせて左上/右上へ再配置する。
    // MainWindow::resizeEvent()から呼ぶこと（layoutIsland()と対になる関数）。
    void reposition();

    // 新旧切り替え用。falseの間はボタン類を隠す。
    void setUiVisible(bool visible);

    AnimatedButton *btnSettings = nullptr; // 左上の丸（+）。当面AnimatedButtonのまま。
    // 右上ピルの3つ。2026-08-26: 「無彩色・境目に区切り線」の
    // セグメント調ボタン(MonoIconButton、UI.cpp内)に差し替えたため、
    // 丸背景を持つAnimatedButtonではなくQPushButtonとして公開する。
    QPushButton    *btnClose    = nullptr;
    QPushButton    *btnMax      = nullptr;
    QPushButton    *btnMin      = nullptr;

signals:
    // +ボタン押下。「丸が広がって設定画面へ遷移する」アニメーションは
    // 未実装のため、現段階ではMainWindow側で任意の処理に繋いでよい
    // （暫定）。
    void settingsRequested();

private:
    QWidget *m_host = nullptr;
    // -□×の背景ピル（実体はUI.cpp内のprivateクラスRightPill）。
    QFrame  *m_rightPill = nullptr;

    static constexpr int kMargin = 15; // 島の従来マージン(15px)に合わせる
};
