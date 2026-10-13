# PocketBoard

Windows のトレイに常駐する、ガラスのバブルに動画・画像・リンクを並べて置いておけるボードアプリです。
A Windows tray app that keeps videos, images and links on a board of glass bubbles.

## 主な機能 / Features
- ローカル動画・画像、Web 画像・Web 動画、Web リンクをドラッグ＆ドロップでバブルとして配置
- 複数の動画を同時に再生（libmpv）
- グローバルショートカットキーでいつでも呼び出し、Windows 起動時にトレイへ常駐
- ガラス表現（シンプル／中間／品質優先、グミなどのスタイル）
- プラグイン機構（`plugins/` と `src/plugin/`）で対応メディアを拡張可能

## ダウンロード / Download
- **Microsoft Store**: [PocketBoard を入手](https://apps.microsoft.com/detail/9MZDTC61K4HM)（自動アップデートあり / auto-updates）
- **インストーラ / Installer**: [Releases](../../releases) から `PocketBoardSetup.exe` をダウンロード

## ビルド / Build
動作確認している環境: Windows 11 / MSYS2 MINGW64 / Qt 6 / CMake

1. [MSYS2](https://www.msys2.org/) を入れ、MINGW64 で必要なパッケージを入れる
   ```
   pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-svg mingw-w64-x86_64-mpv
   ```
2. Qt Creator で `CMakeLists.txt` を開いてビルドする
   （または `cmake -S . -B build -G Ninja && cmake --build build`）
3. インストーラを作る場合は [Inno Setup 6](https://jrsoftware.org/isinfo.php) を入れて
   `build-pocketboard-setup.bat` を実行する（バージョン番号を聞かれます）

libmpv の場所が既定（`C:/msys64/mingw64`）と違う場合は、CMake に
`-DMPV_INCLUDE_DIR=... -DMPV_LIB=...` を渡してください。

## プラグイン / Plugins
`plugins/DummyTestPlugin` がプラグインの最小サンプルです。
インターフェースは `src/plugin/PbPluginApi.h` を参照してください。

## ライセンス / License
PocketBoard のソースコードは [MIT License](LICENSE) です。

配布しているバイナリには、以下のサードパーティ製ライブラリが含まれます。それぞれのライセンスに従います。
- [Qt](https://www.qt.io/) — LGPL v3
- [mpv / libmpv](https://mpv.io/) — GPL v2 以降（MSYS2 のビルド）
- [FFmpeg](https://ffmpeg.org/) — LGPL / GPL（MSYS2 のビルド）
