param(
    [string]$BuildDir = "build/Msys2_MinGW_64bit_Release",
    [string]$OutputDir = "dist/PocketBoardPortable",
    [string]$SourceExeName = "PocketBoard.exe",
    [string]$AppExeName = "PocketBoard.exe",
    [string]$Msys2Bin = "C:/msys64/mingw64/bin",
    [string]$WinDeployQt = "C:/msys64/mingw64/bin/windeployqt.exe"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Resolve-Path (Join-Path $Root $BuildDir)
$SourceExe = Join-Path $BuildPath $SourceExeName
$PortablePath = Join-Path $Root $OutputDir
$PortableExe = Join-Path $PortablePath $AppExeName

if (-not (Test-Path $SourceExe)) {
    throw "Executable not found: $SourceExe"
}

if (-not (Test-Path $WinDeployQt)) {
    $cmd = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $WinDeployQt = $cmd.Source
    } else {
        throw "windeployqt.exe not found. Install Qt or pass -WinDeployQt."
    }
}

New-Item -ItemType Directory -Force -Path $PortablePath | Out-Null
Copy-Item -LiteralPath $SourceExe -Destination $PortableExe -Force

& $WinDeployQt --release --no-translations --compiler-runtime $PortableExe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

$dlls = @(
    "libmpv-2.dll",
    "libopenal-1.dll",
    "avcodec-62.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "avfilter-11.dll",
    "avdevice-62.dll",
    "swresample-6.dll",
    "swscale-9.dll",
    "libvapoursynth-script-0.dll",
    "libvapoursynth.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll"
)

foreach ($dll in $dlls) {
    $src = Join-Path $Msys2Bin $dll
    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination $PortablePath -Force
    } else {
        Write-Warning "Missing optional runtime DLL: $src"
    }
}

$objdump = Join-Path $Msys2Bin "objdump.exe"
if (Test-Path $objdump) {
    $copied = New-Object 'System.Collections.Generic.HashSet[string]'
    Get-ChildItem -Path $PortablePath -Filter *.dll | ForEach-Object { [void]$copied.Add($_.Name.ToLowerInvariant()) }
    [void]$copied.Add((Split-Path $PortableExe -Leaf).ToLowerInvariant())

    $queue = New-Object 'System.Collections.Generic.Queue[string]'
    $queue.Enqueue($PortableExe)
    Get-ChildItem -Path $PortablePath -Filter *.dll | ForEach-Object { $queue.Enqueue($_.FullName) }

    while ($queue.Count -gt 0) {
        $binary = $queue.Dequeue()
        $imports = & $objdump -p $binary 2>$null |
            Select-String -Pattern 'DLL Name: (.+)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }

        foreach ($import in $imports) {
            $name = $import.ToLowerInvariant()
            if ($copied.Contains($name)) {
                continue
            }

            $src = Join-Path $Msys2Bin $import
            if (Test-Path $src) {
                $dst = Join-Path $PortablePath $import
                Copy-Item -LiteralPath $src -Destination $dst -Force
                [void]$copied.Add($name)
                $queue.Enqueue($dst)
            }
        }
    }
}

# アプリ/トレイアイコン。main.cpp が起動時に exe横の ico_light.ico を
# 最優先で参照する（無い場合はQtリソースへフォールバックするが、
# 開発ビルド(CMake POST_BUILDコピー)と同じ状態を配布物でも維持する）。
foreach ($ico in @("ico_light.ico", "ico_dark.ico")) {
    $src = Join-Path $Root ("ico/" + $ico)
    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination $PortablePath -Force
    }
}

# 2026-10-13: サンプル動画（image/samplevideo）の同梱は廃止。本体はもう参照していない。
# 以前のビルドで残った samplevideo/ があれば消しておく（インストーラに巻き込まないため）。
$OldSampleVideo = Join-Path $PortablePath "samplevideo"
if (Test-Path $OldSampleVideo) {
    Remove-Item -LiteralPath $OldSampleVideo -Recurse -Force
}

Write-Host "Portable package ready: $PortablePath"
