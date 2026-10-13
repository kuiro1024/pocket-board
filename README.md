# PocketBoard

https://github.com/user-attachments/assets/dc7007c4-f247-4852-83cd-cd2946ee93ff

**English** | [日本語](README.ja.md)

A Windows tray app that keeps your videos, images and links on a board of glass bubbles.

## Features
- Drag and drop local videos and images, web images and videos, and web links to place them as bubbles
- Play multiple videos at the same time (libmpv)
- Bring it up anytime with a global shortcut, and have it wait in the tray from Windows startup
- Glass effects (Simple / Balanced / Quality, plus styles such as Gummy)
- Extend supported media with plugins (`plugins/` and `src/plugin/`)

## Download
- **Microsoft Store**: [Get PocketBoard](https://apps.microsoft.com/detail/9MZDTC61K4HM) (auto-updates)
- **Installer**: download `PocketBoardSetup.exe` from [Releases](../../releases)

## Support
PocketBoard is free. If you like it, you can buy me a coffee to support development ☕

[![Support me on Ko-fi](https://img.shields.io/badge/Ko--fi-studio__peach-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/studio_peach)

## Build
Tested with: Windows 11 / MSYS2 MINGW64 / Qt 6 / CMake

1. Install [MSYS2](https://www.msys2.org/) and the required packages in the MINGW64 shell
   ```
   pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-svg mingw-w64-x86_64-mpv
   ```
2. Open `CMakeLists.txt` in Qt Creator and build
   (or `cmake -S . -B build -G Ninja && cmake --build build`)
3. To create the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php) and
   run `build-pocketboard-setup.bat` (it asks for the version number)

If libmpv is not in the default location (`C:/msys64/mingw64`), pass
`-DMPV_INCLUDE_DIR=... -DMPV_LIB=...` to CMake.

## Plugins
Plugins can add new kinds of media and features.
See the **[Plugin Guide](docs/plugins.md)** for how to install, what plugins can do, and how to write one.

- Sample: `plugins/DummyTestPlugin`
- API: `src/plugin/`

## License
The PocketBoard source code is licensed under the [MIT License](LICENSE).

The distributed binaries include the following third-party libraries, each under its own license:
- [Qt](https://www.qt.io/) — LGPL v3
- [mpv / libmpv](https://mpv.io/) — GPL v2 or later (MSYS2 build)
- [FFmpeg](https://ffmpeg.org/) — LGPL / GPL (MSYS2 build)
