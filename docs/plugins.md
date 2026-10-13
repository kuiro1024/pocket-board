# PocketBoard Plugin Guide

**English** | [日本語](plugins.ja.md)

PocketBoard can be extended with plugins. The app itself only takes care of laying out bubbles and playing or showing them —
**where the media comes from** is up to the plugin.

- [Using plugins (installation)](#using-plugins-installation)
- [What plugins can do](#what-plugins-can-do)
- [Writing a plugin](#writing-a-plugin)
- [Interface reference](#interface-reference)
- [Rules and notes](#rules-and-notes)

---

## Using plugins (installation)

### Install
1. Drag and drop the plugin's `.zip` file onto a running PocketBoard window
2. A confirmation appears — check it and accept

The contents of the `.zip` are extracted to:

```
%APPDATA%\PocketBoard\PocketBoard\Plugins\<zip name>\
```

### Update
Drop a `.zip` with the same name again to overwrite it (settings are kept).
Windows locks DLLs that are in use, so if the update fails, restart PocketBoard and drop the file again.

### Uninstall
Quit PocketBoard and delete the plugin's folder in the location above.

### Where plugins are loaded from
On startup, these two folders are scanned (including subfolders). If both contain a plugin with the same ID, the one in 1 wins.

1. `%APPDATA%\PocketBoard\PocketBoard\Plugins\` (where `.zip` installs go)
2. `Plugins\` next to `PocketBoard.exe` (for manual placement during development)

### Settings
If a plugin has settings, they appear in the **Plugins** tab of PocketBoard's settings.

> ⚠️ A plugin is a program (DLL) that runs inside PocketBoard. Only install plugins from sources you trust.

---

## What plugins can do

| What you want to do | Interface | Required |
|---|---|---|
| Turn a URL into playable video/audio | `IPbMediaPlugin` | ✅ |
| Set the title, thumbnail and seek bar appearance | `IPbMediaPlugin` (result fields) | |
| Continue to the next item when playback ends | `IPbMediaPlugin` (`nextUrl`) | |
| Show a small label (logo + text) at the top-left of a bubble | `IPbMediaPlugin` (`labelIconSvg` / `labelText`) | |
| Open a list of previous/next items when the label is clicked | `IPbPlaylistNavigator` | |
| Switch to something else when the label is long-pressed | `IPbLabelHoldActionProvider` | |
| Show an up-to-date label icon right after startup | `IPbLabelPreviewProvider` | |
| Provide thumbnail images yourself | `IPbThumbnailProvider` | |
| Restrict merging bubbles into a list | `IPbFusionPolicyProvider` | |
| Add your own settings to the settings screen | `IPbControlProvider` | |

Only `IPbMediaPlugin` is required. Everything else is optional; anything you don't implement is simply treated as "not available".

### Who does what
| PocketBoard (host) | Plugin |
|---|---|
| Drawing, layout and saving of bubbles | Deciding which URLs it handles |
| Playback (libmpv) | Converting a URL into a playable stream |
| UI appearance, interaction and animation | The content to show (title, label, lists, …) |
| Storing setting values | Declaring settings and reacting to changes |

Plugins cannot inject arbitrary widgets or UI code. A plugin **only returns data**; the host keeps the look and feel consistent.

### The order in which a URL reaches plugins
When a URL is dropped (or pasted), the host decides how to handle it in this order:

1. Image URL → shown as an image by the host
2. **A plugin whose `canHandle()` returns `true` → handed to that plugin**
3. Direct link to a video file (`.mp4`, etc.) → played by the host
4. Any other http(s) URL → a web link bubble

In other words, for anything other than image URLs, plugins get the first chance to take the URL.

---

## Writing a plugin

### Requirements
A plugin must be built with **the same Qt version and the same compiler (MSYS2 MINGW64) as PocketBoard itself** (a limitation of Qt's plugin system).
See the [README](../README.md#build) for setting up the build environment.

The API depends on **Qt6::Core only**. You do not need to link Qt6::Gui or Widgets.

### A minimal plugin
`plugins/DummyTestPlugin/` is a working sample. Here is the bare minimum.

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
    // Keep this check cheap (no network access).
    return QUrl(url).scheme() == QStringLiteral("myscheme");
}

void MyPlugin::resolve(const QString &url, int targetHeight,
                       const PbResolveCallback &callback)
{
    Q_UNUSED(targetHeight)
    // Do slow work asynchronously, and always return the result exactly once on the main thread.
    QTimer::singleShot(0, [url, callback]() {
        PbResolvedMedia r;
        r.success   = true;
        r.streamUrl = QStringLiteral("https://example.com/video.mp4"); // URL passed to libmpv
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

# Location of the PocketBoard sources (the API headers live in src/)
set(POCKETBOARD_SOURCE_DIR "C:/path/to/pocket-board" CACHE PATH "")

add_library(MyPlugin MODULE MyPlugin.cpp MyPlugin.h)
target_include_directories(MyPlugin PRIVATE ${POCKETBOARD_SOURCE_DIR}/src)
target_link_libraries(MyPlugin PRIVATE Qt6::Core)
set_target_properties(MyPlugin PROPERTIES PREFIX "")   # MyPlugin.dll instead of libMyPlugin.dll
```

### Testing and distribution
1. Put the built `MyPlugin.dll` into a `.zip`
2. Drop it onto a running PocketBoard to install it
3. Drop a URL for which `canHandle()` returns `true` and check the result

If your plugin depends on DLLs other than Qt6::Core (extra Qt modules or third-party libraries), **put those DLLs in the same `.zip`**.
While loading a plugin, the host temporarily adds the plugin's folder to the DLL search path.

```
MyPlugin.zip
├─ MyPlugin.dll
└─ SomeDependency.dll   ← put dependencies at the same level
```

Logs are written to `%APPDATA%\PocketBoard\PocketBoard\log.txt` as lines starting with `[Plugin]`.
Warnings such as load failures are recorded even in the distributed (Release) build.
If you run a Debug build of PocketBoard that you built yourself, a successful load also prints one line showing which optional interfaces were detected.

```
[Plugin] com.example.myplugin optional interfaces: thumbnail=no playlist=no fusion=no labelHold=no controls=yes labelPreview=no
```

---

## Interface reference

All definitions live in `src/plugin/`. The headers have detailed comments (in Japanese); this section is a summary.

### `IPbMediaPlugin` (required) — `PbPluginApi.h`
| Method | Description |
|---|---|
| `pluginId()` | Unique plugin ID. Must match `Keys` in the JSON |
| `displayName()` | Display name (used e.g. as the heading in settings) |
| `pluginVersion()` | Version string |
| `canHandle(url)` | Whether this plugin handles the URL. **Must return immediately** (no network) |
| `resolve(url, targetHeight, callback)` | Convert the URL into something playable and call `callback` **exactly once** |

`targetHeight` is the preferred video height in pixels. 0 means no preference. Supporting it is optional.

#### `PbResolvedMedia` (the result of resolve)
| Field | Description |
|---|---|
| `success` / `errorMessage` / `errorCode` | Whether it succeeded, the failure reason (for logs), and the failure category |
| `streamUrl` | Playback URL passed to libmpv (required) |
| `audioStreamUrl` | Audio URL when video and audio are separate (empty = `streamUrl` contains audio too) |
| `httpHeaders` | HTTP headers needed to fetch the stream (e.g. `Referer: ...`) |
| `title` / `thumbnailUrl` | Title and thumbnail for display |
| `resolvedWidth` / `resolvedHeight` | The resolution actually chosen (if known) |
| `seekBarVisible` / `seekBarTimeMode` / `seekBarTimeModeLocked` / `seekBarColor` | Seek bar display (e.g. a countdown for live streams) |
| `liveEdgeBufferMs` | For live streams, how far behind the live edge to play |
| `nextUrl` / `nextUrlPending` | URL to open next when playback ends (continuous playback) |
| `labelIconSvg` / `labelText` | Label at the top-left of the bubble (SVG logo + short text). The logo is clipped to a circle |
| `labelHoverable` / `labelPinnable` / `labelProgress` | Whether the label reacts to hover, can be pinned, and shows playback progress on its border |

For failure categories (`PbResolveErrorCode`), only a 3-digit number is shown on screen.

| Value | Meaning |
|---|---|
| `NotFound` (3) | The item does not exist |
| `Unsupported` (4) | No playable format found |
| `Network` (5) | Network error |
| `ToolMissing` (2) | A required external tool was not found |
| `PluginError` (6) | Any other internal plugin error |

### `IPbControlProvider` (settings) — `PbControlApi.h` / `PbControlSpec.h`
Declare your settings in `controlSpecs()`; the host builds the UI and stores the values.

- Available controls: `Toggle` / `Slider` / `SpinBox` / `Choice` / `Button` / `Label` / `TextInput` / `KeyInput`
- `sameRowAsPrevious = true` places a control on the same row as the previous one (up to 3 per row)
- Up to 12 controls per plugin. IDs up to 64 characters, labels up to 48
- Values are saved by the host to `plugin_config.json` in the plugin's folder (do not edit it directly)
- On startup, saved values are passed to `onControlValueRestored()`
- The `PbControlHost` received in `onControlHostReady()` lets you enable/disable controls or change their text later

### `IPbPlaylistNavigator` (previous/next list) — `PbPlaylistApi.h`
Returns the contents of the list shown when the label is clicked. The host draws and handles the list.
`playlistContext()` **must return immediately**. If you don't know yet, return `pending = true` and the host will ask again at a modest interval.

### `IPbLabelHoldActionProvider` / `IPbLabelPreviewProvider` — `PbLabelActionApi.h`
- Decide what happens when the label is long-pressed until the gauge fills (`None` / `Core` / `Plugin`)
- With `Plugin`, `runLabelHoldAction()` is called and returns the URL to switch to. Set `keepPlaying = true` to switch without interrupting playback
- Implementing `IPbLabelPreviewProvider` lets restored bubbles show the latest label icon right away

### `IPbThumbnailProvider` (thumbnails) — `PbThumbnailApi.h`
Return thumbnail image data (PNG, JPEG, etc.) yourself. If not implemented, the host fetches `thumbnailUrl`.

### `IPbFusionPolicyProvider` (merging into lists) — `PbFusionApi.h`
Decline merging: refuse to accept other bubbles into this media's list, or refuse to join another bubble's list.

### Implementing several interfaces
Inherit from them alongside `IPbMediaPlugin` and list them all in `Q_INTERFACES`.

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

## Rules and notes

### Threads
- Every call from the host comes from the Qt main thread
- Always invoke `callback` **on the main thread**. Don't call it directly from a worker thread; marshal it back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` or similar
- The following methods are called repeatedly while the screen updates. **Do not do network, external process or file I/O — return immediately:**
  - `canHandle()` / `playlistContext()` / `fusionPolicy()` / `labelHoldAction()` / `labelPreview()`

### API versions (IIDs)
Each interface has a versioned ID such as `"com.pocketboard.IPbMediaPlugin/2.1"`.
**Only plugins built against headers with the same version as the host are loaded.**
If an update to PocketBoard changes an ID, rebuild your plugin with the new headers.

If only an optional interface's ID differs, just that feature is disabled; the plugin itself still loads.

### Don'ts
- Don't edit `plugin_config.json` directly (the host manages it)
- Don't skip calling `callback`, or call it more than once
- Don't obtain content in ways that violate terms of service or the law

Plugins are distributed under their authors' own responsibility. PocketBoard is not responsible for the behaviour or distribution of individual plugins.
