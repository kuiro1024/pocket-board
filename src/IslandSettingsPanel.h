#pragma once

#include <QFrame>
#include <QHash>
#include <QList>

#include "IslandControlRegistry.h"
#include "IslandTabGoo.h"

class QScrollArea;
class QVBoxLayout;
class IslandSettingsTabBar;
class IslandSettingsRow;
class IslandHelpGuideWidget;

// ============================================================
//  IslandSettingsPanel
//
//  島（DynamicIsland）の設定パネル。旧実装（DynamicIsland::
//  toggleSettingsPanel() 内の PANEL_H=220 固定＋move()手動座標配置）を
//  置き換える自動レイアウト版。
//
//  - IslandControlRegistry のセクション一覧からウィジェットを生成する
//    （rebuild()）。コントロールが増減すればパネルの必要高さも変わる
//  - 中身は QScrollArea に載っており、preferredPanelHeight() が
//    ウィンドウ高さ由来の上限（maxTotalHeight）を超える場合は
//    スクロールで全項目にアクセスできる
//  - 高さアニメーション自体は従来どおり DynamicIsland 側
//    （m_heightAnim）が担当し、本クラスは「必要高さの計算」と
//    「ジオメトリの適用」だけを行う
//
//  旧実装からの対応:
//    旧 QFrame *m_settingsPanel（DynamicIsland.cpp内で直接構築）
//    → 本クラス。QSSは旧実装の文字列を移設・一元化した。
//    Grid Size の自前▲▼ステッパー（QToolButton）もそのまま部品化。
// ============================================================
class IslandSettingsPanel : public QFrame
{
    Q_OBJECT

public:
    explicit IslandSettingsPanel(int islandWidth, int baseH, QWidget *parent = nullptr);

    // IslandControlRegistry の現内容からUIを再構築する。
    // 既存の行ウィジェットはすべて破棄して作り直す（項目数は高々
    // 数十なのでコスト無視できる。開いたままのプラグイン追加等で呼ばれる）。
    // 2026-09-15: 作るのは選択中のタブ（カテゴリ）の項目だけ。タブの一覧も
    // ここで作り直す（プラグインが無ければ「プラグイン」タブは出さない）。
    void rebuild();

    // ---- タブ（2026-09-15）----
    // 島の上端の帯（設定ボタンと最小化ボタンの間）に置くカテゴリのタブ。
    // left       : 島の左端からタブの左端まで
    // rightSpace : タブの右端から島の右端まで
    // 島の幅が変わってもこの2つは一定（ボタンが両端に寄っているため）。
    void setTabBarInsets(int left, int rightSpace);

    // ---- 行の揃え線（2026-09-16）----
    // 設定項目はすべて「1項目＝1行」で、次の3本の縦線に揃えて並ぶ。
    // 値はいずれも島ローカル座標で、島の上端のボタンから取る（DynamicIsland が渡す）:
    //   markerX       … 行頭の点の中心。＋ボタンの中心と同じ線
    //   labelX        … 項目名の左端。タブの左端と同じ線
    //   controlRight  … 島の右端から「操作部品の右端」までの距離。×ボタンの中心と同じ線
    // 後から追加された項目やプラグインの項目も、種類（PbControlKind）に
    // 応じた部品を右側に置くだけで、この規則に自動的に従う。
    void setRowGuides(int markerX, int labelX, int controlRightInset);
    // 2026-09-15: 分裂／吸い込みアニメ（IslandTabGooLayer）の間は、静止用の
    // タブを隠す（同じ場所にレイヤーが雫として描くため）。
    void setTabBarSuppressed(bool suppressed);
    // 2026-09-22: タブの面（GL側）の登録を今の位置・島の不透明度へ合わせ直す。
    // 島のサイズ・不透明度が変わったときに DynamicIsland から呼ばれる。
    void syncTabGlass();
    // レイヤーが途中の文字・アイコンを描くための問い合わせ
    QVector<IslandTabGoo::TabItem> tabItems() const;
    int currentTabIndex() const { return m_categories.indexOf(m_currentCategory); }

    // 全項目を表示するのに必要なパネル全体の高さ（島上部の丸UI部分
    // baseH を含む）。maxTotalHeight でクランプされ、超過分は
    // スクロールになる。
    int preferredPanelHeight(int maxTotalHeight) const;

    // パネル自身と内部スクロール領域のジオメトリを適用する。
    // 2026-09-15: 設定パネルは島ごとウィンドウ全体へ展開するようになった。
    // width/panelHeight は展開し切ったときの島のサイズ。行は島の幅いっぱいに
    // 広がり、スクロールバーは島の右端に置く。
    void applyPanelGeometry(int width, int panelHeight);

    // 展開アニメーション中の島の幅に合わせて、上端のタブの幅を合わせる。
    // 2026-09-15: 項目の列は左寄せになったので、ここで動かすのはタブだけ。
    // タブは自前描画の軽いウィジェットなので、毎フレームのリサイズで問題ない
    // （スクロール領域はレイアウトの再計算が走るので毎フレームは触らない）。
    void setVisibleWidth(int islandWidth);

    // 項目の列の幅（構築時の島の幅）。
    int columnWidth() const { return m_islandWidth; }

protected:
    // 値系ウィジェット（SpinBox/ComboBox/Slider）上のホイールで値が
    // 変わる事故を防ぐ。ホイールイベントはスクロールエリアへ転送し、
    // 「パネルのスクロール」としてのみ機能させる（installWheelGuard参照）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // w のホイールイベントを本パネル（eventFilter）で横取りする。
    void installWheelGuard(QWidget *w);
    // 行の右側に置く操作部品だけを作る（項目名は行が持つ）。
    QWidget *createControlWidget(const IslandControlEntry &entry);
    void addSectionHeading(const QString &title);
    // 行を並べ直す（揃え線やパネル幅が変わったとき）
    void relayoutRows();
    void clearContent();
    // 登録済みセクションから、出すべきタブ（カテゴリ）の一覧を作る。
    QList<IslandControlCategory> availableCategories() const;
    void selectCategory(IslandControlCategory category);

    // ---- 実行時の状態上書き（PbControlHost経由。2026-07-13追加）----
    // Registryが保持する上書き（有効/無効・Label/Button文言）をウィジェット
    // 1つへ適用する。構築時（rebuild内）と controlStateChanged 受信時の
    // 両方から使う。文言変更は QLabel / QPushButton の場合のみ有効
    // （他kindのコンテナには enabled のみ適用）。
    static void applyControlStateToWidget(QWidget *w, const IslandControlState &state);
    // controlStateChanged 受信スロット。開いているパネルの該当ウィジェット
    // だけをその場更新する（rebuild()はしない。クリック処理中のボタン破棄・
    // スクロール位置リセットを避けるため）。
    void onControlStateChanged(const QString &sectionId, const QString &controlId);
    // pushControlValue 受信。部品の値だけを通知なしで差し替える（2026-09-16）。
    void onControlValuePushed(const QString &sectionId, const QString &controlId, const QVariant &value);
    static QString controlWidgetKey(const QString &sectionId, const QString &controlId)
    { return sectionId + QLatin1Char('/') + controlId; }

    QScrollArea *m_scroll  = nullptr;
    // 2026-09-16: 行の揃え線（setRowGuides）と、今並んでいる行
    int m_markerX = 21;
    int m_labelX = 47;
    int m_controlRightInset = 21;
    int m_panelWidth = 0;
    QList<IslandSettingsRow *> m_rows;
    // 2026-09-18: ヘルプタブのときだけ作られる操作ガイド（行の代わり）
    IslandHelpGuideWidget *m_helpWidget = nullptr;
    IslandSettingsTabBar *m_tabBar = nullptr;
    QList<IslandControlCategory> m_categories;
    // 選択中のタブ。閉じて開き直しても保持する。
    IslandControlCategory m_currentCategory = IslandControlCategory::Appearance;
    int m_tabLeft = 0;
    int m_tabRightSpace = 0;
    bool m_tabBarSuppressed = false;
    QWidget     *m_content = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
    int m_islandWidth;
    int m_baseH;

    // "sectionId/controlId" → 生成済みウィジェット。rebuild()のたびに
    // 作り直される（clearContent()でクリア）。
    QHash<QString, QWidget *> m_controlWidgets;
};
