# PocketBoard

https://github.com/user-attachments/assets/dc7007c4-f247-4852-83cd-cd2946ee93ff

[English](README.md) | **日本語**

Windows のトレイに常駐する、ガラスのバブルに動画・画像・リンクを並べて置いておけるボードアプリです。

## 主な機能
- ローカル動画・画像、Web 画像・Web 動画、Web リンクをドラッグ＆ドロップでバブルとして配置
- 複数の動画を同時に再生（libmpv）
- グローバルショートカットキーでいつでも呼び出し、Windows 起動時にトレイへ常駐
- ガラス表現（シンプル／中間／品質優先、グミなどのスタイル）
- プラグイン機構（`plugins/` と `src/plugin/`）で対応メディアを拡張可能

## ダウンロード
- **Microsoft Store**: [PocketBoard を入手](https://apps.microsoft.com/detail/9MZDTC61K4HM)（自動アップデートあり）
- **インストーラ**: [Releases](../../releases) から `PocketBoardSetup.exe` をダウンロード

## 応援
PocketBoard は無料で使えます。気に入っていただけたら、コーヒー1杯分の応援をいただけると開発の励みになります ☕

[![Ko-fi で応援する](https://img.shields.io/badge/Ko--fi-studio__peach-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/studio_peach)

## ビルド
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

## プラグイン
プラグインで、扱えるメディアの種類や機能を増やせます。
インストール方法・できること・作り方は **[プラグインガイド](docs/plugins.ja.md)** を参照してください。

- サンプル: `plugins/DummyTestPlugin`
- API: `src/plugin/`

## ライセンス
PocketBoard のソースコードは [MIT License](LICENSE) です。

配布しているバイナリには、以下のサードパーティ製ライブラリが含まれます。それぞれのライセンスに従います。
- [Qt](https://www.qt.io/) — LGPL v3
- [mpv / libmpv](https://mpv.io/) — GPL v2 以降（MSYS2 のビルド）
- [FFmpeg](https://ffmpeg.org/) — LGPL / GPL（MSYS2 のビルド）
