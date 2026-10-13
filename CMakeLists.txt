cmake_minimum_required(VERSION 3.16)
project(PocketBoard VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    # 2026-07-15: Multimedia/MultimediaWidgets（QMediaPlayerフォールバック撤去）
    # とSpatialAudio（2026-07-08 Phase 11調査で元々未使用と判明していたもの）を
    # 削除。復活させる場合は、mpv初期化失敗時のフォールバックやサラウンド音声
    # 周りの設計を先に決めること。
    Gui
    OpenGL
    OpenGLWidgets
    Network
    Svg
)

# ---- libmpv (Msys2 MINGW64) ----
# 既定値は従来どおり C:/msys64/mingw64 を前提とする。別環境でビルドする
# 場合は、これらを変更せずに
#   cmake -DMPV_INCLUDE_DIR=... -DMPV_LIB=... ...
# のようにキャッシュ変数として上書きできる（未指定なら従来と同一の既定値）。
set(MPV_INCLUDE_DIR "C:/msys64/mingw64/include" CACHE PATH "libmpv include directory (Msys2 MINGW64)")
set(MPV_LIB         "C:/msys64/mingw64/lib/libmpv.dll.a" CACHE FILEPATH "libmpv import library (Msys2 MINGW64)")

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/CanvasWidget.cpp
    src/CanvasSessionSerializer.cpp
    src/CanvasUndoHistory.cpp
    src/CanvasDropHandler.cpp
    src/BubbleDragExporter.cpp
    src/CanvasSelectionHelper.cpp
    src/BubbleFusion.cpp
    src/FocusSessionController.cpp
    src/CanvasItem.cpp
    src/ThumbnailLoader.cpp
    src/WebImageCache.cpp
    src/WebVideoCache.cpp
    src/WebImageUrlResolver.cpp
    src/CameraPanInertia.cpp
    src/CameraZoomEase.cpp
    src/CanvasRenderScheduler.cpp
    src/CanvasStaticLayerCache.cpp
    src/PluginMediaResolver.cpp
    src/VideoSleepCoordinator.cpp
    src/VideoRenderResolutionPolicy.cpp
    src/MpvTeardown.cpp
    src/MasterVolumeController.cpp
    src/CanvasVideo.cpp
    src/CanvasVideoLabel.cpp
    src/CanvasOverlayWidget.cpp
    src/CanvasImage.cpp
    src/CanvasLauncher.cpp
    src/WebLinkIcon.cpp
    src/DynamicIsland.cpp
    # ---- 島設定パネルの自動レイアウト化＋コントロール型システム ----
    # （md/island_settings_controls_design_2026-07-13.md 参照）
    src/IslandSettingsPanel.cpp
    src/IslandTabGoo.cpp
    src/IslandControls.cpp
    src/IslandControlRegistry.cpp
    src/IslandHelpGuide.cpp
    # ---- 島から生える確認プロンプト（プラグインzipドロップ時）----
    src/IslandPromptPanel.cpp
    src/ResizeHandle.cpp
    src/TrashZone.cpp
    src/Renderer.cpp
    src/GlassHighlightEffect.cpp
    src/GpuProfiler.cpp
    src/BubbleLayerCache.cpp
    # ---- UI背後のぼかし(Blur.h参照) ----
    src/Blur.cpp
    src/SessionManager.cpp
    src/StartupRegistration.cpp
    # ---- レイヤー機能: 2026-07-19、誤操作事故の懸念により一旦切り離し。
    # 2026-09-23: 使わなくなった昔の機能（Flow・フレーム・レイヤー）は
    # old_fn/ にまとめてある（ビルドには含めない。old_fn/README.md 参照）。
    # ---- Phase 0: 再生基盤骨格 ----
    src/BubbleController.cpp
    src/engine/MpvPlayer.cpp
    src/engine/VideoRenderThread.cpp # 2026-09-15 非同期化 P1
    # ---- D3D11レイヤー分離方式: ステップ1 ----
    src/engine/VideoLayerHost.cpp
    src/engine/VideoLayerSurface.cpp
    # ---- Phase 8: Plugin API 基盤 ----
    src/plugin/PluginManager.cpp
    src/plugin/PluginConfigStore.cpp
    ico/resource.rc
    image/svg/icons.qrc
)

set(HEADERS
    src/MainWindow.h
    src/CanvasWidget.h
    src/CameraPanInertia.h
    src/CameraZoomEase.h
    src/CanvasRenderScheduler.h
    src/CanvasStaticLayerCache.h
    src/GpuProfiler.h
    src/BubbleLayerCache.h
    src/CanvasDropHandler.h
    src/BubbleDragExporter.h
    src/CanvasSelectionHelper.h
    src/BubbleFusion.h
    src/FocusSessionController.h
    src/CanvasItem.h
    src/ThumbnailLoader.h
    src/PluginMediaResolver.h
    src/VideoSleepCoordinator.h
    src/VideoRenderResolutionPolicy.h
    src/MpvTeardown.h
    src/MasterVolumeController.h
    src/CanvasVideo.h
    src/CanvasOverlayWidget.h
    src/CanvasImage.h
    src/CanvasLauncher.h
    src/WebLinkIcon.h
    src/DynamicIsland.h
    # ---- 島設定パネルの自動レイアウト化＋コントロール型システム ----
    src/IslandSettingsPanel.h
    src/IslandTabGoo.h
    src/IslandControls.h
    src/IslandControlRegistry.h
    # ---- 島から生える確認プロンプト（プラグインzipドロップ時）----
    src/IslandPromptPanel.h
    src/ResizeHandle.h
    src/TrashZone.h
    src/Renderer.h
    src/GlassHighlightEffect.h
    src/GlassContentSource.h
    src/AppSettings.h
    src/MediaSource.h
    src/SessionManager.h
    src/StartupRegistration.h
    # ---- レイヤー機能: 2026-07-19、誤操作事故の懸念により一旦切り離し。
    # ---- Phase 0: 再生基盤骨格 ----
    src/BubbleState.h
    src/IPbPlayer.h
    src/BubbleController.h
    src/engine/MpvPlayer.h
    # ---- D3D11レイヤー分離方式: ステップ1 ----
    src/engine/VideoLayerHost.h
    src/engine/VideoLayerSurface.h
    # ---- Phase 8: Plugin API 基盤 ----
    src/plugin/PbPluginApi.h
    src/plugin/PbThumbnailApi.h
    src/plugin/PbPlaylistApi.h
    src/plugin/PbFusionApi.h
    src/plugin/PbLabelActionApi.h
    src/plugin/PluginManager.h
    # ---- プラグインコントロールAPI（島設定パネル拡張） ----
    src/plugin/PbControlSpec.h
    src/plugin/PbControlApi.h
    src/plugin/PluginConfigStore.h
)

qt_add_executable(PocketBoard
    ${SOURCES}
    ${HEADERS}
)

# アプリのバージョンは project(... VERSION x.y.z) が唯一の定義元。
# build-pocketboard-setup.bat で書き換えられ、main.cpp の
# setApplicationVersion へ PB_APP_VERSION として渡る。
target_compile_definitions(PocketBoard PRIVATE PB_APP_VERSION="${PROJECT_VERSION}")

target_include_directories(PocketBoard PRIVATE
    src/
    src/engine/
    src/plugin/
    ${MPV_INCLUDE_DIR}
)

target_link_libraries(PocketBoard PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Network
    Qt6::Svg
    ${MPV_LIB}
    dwmapi
    user32
    advapi32
    psapi
    d3d11
    dxgi
    dcomp
    shell32
    ole32
    uuid
    # 2026-07-08: ローカル動画サムネイル取得（ThumbnailLoader::
    # fetchWindowsShellThumbnail、IShellItemImageFactory::GetImage()の
    # HBITMAP→QImage変換）でGetDIBits/GetObject/GetDC等のGDI APIを使うため。
    gdi32
)

# ビルド後に指定ファイル群を PocketBoard の実行ファイルと同じ階層へ
# copy_if_different する。存在しないファイルは黙ってスキップする
# （従来の各 foreach + if(EXISTS) を1箇所にまとめただけで、コピー対象・
#  コピー先・スキップ条件は変更していない）。
function(pb_copy_files_if_exist FILE_LIST)
    foreach(SRC_FILE ${FILE_LIST})
        if(EXISTS "${SRC_FILE}")
            add_custom_command(TARGET PocketBoard POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SRC_FILE}" $<TARGET_FILE_DIR:PocketBoard>
            )
        endif()
    endforeach()
endfunction()

if(WIN32)
    if(MINGW)
        target_link_options(PocketBoard PRIVATE -mwindows)
    else()
        set_target_properties(PocketBoard PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    # Msys2 の libmpv と依存 DLL をコピー
    set(MSYS2_BIN "C:/msys64/mingw64/bin")
    set(MPV_DLLS
        "${MSYS2_BIN}/libmpv-2.dll"
        "${MSYS2_BIN}/libopenal-1.dll"
        "${MSYS2_BIN}/avcodec-62.dll"
        "${MSYS2_BIN}/avformat-62.dll"
        "${MSYS2_BIN}/avutil-60.dll"
        "${MSYS2_BIN}/avfilter-11.dll"
        "${MSYS2_BIN}/avdevice-62.dll"
        "${MSYS2_BIN}/swresample-5.dll"
        "${MSYS2_BIN}/swscale-8.dll"
        "${MSYS2_BIN}/postproc-57.dll"
        # 2026-07-03: libvapoursynth.dll / libvapoursynth-script-0.dll は
        # objdumpでの依存関係グラフ調査の結果、どこからも参照されていない
        # 浮いたファイルだと判明したため削除（詳細:
        # cpp/imp/dll_trimming_plan_2026-07-03.md）。
    )
    pb_copy_files_if_exist("${MPV_DLLS}")
    # Qt 付属の MinGW ランタイムで Msys2 版を上書き（競合回避）
    # libopenal 等が Msys2 版ランタイムに依存しているため上書きしない
    # Msys2 版ランタイムで統一する
    set(MINGW_RUNTIME_DLLS
        "${MSYS2_BIN}/libgcc_s_seh-1.dll"
        "${MSYS2_BIN}/libstdc++-6.dll"
        "${MSYS2_BIN}/libwinpthread-1.dll"
    )
    pb_copy_files_if_exist("${MINGW_RUNTIME_DLLS}")

    # アイコンを exe と同階層にコピー（起動時に QFile::exists で参照）
    set(PB_ICON_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/ico/ico_light.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/ico/ico_dark.ico"
    )
    pb_copy_files_if_exist("${PB_ICON_FILES}")
endif()

# ---- 2026-10-12: Release のログ ----
# Debug 以外の構成では qDebug()/qInfo() をコンパイル時に消す（引数の組み立て・
# 文字列化も含めて丸ごと無くなる）。qWarning() 以上は残す（エラー報告用）。
# ディレクトリ単位の指定なので、本体と下の add_subdirectory のプラグインの両方に効く。
add_compile_definitions(
    $<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG_OUTPUT>
    $<$<NOT:$<CONFIG:Debug>>:QT_NO_INFO_OUTPUT>
)

# ---- Phase 8: Plugin API ----
# 要約: 通常ビルドでは各プラグインの.dllをPocketBoard.exe横のPlugins/へ
# 自動配置しない（本体とプラグインを完全に別物として扱う方針のため）。
# 各プラグインは、本体(PocketBoard.exe)とは完全に別物として
# 扱う方針のため、通常ビルドで PocketBoard.exe 横の Plugins/ へは
# 自動配置しない。ビルド成果物は pluginbuild/<name>/ 配下にのみ置かれる。
# 動作確認したい場合は、その .dll を手動で zip 化し、実行中の
# PocketBoard へドラッグ&ドロップしてインストールすること
# （PluginManager::installPluginZip 経由で %APPDATA% 側へ導入される）。
#
# 【注意】add_subdirectory() の第2引数でビルドツリーの出力先を明示的に
# "pluginbuild/" 配下に指定している。指定しない場合、CMakeはソースの
# 相対パス "plugins/<name>/" をそのままビルドツリーにも複製するが、
# Windowsのファイルシステムは既定で大文字小文字を区別しないため、
# 実行時プラグインフォルダである "Plugins/"（大文字P）と衝突しうる。
add_subdirectory(plugins/DummyTestPlugin ${CMAKE_BINARY_DIR}/pluginbuild/DummyTestPlugin)

# 2026-10-13: 公開リポジトリに含まれないローカル専用プラグインの読み込み口。
# private/plugins.cmake があればそれを読み込む（無ければ何もしない）。
# clone しただけの環境では本体と公開プラグインだけでビルドできる。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/private/plugins.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/private/plugins.cmake")
endif()
