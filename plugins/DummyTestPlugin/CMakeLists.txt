# ============================================================
#  DummyTestPlugin
#
#  Phase 8 Plugin API 動作確認用のダミープラグイン。
#  PocketBoard.exe と同じ Qt6/MinGW64 環境でビルドし、
#  IPbMediaPlugin (QPluginLoader経由) として読み込まれる MODULE。
#
#  【意図的に PocketBoard.exe 横の Plugins/ へは自動配置しない】
#  本体と完全に切り離す方針のため、通常のビルドツリー
#  （add_subdirectory の第2引数で指定した pluginbuild/DummyTestPlugin/
#  配下）にのみ出力する。動作確認したい場合は .dll を手動で zip化し、
#  実行中の PocketBoard へドラッグ&ドロップしてインストールすること。
# ============================================================

add_library(DummyTestPlugin MODULE
    DummyTestPlugin.cpp
    DummyTestPlugin.h
)

target_include_directories(DummyTestPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(DummyTestPlugin PRIVATE
    Qt6::Core
)

set_target_properties(DummyTestPlugin PROPERTIES
    PREFIX ""
)
