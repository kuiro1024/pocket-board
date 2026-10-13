; ================================================================
; PocketBoard インストーラ定義（Inno Setup 6）
; ----------------------------------------------------------------
; 【重要】このファイルが正式なソースです。以前は
; tools/build-setup-from-scratch.ps1 がビルドごとにヒアドキュメントから
; 再生成していましたが、日本語文字列の文字化け（PowerShell 5.1 の
; エンコーディング問題）と設定消失の原因になったため廃止しました。
; インストーラの変更は必ずこのファイルを直接編集すること。
; 【エンコーディング】UTF-8 (BOM付き) で保存すること。BOMが無いと
; ISCC が ANSI として解釈し、日本語のCustomMessagesが文字化けする。
; ================================================================
#define MyAppName "PocketBoard"
; バージョンは build-pocketboard-setup.bat → ISCC /DMyAppVersion=x.y.z で渡される。
; ISCCを直接実行した場合だけ下の既定値が使われる。
#ifndef MyAppVersion
  #define MyAppVersion "0.9.0"
#endif
#define MyAppPublisher "PocketBoard"
#define MyAppExeName "PocketBoard.exe"
#define MySourceDir "..\dist\PocketBoardPortable"

[Setup]
AppId={{4B38C27B-AD65-4AD8-950F-3F62647465E8}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={commonpf}\PocketBoard
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist\installer
OutputBaseFilename=PocketBoardSetup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\ico\ico_light.ico

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
japanese.SystemSettingsGroup=システム設定:
english.SystemSettingsGroup=System settings:
japanese.EnableTransparencyTask=Windows の「透明効果」を有効にする（ON推奨: PocketBoardの半透明ウィンドウ表示に必要です）
english.EnableTransparencyTask=Enable Windows "Transparency effects" (recommended ON: required for PocketBoard's translucent window)
japanese.LaunchAtStartupTask=Windows起動時に PocketBoard を自動で起動する（トレイに常駐。あとからPBの設定 > 全般 で切り替えられます）
english.LaunchAtStartupTask=Launch PocketBoard when Windows starts (runs in the tray; can be changed later in Settings > General)

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
; Windows起動時に自動で起動。PB本体の設定（全般の一番上）と同じ
; HKCU\...\Run の "PocketBoard" を書く（src/StartupRegistration.cpp 参照）。
Name: "launchatstartup"; Description: "{cm:LaunchAtStartupTask}"; GroupDescription: "{cm:SystemSettingsGroup}"; Flags: checkedonce
; Windowsの透明効果（設定 > 個人用設定 > 色 > 透明効果）。
; PocketBoardのアクリル(ブラー)背景に必要なため既定でチェックON。
; OFFのままでも本体側が自前の半透明フォールバック描画で動作はする
; （MainWindow::applyAcrylicOrFallback参照）。
Name: "enabletransparency"; Description: "{cm:EnableTransparencyTask}"; GroupDescription: "{cm:SystemSettingsGroup}"; Flags: checkedonce

[Files]
; Excludes: 万一 dist/PocketBoardPortable に開発用のプラグイン
; （Plugins/フォルダ）や外部ツールを手動で置いたまま忘れても、
; 公式インストーラには絶対に巻き込まないための保険。
; （PocketBoard本体はexe横のPlugins/を自動ロードするため、
;   同梱されるとStore配布方針・利用規約面でまずい）
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "\Plugins\*,\Plugins\*\*,yt-dlp*"

[Registry]
; 透明効果ON (EnableTransparency=1)。
; 注意: 管理者昇格中のHKCUは「昇格したユーザー」のハイブを指すため、
; 標準ユーザーが別の管理者アカウントで昇格した場合はそのユーザー側に
; 書き込まれる（単一ユーザー環境では問題にならない）。
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize"; ValueType: dword; ValueName: "EnableTransparency"; ValueData: "1"; Tasks: enabletransparency
; スタートアップ登録（タスク選択時のみ）。--background でトレイ常駐のみで起動する。
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PocketBoard"; ValueData: """{app}\{#MyAppExeName}"" --background"; Tasks: launchatstartup
; アンインストール時は、インストール時の選択に関係なく必ず消す
; （インストール後にPBの設定からオンにした場合も残さないため）。
; ValueType: none は何も書かず、uninsdeletevalue の削除だけを行う。
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "PocketBoard"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: none; ValueName: "PocketBoard"; Flags: uninsdeletevalue

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
