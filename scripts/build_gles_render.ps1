param(
    [string]$Ndk = (Join-Path $PSScriptRoot '../.velox/sdk-toolchain/ndk/30.0.16248370'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '../.velox/gles-render')
)
$ErrorActionPreference = 'Stop'
$toolchain = Join-Path (Resolve-Path -LiteralPath $Ndk) 'toolchains/llvm/prebuilt/windows-x86_64'
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../benchmarks/gles/render.c')).Path
$output = (New-Item -ItemType Directory -Force -Path $OutputDir).FullName
$headers = (New-Item -ItemType Directory -Force -Path (Join-Path $output 'headers')).FullName
foreach ($folder in 'EGL', 'GLES3', 'KHR') {
    Copy-Item -Recurse -Force -LiteralPath (Join-Path $toolchain "sysroot/usr/include/$folder") -Destination $headers
}

& (Join-Path $toolchain 'bin/clang.exe') --target=x86_64-linux-android28 -std=c17 -O2 -Wall -Wextra -Werror `
    $source -lEGL -lGLESv3 -o (Join-Path $output 'gles-render-android')
if ($LASTEXITCODE) { throw "Android GLES ビルドに失敗しました ($LASTEXITCODE)" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio の x64 C++ ツールが見つかりません' }
$vsDevCmd = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
$command = 'set "PATH={0};%PATH%" && call "{1}" -arch=amd64 -host_arch=amd64 && cl /nologo /utf-8 /std:c11 /O2 /W4 /WX /I"{2}" "{3}" /Fo"{4}\gles-render.obj" /Fe"{4}\gles-render.exe"' -f (Split-Path $vswhere), $vsDevCmd, $headers, $source, $output
cmd.exe /d /c $command
if ($LASTEXITCODE) { throw "Windows GLES ビルドに失敗しました ($LASTEXITCODE)" }
