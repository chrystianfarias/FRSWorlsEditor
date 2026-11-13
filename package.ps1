# Builds FRS World Editor and packages it for distribution in dist\:
#
#   dist\FRSWorldEditor-<version>\       what a player unzips anywhere
#       FRSWorldEditor.exe               the editor; its setup installs the game side
#       game\dinput8.dll                 Ultimate ASI Loader (ThirteenAG, MIT)
#       game\scripts\FRSWorldLoader.asi  the editor's end in the game
#       game\scripts\FRSFastBoot.asi     Launch Game straight into free roam
#       README.txt, LICENSE.txt
#   dist\FRSWorldEditor-<version>.zip    the same, zipped
#
#   .\package.ps1
#   .\package.ps1 -Version 0.2.0
#   .\package.ps1 -FastBoot D:\path\to\FRSFastBoot   # its repository, built here
#   .\package.ps1 -WithExtensions                    # a private build, see README
#
# The release is built in folders of its own (build-release, loader\build-release)
# and without extensions, whatever sits in .\private: what ships is this
# repository and nothing else. -WithExtensions builds them in instead and
# names the package -private, for your own use.
#
# FRSFastBoot lives in its own repository (by default the folder beside this
# one) and is built from there. Ultimate ASI Loader is downloaded once, at the
# version pinned below, into third_party\asi-loader.

param(
    [string]$Version = "0.1.0",
    [switch]$WithExtensions,
    [string]$FastBoot = (Join-Path (Split-Path $PSScriptRoot -Parent) "fastloader"),
    [string]$AsiLoaderVersion = "v9.7.4"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$dist = Join-Path $root "dist"
$name = "FRSWorldEditor-$Version" + $(if ($WithExtensions) { "-private" } else { "" })
$extensions = if ($WithExtensions) { "ON" } else { "OFF" }
$stage = Join-Path $dist $name
$asiLoader = Join-Path $root "third_party\asi-loader"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

# `$options`: extra -D settings, for this repository's own projects only (a
# setting a project does not know makes CMake warn, which stops the script).
function Build([string]$source, [string]$binary, [string]$arch, [string[]]$options = @()) {
    & $cmake -S $source -B $binary -G "Visual Studio 17 2022" -A $arch @options | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $source" }
    & $cmake --build $binary --config Release | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed for $source" }
}

# ---- build: the editor is x64, the game and its plugins are 32-bit
"[..] building the editor"
Build $root (Join-Path $root "build-release") "x64" @("-DFRS_WITH_EXTENSIONS=$extensions")
"[..] building FRS World Loader"
Build (Join-Path $root "loader") (Join-Path $root "loader\build-release") "Win32" @("-DFRS_WITH_EXTENSIONS=$extensions")
if (-not (Test-Path (Join-Path $FastBoot "CMakeLists.txt"))) {
    throw "FRSFastBoot's repository is not at $FastBoot (pass -FastBoot <folder>)"
}
"[..] building FRSFastBoot from $FastBoot"
Build $FastBoot (Join-Path $FastBoot "build") "Win32"

# ---- the ASI loader, once
$stamp = Join-Path $asiLoader ".version"
$have = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }
if ($have -ne $AsiLoaderVersion -or -not (Test-Path (Join-Path $asiLoader "dinput8.dll"))) {
    "[..] downloading Ultimate ASI Loader $AsiLoaderVersion (x86)"
    $archive = Join-Path $env:TEMP "frsworldeditor-asiloader.zip"
    Invoke-WebRequest -UseBasicParsing -OutFile $archive `
        -Uri "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/$AsiLoaderVersion/Ultimate-ASI-Loader-NoPDB.zip"
    if (Test-Path $asiLoader) { Remove-Item $asiLoader -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $asiLoader | Out-Null
    Expand-Archive -Path $archive -DestinationPath $asiLoader -Force
    if (-not (Test-Path (Join-Path $asiLoader "dinput8.dll"))) { throw "dinput8.dll not in the ASI loader archive" }
    Invoke-WebRequest -UseBasicParsing -OutFile (Join-Path $asiLoader "LICENSE.txt") `
        -Uri "https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/master/license"
    Set-Content $stamp $AsiLoaderVersion
}

# ---- stage
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$scripts = Join-Path $stage "game\scripts"
New-Item -ItemType Directory -Force -Path $scripts | Out-Null

Copy-Item (Join-Path $root "build-release\bin\Release\FRSWorldEditor.exe") $stage
Copy-Item (Join-Path $root "packaging\README.txt") $stage
Copy-Item (Join-Path $asiLoader "dinput8.dll") (Join-Path $stage "game")
Copy-Item (Join-Path $root "loader\build-release\Release\FRSWorldLoader.asi") $scripts
Copy-Item (Join-Path $root "loader\FRSWorldLoader.ini") $scripts
Copy-Item (Join-Path $FastBoot "build\Release\FRSFastBoot.asi") $scripts
Copy-Item (Join-Path $FastBoot "dist\FRSFastBoot.ini") $scripts
Copy-Item (Join-Path $FastBoot "dist\FRSFastBoot-Tracks.txt") $scripts

$rule = "=" * 76
$license = @(
    "FRS World Editor $Version",
    "",
    "This package holds works under three licenses.",
    "",
    $rule,
    "1. FRS World Editor (FRSWorldEditor.exe), FRS World Loader",
    "   (game\scripts\FRSWorldLoader.asi) and FRSFastBoot (game\scripts\FRSFastBoot.asi),",
    "   by Chrystian Farias -- https://buymeacoffee.com/chrystianfarias",
    "   Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)",
    "   You may share and adapt them, crediting Chrystian Farias, for non-commercial",
    "   purposes only: they may not be sold.",
    $rule,
    "",
    (Get-Content (Join-Path $root "LICENSE") -Raw).TrimEnd(),
    "",
    $rule,
    "2. Win32 property grid, by David MacDermot, compiled into FRSWorldEditor.exe",
    "   The Code Project Open License (CPOL) 1.02",
    $rule,
    "",
    (Get-Content (Join-Path $root "third_party\propertygrid\LICENSE") -Raw).TrimEnd(),
    "",
    $rule,
    "3. Ultimate ASI Loader (game\dinput8.dll), by ThirteenAG -- MIT",
    "   https://github.com/ThirteenAG/Ultimate-ASI-Loader",
    $rule,
    "",
    (Get-Content (Join-Path $asiLoader "LICENSE.txt") -Raw).TrimEnd()
)
($license -join "`r`n") + "`r`n" | Set-Content (Join-Path $stage "LICENSE.txt") -Encoding utf8 -NoNewline

# ---- zip
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
"[ok] $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { "     " + $_.FullName.Substring($stage.Length + 1) }
