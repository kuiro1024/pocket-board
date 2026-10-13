param(
    # Example: -Version 1.0.0  (omit to keep the version in CMakeLists.txt)
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$InstallerDir = Join-Path $Root "installer"
$IssPath = Join-Path $InstallerDir "PocketBoard.iss"
# 2026-10-13: 配布用なので Release ビルドを使う（以前は Debug を使っていた）。
# Qt Creator の Release キットのビルドフォルダと同じ場所。
$BuildDirName = "build/Msys2_MinGW_64bit_Release"
$BuildDir = Join-Path $Root $BuildDirName
$SetupPath = Join-Path $Root "dist/installer/PocketBoardSetup.exe"

function Find-FirstExistingPath {
    param([string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

# 【重要】以前はここで PocketBoard.iss をヒアドキュメントから毎回
# 再生成していたが、PowerShell 5.1 のエンコーディング問題による日本語の
# 文字化け・カスタムタスク（透明効果ON等）の消失の原因になったため廃止。
# installer/PocketBoard.iss（UTF-8 BOM付き）が正式なソースであり、
# インストーラの変更はそのファイルを直接編集すること。
if (-not (Test-Path $IssPath)) {
    throw "Installer script not found: $IssPath (installer/PocketBoard.iss is the maintained source; it is no longer auto-generated)"
}
Write-Host "Using installer script: $IssPath"

# ---- Version ----
# CMakeLists.txt の project(PocketBoard VERSION x.y.z) が唯一の定義元。
# -Version が指定されたらそこを書き換え、exe(main.cpp) と ISS の両方へ反映する。
# PowerShell 5.1 の Get-Content/Set-Content は日本語を壊すので .NET で UTF-8(BOMなし) 読み書き。
$CMakeListsPath = Join-Path $Root "CMakeLists.txt"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$cmakeText = [System.IO.File]::ReadAllText($CMakeListsPath, $Utf8NoBom)
$versionPattern = 'project\(PocketBoard VERSION (\d+\.\d+\.\d+)'
$m = [regex]::Match($cmakeText, $versionPattern)
if (-not $m.Success) {
    throw "project(PocketBoard VERSION x.y.z) not found in CMakeLists.txt"
}
$currentVersion = $m.Groups[1].Value

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $currentVersion
} else {
    $Version = $Version.Trim().TrimStart("v", "V")
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        throw "Invalid version '$Version'. Use x.y.z (e.g. 1.0.0)."
    }
    if ($Version -ne $currentVersion) {
        $cmakeText = ([regex]$versionPattern).Replace($cmakeText, "project(PocketBoard VERSION $Version", 1)
        [System.IO.File]::WriteAllText($CMakeListsPath, $cmakeText, $Utf8NoBom)
        Write-Host "Updated CMakeLists.txt version: $currentVersion -> $Version"
    }
}
Write-Host "PocketBoard version: $Version"

$env:PATH = "C:\msys64\mingw64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"

$cmake = Find-FirstExistingPath @(
    "C:\Qt\Tools\CMake_64\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if (-not $cmake) {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $cmake = $cmd.Source
    }
}

if (-not $cmake) {
    throw "cmake.exe not found."
}

if (-not (Test-Path $BuildDir)) {
    throw "Build directory not found: $BuildDir"
}

Write-Host "Building application..."
& $cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    throw "cmake build failed with exit code $LASTEXITCODE"
}

Write-Host "Packaging portable app..."
& (Join-Path $PSScriptRoot "package-portable.ps1") -BuildDir $BuildDirName
if ($LASTEXITCODE -ne 0) {
    throw "Portable packaging failed with exit code $LASTEXITCODE"
}

$iscc = Find-FirstExistingPath @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)

if (-not $iscc) {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $iscc = $cmd.Source
    }
}

if (-not $iscc) {
    throw "ISCC.exe not found. Install Inno Setup 6."
}

if (Test-Path $SetupPath) {
    Write-Host "Removing existing setup: $SetupPath"
    Remove-Item -LiteralPath $SetupPath -Force
}

Write-Host "Compiling installer..."
& $iscc "/DMyAppVersion=$Version" $IssPath
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $SetupPath)) {
    throw "Setup was not generated: $SetupPath"
}

Write-Host "Generated setup ($Version): $SetupPath"
