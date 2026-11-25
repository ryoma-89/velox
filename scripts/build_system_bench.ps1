param(
    [string]$Ndk = (Join-Path $PSScriptRoot '../.velox/sdk-toolchain/ndk/30.0.16248370'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '../.velox/system-bench')
)
$ErrorActionPreference = 'Stop'
$toolchain = Join-Path (Resolve-Path -LiteralPath $Ndk) 'toolchains/llvm/prebuilt/windows-x86_64'
$output = (New-Item -ItemType Directory -Force -Path $OutputDir).FullName
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio の x64 C++ ツールが見つかりません' }
$vsDevCmd = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
foreach ($workload in 'memory', 'storage', 'cpu') {
    $source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "../benchmarks/system/$workload.c")).Path
    & (Join-Path $toolchain 'bin/clang.exe') --target=x86_64-linux-android28 -std=c17 -O2 -Wall -Wextra -Werror `
        $source -lm -o (Join-Path $output "$workload-bench-android")
    if ($LASTEXITCODE) { throw "Android $workload ビルドに失敗しました ($LASTEXITCODE)" }
    $command = 'set "PATH={0};%PATH%" && call "{1}" -arch=amd64 -host_arch=amd64 && cl /nologo /utf-8 /std:c11 /O2 /W4 /WX "{2}" /Fo"{3}\{4}-bench.obj" /Fe"{3}\{4}-bench.exe"' -f (Split-Path $vswhere), $vsDevCmd, $source, $output, $workload
    cmd.exe /d /c $command
    if ($LASTEXITCODE) { throw "Windows $workload ビルドに失敗しました ($LASTEXITCODE)" }
}
