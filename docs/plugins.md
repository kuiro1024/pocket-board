# PocketBoard Plugin Guide

PocketBoard はプラグインで機能を足せます。本体は「バブルを並べて再生・表示する」ことだけを受け持ち、
**どこから何を持ってくるか** はプラグインが決めます。

> English: This guide is currently written in Japanese. The API headers in `src/plugin/` are the source of truth.

- [プラグインを使う（インストール）](#プラグインを使うインストール)
- [プラグインでできること](#プラグインでできること)
- [プラグインを作る](#プラグインを作る)
- [各インターフェースの詳細](#各インターフェースの詳細)
- [注意点・ルール](#注意点ルール)

---

## プラグインを使う（インストール）

### インストール
1. プラグインの `.zip` ファイルを、起動中の PocketBoard にドラッグ＆ドロップする
2. 確認の表示が出るので、内容を確かめて承認する

`.zip` の中身は次の場所へ展開されます。

```
%APPDATA%\PocketBoard\PocketBoard\Plugins\<zipの名前>\
```

### 更新
同じ名前の `.zip` をもう一度ドロップすると上書きされます（設定は残ります）。
使用中の DLL は Windows がロックしているため、失敗した場合は PocketBoard を再起動してからドロップし直してください。

### 削除
PocketBoard を終了して、上の場所にあるプラグインのフォルダを削除してください。

### 読み込まれる場所
起動時に、次の2か所が（サブフォルダも含めて）読み込まれます。同じ ID のプラグインがある場合は 1 が優先されます。

1. `%APPDATA%\PocketBoard\PocketBoard\Plugins\`（.zip ドロップでのインストール先）
2. `PocketBoard.exe` と同じ場所の `Plugins\`（開発時の手動配置用）

### 設定
プラグインが設定項目を持っている場合、PocketBoard の設定画面の **プラグイン** タブに表示されます。

> ⚠️ プラグインは PocketBoard の中でそのまま動くプログラム（DLL）です。信頼できる配布元のものだけを入れてください。

---

## プラグインでできること

| やりたいこと | 使うインターフェース | 必須 |
|---|---|---|
| URL を受け取って、再生できる動画・音声に変える | `IPbMediaPlugin` | ✅ |
| タイトル・サムネイル・シークバーの見た目を指定する | `IPbMediaPlugin`（結果の項目） | |
| 再生が終わったら次の動画へ続ける | `IPbMediaPlugin`（`nextUrl`） | |
| バブル左上に小さなラベル（ロゴ＋文字）を出す | `IPbMediaPlugin`（`labelIconSvg` / `labelText`） | |
| ラベルを押すと前後の一覧を出す | `IPbPlaylistNavigator` | |
| ラベル長押しで別のものに切り替える | `IPbLabelHoldActionProvider` | |
| 起動直後からラベルの絵を最新にする | `IPbLabelPreviewProvider` | |
| サムネイル画像を自前で用意する | `IPbThumbnailProvider` | |
| バブルを重ねてリストにまとめる操作を制限する | `IPbFusionPolicyProvider` | |
| 設定画面に自分の設定項目を出す | `IPbControlProvider` | |

`IPbMediaPlugin` だけは必須で、ほかはすべて任意です。実装していないものは「その機能は無し」として扱われます。

### 本体とプラグインの役割分担
| 本体（PocketBoard） | プラグイン |
|---|---|
| バブルの描画・配置・保存 | どの URL を担当するか判断する |
| 再生（libmpv） | URL を実際に再生できるストリームへ変換する |
| UI の見た目・操作・アニメーション | 「何を表示するか」の中身（タイトル・ラベル・一覧など） |
| 設定値の保存 | 設定項目の宣言と、値が変わったときの処理 |

プラグインが任意のウィジェットや UI コードを注入することはできません。プラグインは **データを返すだけ** で、見せ方は本体が統一します。

### URL がプラグインに届く順番
URL をドロップ（または貼り付け）すると、本体は次の順で扱いを決めます。

1. 画像の URL → 本体が画像として表示
2. **`canHandle()` が `true` を返したプラグイン → プラグインに任せる**
3. 動画ファイルの直リンク（`.mp4` など） → 本体が再生
4. それ以外の http(s) URL → Web リンクのバブル

つまり、画像以外の URL であれば、プラグインが本体より先に引き受けられます。

---

## プラグインを作る

### 必要な環境
プラグインは **PocketBoard 本体と同じ Qt のバージョン・同じコンパイラ（MSYS2 MINGW64）** でビルドする必要があります（Qt のプラグイン機構の制約です）。
ビルド環境の用意は [README](../README.md#ビルド--build) を参照してください。

API は **Qt6::Core だけ** に依存します。Qt6::Gui や Widgets をリンクする必要はありません。

### 最小のプラグイン
`plugins/DummyTestPlugin/` が動くサンプルです。ここでは最小限の形を示します。

**MyPlugin.json**
```json
{
    "Keys": [ "com.example.myplugin" ]
}
```

**MyPlugin.h**
```cpp
#pragma once
#include <QObject>
#include "plugin/PbPluginApi.h"

class MyPlugin : public QObject, public IPbMediaPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IPbMediaPlugin_iid FILE "MyPlugin.json")
    Q_INTERFACES(IPbMediaPlugin)

public:
    QString pluginId() const override      { return QStringLiteral("com.example.myplugin"); }
    QString displayName() const override   { return QStringLiteral("My Plugin"); }
    QString pluginVersion() const override { return QStringLiteral("0.1.0"); }

    bool canHandle(const QString &url) const override;
    void resolve(const QString &url, int targetHeight,
                 const PbResolveCallback &callback) override;
};
```

**MyPlugin.cpp**
```cpp
#include "MyPlugin.h"
#include <QTimer>
#include <QUrl>

bool MyPlugin::canHandle(const QString &url) const
{
    // 軽い判定だけを行う（ネットワークアクセスはしない）。
    return QUrl(url).scheme() == QStringLiteral("myscheme");
}

void MyPlugin::resolve(const QString &url, int targetHeight,
                       const PbResolveCallback &callback)
{
    Q_UNUSED(targetHeight)
    // 時間のかかる処理は非同期で行い、結果は必ずメインスレッドで1回だけ返す。
    QTimer::singleShot(0, [url, callback]() {
        PbResolvedMedia r;
        r.success   = true;
        r.streamUrl = QStringLiteral("https://example.com/video.mp4"); // libmpv に渡す URL
        r.title     = QStringLiteral("Example");
        callback(r);
    });
}
```

**CMakeLists.txt**
```cmake
cmake_minimum_required(VERSION 3.16)
project(MyPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core)

# PocketBoard のソースの場所（src/ にある API ヘッダーを使う）
set(POCKETBOARD_SOURCE_DIR "C:/path/to/pocket-board" CACHE PATH "")

add_library(MyPlugin MODULE MyPlugin.cpp MyPlugin.h)
target_include_directories(MyPlugin PRIVATE ${POCKETBOARD_SOURCE_DIR}/src)
target_link_libraries(MyPlugin PRIVATE Qt6::Core)
set_target_properties(MyPlugin PROPERTIES PREFIX "")   # libMyPlugin.dll ではなく MyPlugin.dll にする
```

### 動作確認と配布
1. ビルドしてできた `MyPlugin.dll` を `.zip` にまとめる
2. 起動中の PocketBoard にドロップしてインストールする
3. `canHandle()` が `true` を返す URL をドロップして確かめる

プラグインが Qt6::Core 以外の DLL（追加の Qt モジュールや外部ライブラリ）に依存する場合は、**それらの DLL も同じ .zip に入れてください**。
本体は、プラグインを読み込む間だけプラグインのフォルダを DLL の検索先に加えます。

```
MyPlugin.zip
├─ MyPlugin.dll
└─ SomeDependency.dll   ← 依存 DLL は同じ階層に置く
```

ログは `%APPDATA%\PocketBoard\PocketBoard\log.txt` に、`[Plugin]` で始まる行として出力されます。
読み込みの失敗などの警告は、配布版（Release ビルド）でも記録されます。
自分でビルドした Debug ビルドの本体で動かすと、読み込みに成功したときに、どの任意インターフェースが有効になったかも1行で表示されます。

```
[Plugin] com.example.myplugin optional interfaces: thumbnail=no playlist=no fusion=no labelHold=no controls=yes labelPreview=no
```

---

## 各インターフェースの詳細

すべての定義は `src/plugin/` にあります。ここでは要点だけをまとめます。

### `IPbMediaPlugin`（必須）— `PbPluginApi.h`
| メソッド | 内容 |
|---|---|
| `pluginId()` | プラグインを一意に識別する ID。JSON の `Keys` と同じにする |
| `displayName()` | 表示名（設定画面の見出しなどに使われる） |
| `pluginVersion()` | バージョン文字列 |
| `canHandle(url)` | この URL を担当するか。**すぐ返すこと**（ネットワーク禁止） |
| `resolve(url, targetHeight, callback)` | URL を再生できる形に変換して `callback` を **1回だけ** 呼ぶ |

`targetHeight` は希望する画質の高さ（px）です。0 は指定なし。対応しなくても構いません。

#### `PbResolvedMedia`（resolve の結果）
| 項目 | 内容 |
|---|---|
| `success` / `errorMessage` / `errorCode` | 成功したか、失敗の理由（ログ用）、失敗の分類 |
| `streamUrl` | libmpv に渡す再生用の URL（必須） |
| `audioStreamUrl` | 映像と音声が別々のときの音声 URL（空なら `streamUrl` に音声も含まれる） |
| `httpHeaders` | 取得に必要な HTTP ヘッダー（例: `Referer: ...`） |
| `title` / `thumbnailUrl` | 表示用のタイトルとサムネイル |
| `resolvedWidth` / `resolvedHeight` | 実際に選んだ解像度（分かれば） |
| `seekBarVisible` / `seekBarTimeMode` / `seekBarTimeModeLocked` / `seekBarColor` | シークバーの表示（ライブ配信向けの残り時間表示など） |
| `liveEdgeBufferMs` | ライブ配信で、最新の位置からどれだけ遅らせて再生するか |
| `nextUrl` / `nextUrlPending` | 再生が終わったら続けて開く URL（連続再生） |
| `labelIconSvg` / `labelText` | バブル左上のラベル（SVG ロゴ＋短い文字）。ロゴは円形に切り抜かれる |
| `labelHoverable` / `labelPinnable` / `labelProgress` | ラベルが反応するか・固定できるか・再生位置を縁に表示するか |

失敗の分類（`PbResolveErrorCode`）は、画面には3桁の数字だけが表示されます。

| 値 | 意味 |
|---|---|
| `NotFound` (3) | 対象が存在しない |
| `Unsupported` (4) | 再生できる形式が見つからない |
| `Network` (5) | ネットワークエラー |
| `ToolMissing` (2) | 必要な外部ツールが見つからない |
| `PluginError` (6) | その他のプラグイン内部のエラー |

### `IPbControlProvider`（設定項目）— `PbControlApi.h` / `PbControlSpec.h`
`controlSpecs()` で設定項目を宣言するだけで、設定画面の部品は本体が作り、値も本体が保存します。

- 使える部品：`Toggle` / `Slider` / `SpinBox` / `Choice` / `Button` / `Label` / `TextInput` / `KeyInput`
- `sameRowAsPrevious = true` で、直前の部品と同じ行に並べられます（1行に3個まで）
- 1つのプラグインにつき最大12個まで。ID は64文字、表示名は48文字まで
- 値はプラグインのフォルダ内の `plugin_config.json` に本体が保存します（プラグインは直接触らないこと）
- 起動時には `onControlValueRestored()` で保存値が渡されます
- `onControlHostReady()` で受け取る `PbControlHost` を使うと、項目の有効/無効や文字を後から変えられます

### `IPbPlaylistNavigator`（前後の一覧）— `PbPlaylistApi.h`
ラベルを押したときに出る一覧の中身を返します。一覧の描画や操作は本体が行います。
`playlistContext()` は **すぐ返すこと**。まだ分からなければ `pending = true` を返すと、本体が控えめに聞き直します。

### `IPbLabelHoldActionProvider` / `IPbLabelPreviewProvider` — `PbLabelActionApi.h`
- ラベルを長押しして、たまりきったときの動作を決められます（`None` / `Core` / `Plugin`）
- `Plugin` の場合は `runLabelHoldAction()` が呼ばれ、切り替え先の URL を返します。`keepPlaying = true` にすると、再生を止めずに切り替えられます
- `IPbLabelPreviewProvider` を実装すると、保存していたバブルを開いた直後から最新のラベルの絵を出せます

### `IPbThumbnailProvider`（サムネイル）— `PbThumbnailApi.h`
サムネイル画像のデータ（PNG / JPEG など）を自前で返せます。実装しない場合は、`thumbnailUrl` から本体が取得します。

### `IPbFusionPolicyProvider`（リストへのまとめ）— `PbFusionApi.h`
バブルを別のバブルに重ねてリストにまとめる操作を、このメディアでは受け付けない・加わらない、と断れます。

### 複数のインターフェースを実装する
`IPbMediaPlugin` と多重継承し、`Q_INTERFACES` に並べて書きます。

```cpp
class MyPlugin : public QObject, public IPbMediaPlugin, public IPbControlProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IPbMediaPlugin_iid FILE "MyPlugin.json")
    Q_INTERFACES(IPbMediaPlugin IPbControlProvider)
    ...
};
```

---

## 注意点・ルール

### スレッド
- 本体からの呼び出しは、すべて Qt のメインスレッドから行われます
- `callback` も **必ずメインスレッドで** 呼んでください。ワーカースレッドから直接呼ばず、`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` などで戻します
- 次のメソッドは画面の更新中に何度も呼ばれます。**ネットワーク・外部プロセス・ファイル I/O を行わず、すぐ返してください**
  - `canHandle()` / `playlistContext()` / `fusionPolicy()` / `labelHoldAction()` / `labelPreview()`

### API のバージョン（IID）
各インターフェースには `"com.pocketboard.IPbMediaPlugin/2.1"` のようなバージョン付きの ID があります。
**本体と同じバージョンのヘッダーでビルドしたプラグインだけが読み込まれます。**
本体を更新して ID が変わった場合は、プラグインを新しいヘッダーでビルドし直してください。

任意インターフェースの ID だけが違う場合は、その機能だけが無効になり、プラグイン自体は読み込まれます。

### やってはいけないこと
- `plugin_config.json` を直接書き換える（本体が管理しています）
- `callback` を呼ばない、または2回以上呼ぶ
- 利用規約や法律に反する方法でコンテンツを取得する

プラグインは、作った人の責任で配布してください。PocketBoard 本体は、個々のプラグインの動作や配布について責任を負いません。
