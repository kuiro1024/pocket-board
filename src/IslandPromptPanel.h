#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;
class QVBoxLayout;

// ============================================================
//  IslandPromptPanel
//
//  島（DynamicIsland）から生える確認プロンプト。プラグインzipの
//  ドロップ時に出していた QMessageBox::question() を置き換える。
//
//  設計の位置づけは IslandSettingsPanel と同じ「島の子ウィジェット」。
//    - 高さの伸縮アニメーションは DynamicIsland 側（m_heightAnim ＋
//      settingsExpandEasing）が担当し、本クラスは必要高さの計算
//      （preferredPanelHeight）とジオメトリ適用（applyPanelGeometry）
//      だけを行う
//    - 文言は QLabel、YES/NO は自前描画のピル型ボタン。ヒットテストと
//      ホバーはQtに任せる（paintEventへ直接描き込まない理由）
//
//  設定パネル（IslandSettingsPanel）とは排他。どちらか一方だけが
//  show されている状態を DynamicIsland が保証する。
// ============================================================

// プロンプトに表示する内容。DynamicIsland / IslandPromptPanel は
// プラグインの都合を知らずに済むよう、呼び出し側（MainWindow）が
// この構造体へ文字列化して渡す。
struct IslandPromptContent
{
    QString     title;        // 見出し（例: "Install Plugin"）
    QString     subject;      // 主対象。強調表示される（例: "ExamplePlugin.zip"）
    QString     message;      // subjectの下に出る本文（1行想定）
    QStringList details;      // 箇条書き（例: DLL名 + ID）。等幅寄りの小さい文字
    QString     warning;      // 最下部の注意文（琥珀色）
    QString     acceptText {QStringLiteral("YES")};
    QString     rejectText {QStringLiteral("NO")};
    // false の場合 YES ボタンをグレーアウトして押せなくする
    // （互換プラグインが1つも無い zip など、実行しても意味が無いケース）。
    bool        acceptEnabled {true};
};

class IslandPromptPanel : public QFrame
{
    Q_OBJECT

public:
    explicit IslandPromptPanel(int islandWidth, int baseH, QWidget *parent = nullptr);

    // 表示内容を差し替える。ウィジェットの作り直しはせず、文言の
    // 入れ替えとdetails行の増減だけを行う。
    void setContent(const IslandPromptContent &content);

    // 全内容を表示するのに必要なパネル全体の高さ（島上部の丸UI部分
    // baseH を含む）。maxTotalHeight でクランプされる。
    //
    // layoutWidth には「島が最終的に到達する幅」を必ず渡すこと。文言は
    // 折り返すので、幅が変われば行数＝必要高さが変わる。以前は内部の
    // m_islandWidth（＝パネル生成時の島の幅）を見ており、初回だけ
    // 畳まれた島の幅で行数を数えて異常に高いパネルになっていた。
    //
    // 【計算コスト】中身はラベル5枚への heightForWidth()＝テキストレイアウトで、
    // ウィンドウのリサイズアニメ中は resizeEvent が毎フレーム飛んでくる
    // （DynamicIsland::notifyHostWindowResized）。内容と幅が変わらなければ
    // 結果も変わらないので、クランプ前の高さをキャッシュして使い回す。
    int preferredPanelHeight(int layoutWidth, int maxTotalHeight) const;

    // パネル自身と内部ウィジェットのジオメトリを適用する。
    // width には「島が最終的に到達する幅」を渡すこと（アニメーション中の
    // 途中の幅ではない）。文言の折り返し行数＝必要高さが幅で決まるため、
    // 途中の幅で計算すると高さが毎フレーム変わってしまう。
    void applyPanelGeometry(int width, int panelHeight);

    // 島の幅アニメーション中、現在の幅へ追従させる（毎フレーム呼ばれる）。
    // applyPanelGeometry() で決めた最終幅のレイアウトを保ったまま、
    // パネル矩形とボタン行の横幅だけを currentWidth に合わせる。
    //
    // これが無いと、最終幅でレイアウトされたボタン行が島にクリップされ、
    // 右端のボタン（NO）だけが島の右端の動き（OutBackの行き過ぎと戻り）に
    // 合わせて「伸びて縮む」ように見える。左端のYESは早々に全部出てしまう
    // ので、左右で挙動が違って見えるのが目立っていた。
    void setVisibleWidth(int currentWidth);

signals:
    void accepted();
    void rejected();

private:
    // 内容から求めた「島の丸UIより下に必要な高さ」。
    // layoutWidth は折り返し計算に使う最終幅。
    int contentHeight(int layoutWidth) const;

    // YES/NOボタン行の配置。width には島の現在幅を渡す。
    void layoutButtons(int width, int buttonTop);

    QWidget *m_content     = nullptr;  // 文言ブロック（ボタンより上）
    QLabel  *m_titleLabel  = nullptr;
    QLabel  *m_subjectLabel= nullptr;
    QLabel  *m_messageLabel= nullptr;
    QLabel  *m_detailLabel = nullptr;
    QLabel  *m_warningLabel= nullptr;
    QPushButton *m_acceptBtn = nullptr;
    QPushButton *m_rejectBtn = nullptr;

    int m_islandWidth;   // 最終幅（レイアウト計算の基準）
    int m_baseH;
    // 現在の表示幅。setVisibleWidth() が更新する。applyPanelGeometry() の
    // 直後は m_islandWidth と同じ。
    int m_visibleWidth = 0;

    // preferredPanelHeight() のキャッシュ（クランプ前の高さ）。
    // setContent() で無効化し、幅が変わったときだけ計算し直す。
    mutable int m_cachedHeightForWidth = -1;
    mutable int m_cachedUncappedHeight = 0;
};
