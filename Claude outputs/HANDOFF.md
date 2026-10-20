# Pocket Board 公式サイト 引き継ぎメモ

## 何を作っているか
- PocketBoard（Windows のトレイ常駐アプリ、作者名「Peach」、X: https://x.com/Peach4702527919）の紹介サイト。
- 公開先: GitHub Pages（リポジトリ `kuiro1024/pocket-board` の `main` ブランチ `/docs`。Pages 設定は済み）。
- 方向性: おしゃれ系（ゲームっぽくしない）。白いガラス風UI。Apple のサイトのようなシームレスなスクロール演出がほしい。
- プレビュー（Artifact）: https://claude.ai/artifact/6VFeFejnNTVepR3F7rvUFd

## フォルダ構成（このzip）
- `page.html` … ページ本体（HTML/CSS/ページ用の小さなJS）。`build.py` で `index.html` を作る。
- `src/` … デモ等のJS。`build_app.py` で連結して `assets/app.js` を作る。
  - `gl.js` … WebGL2 のバブル描画（本体 GlassHighlightEffect の GLASS_FRAG を移植。シェーダー本体は `glass_frag.glsl` を JSON で埋め込む）
  - `demo.js` … サイト上で触れる PB デモ（本体ソースに合わせた挙動。下記）
  - `figs.js` … 機能紹介の画像を画像バブルとして描く
  - `switch.js` … Standard/Carved/Gummy のスライダー（ドラッグ可、島と同じガラスの台、グレー）
  - `island.js` … 本体 DynamicIsland の描画（`paintIslandBody` は他のUIでも使用）
  - `paint.js` / `art.js` / `sound.js` … デモ用の動画の中身・アイコン・音量アイコン
  - `flow.js` … 旧: WebGL の背景の流れ（現在は未使用。CSS の光のかたまりに置き換え済み）
- `assets/` … 画像・動画（promo.mp4 は最新版 54.6秒・音声付き）・`app.js`
- `docs/` … GitHub の `docs/` に置く完成品（index.html はスクリプト埋め込み済み）
- `glass_frag.glsl` … ガラスのシェーダー（build_app.py が読む）

## ビルド
```
python3 build_app.py   # src/*.js → assets/app.js（build_app.py 内の glass_frag.glsl のパスを要調整）
python3 build.py       # page.html → index.html（app.js をページ内に埋め込む）
```
※ Artifact のプレビューでは「自前ファイルからのスクリプト読み込み」と「data: の SVG 画像」が使えないため、app.js はページ内に埋め込み、アイコン類は PNG ファイルにしてある。

## ページ構成
1. 上部バー（固定・スクロールで白いガラスの帯に）: ロゴ / JA・EN / X / GitHub
2. ヒーロー: 「動画も。音声も。画像も。／すべてをひとつのキャンバスに。」＋アイコンとロゴ、Store（白・目立つ）とインストーラ（ガラス）、プロモ動画（ガラス窓、音ボタン）
3. 触れるデモ（Try it）＋その下に Standard/Carved/Gummy スライダー（場所はここ。勝手に動かさないこと）
4. 機能紹介（画像はバブル。スライダーに追従。ホバーで1.2%だけなめらかに拡大）
5. ダウンロード（Store / インストーラ v1.3.0 / GitHub、Ko-fi）
6. フッター（© 2026 Peach）
- 英語の文言: 「Your Video, Audio, and Images / All on One Canvas.」（ユーザー指定）

## デモPB（本体ソース src/ に合わせた挙動）
- ドロップ: 出現アニメなし、その場に置く（中身の短辺162px）。サイトは Web リンクのランチャー（favicon が無いときの「ホスト名の色の丸＋頭文字」）
- 左クリック: アクティブ化（青い枠 140,195,255 が 190↔255 で 900ms 脈打つ）。動画は280ms待ってから再生/一時停止
- 左ドラッグ: 移動（lift 1.025）／右下の印でリサイズ／空白は範囲選択。離すと重なったバブルを押し出し（computeOverlapPushOffsets、420ms OutBack）
- ダブルクリック（2回目を離して確定）: 全画面。カメラは animateCamera の式（ズーム対数補間、アンカー基準の画面座標補間、寄る500ms・戻る300ms、InOutCubic）、ウィンドウは calcFocusSize で中身の対比へ同時に変化。解除後はキャンバス固定（元の大きさ）
- 2回目を押したまま動かす／Shift+ドラッグ: 逆輸入（キャンバス暗転 α120・200ms、プレビュー最大160×120）→ 右のエクスプローラーへ落とすと追加
- 中ボタン: パン（CameraPanInertia: EMA0.35、離す前30ms静止で慣性なし、40未満は無し、摩擦0.06/秒）／ホイール: 1コリ10%をなめらかに（PB上ではページをスクロールしない）
- 右: クリックでアクティブ化のみ、ドラッグでウィンドウ移動、ダブルで全画面
- 島: 上中央300×80にカーソルで出る（幅42→240 OutBack500ms、ボタン順に出現）。操作中は隠れる。×/− で隠れて Ctrl+Shift+Space で戻る、□ で最大化
- 再生バー: CanvasVideo::paintPlaybackOverlay どおり（再生/音量の丸、シーク、時刻の台80×32、出るときポップ、全画面中は2秒で隠れる）

## 未解決・要確認
- **ユーザーの環境で「背景がうにうに動かない」「セクションが重なっていく演出が無い」**。こちらのテストブラウザでは動いている。
  - 有力な原因: Windows の「アニメーション効果」がオフ（= prefers-reduced-motion: reduce）だと、このページは意図的に動きを止める作りになっている（CSS で animation:none、JS で重なり演出を外す）。まず確認すること。ユーザーが常に動きを見たいなら、reduce 時の停止をやめる／弱めるだけにする。
  - 次点: Artifact プレビュー固有の制限。GitHub Pages に置いて確認するのが確実。
- GitHub への push は権限エラー（403）。Claude の GitHub App を `kuiro1024/pocket-board` に入れるか、`docs/` を手動で置いて push。
- ユーザーの要望: もっと「流動」している背景、Apple のような継ぎ足されていくシームレスなスクロールアニメーション。
