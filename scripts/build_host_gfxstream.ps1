param(
    # CMake/Ninja のビルドディレクトリ（BUILD_FROM_SOURCE.md の短いパス）
    [string]$BuildDir = '.velox\build\obj',
    # 起動用ランタイムの複製元と出力先。出力先の lib64 に新しい libgfxstream_backend.dll を置く
    [string]$BaseRuntime = '',
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Targets = 'libgfxstream_backend.dll'
)
# ホスト側 gfxstream（libgfxstream_backend.dll）だけを増分ビルドし、既存のランタイムを複製して差し替える。
# QEMU 本体を変えた場合は -Targets に qemu-system-x86_64-headless なども渡す。
$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $scriptDir '..')).Path
if (-not $BaseRuntime) { $BaseRuntime = Join-Path $root '.velox/emulator-builds/whpx20-short-release/runtime-velox' }
$ninja = Join-Path $root 'third_party/aosp/emu-main-win/prebuilts/ninja/windows-x86/ninja.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio の x64 C++ ツールが見つかりません' }
$vsDevCmd = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
$logDir = Join-Path $root '.velox/emulator-builds/whpx20-short-release'
$log = Join-Path $logDir ("build-host-$Tag-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
# VsDevCmd.bat は内部で vswhere を探して標準エラーに警告を出すため、出力は cmd 側でログへ送る
$cmd = '(call "{0}" -arch=amd64 -host_arch=amd64 && "{1}" -C "{2}" {3}) > "{4}" 2>&1' -f $vsDevCmd, $ninja, $BuildDir, $Targets, $log
cmd.exe /d /c $cmd
if ($LASTEXITCODE) { Get-Content $log -Tail 40; throw "ホストのビルドに失敗しました ($LASTEXITCODE)。ログ: $log" }

$out = Join-Path $logDir "runtime-$Tag"
if (-not (Test-Path $out)) {
    Copy-Item -Recurse -LiteralPath (Resolve-Path $BaseRuntime) -Destination $out
}
$dll = Join-Path $BuildDir 'libgfxstream_backend.dll'
Copy-Item -Force -LiteralPath $dll -Destination (Join-Path $out 'lib64/libgfxstream_backend.dll')
$pdb = Join-Path $BuildDir 'build/debug_info/libgfxstream_backend.pdb'
if (-not (Test-Path -LiteralPath $pdb)) {
    $pdb = Join-Path $BuildDir 'libgfxstream_backend.pdb'
}
if (Test-Path $pdb) { Copy-Item -Force -LiteralPath $pdb -Destination (Join-Path $out 'lib64/libgfxstream_backend.pdb') }
$targetNames = $Targets -split '\s+'
foreach ($exe in 'qemu-system-x86_64.exe', 'qemu-system-x86_64-headless.exe') {
    $built = Join-Path $BuildDir "qemu/windows-x86_64/$exe"
    if ($exe.Replace('.exe', '') -in $targetNames -or "qemu/windows-x86_64/$exe" -in $targetNames) {
        $engineDir = Join-Path $out 'qemu/windows-x86_64'
        Copy-Item -Force -LiteralPath $built -Destination $engineDir
        $pdb = Join-Path $BuildDir ("build/debug_info/" + [IO.Path]::ChangeExtension($exe, '.pdb'))
        if (Test-Path -LiteralPath $pdb) {
            Copy-Item -Force -LiteralPath $pdb -Destination $engineDir
        }
    }
}
Get-FileHash -Algorithm SHA256 (Join-Path $out 'lib64/libgfxstream_backend.dll') | Format-List
"ランタイム: $out"
"ログ: $log"
