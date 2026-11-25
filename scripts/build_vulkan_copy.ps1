param(
    [string]$Ndk = (Join-Path $PSScriptRoot '../.velox/sdk-toolchain/ndk/30.0.16248370'),
    [string]$OutputDir,
    [ValidateSet('copy', 'compute', 'upload', 'syncbench', 'ray_query', 'ray_bench', 'render', 'maintenance5', 'cooperative_matrix', 'game_demo', 'mem_paths')]
    [string]$Workload = 'copy'
)
$ErrorActionPreference = 'Stop'
$ndkRoot = (Resolve-Path -LiteralPath $Ndk).Path
$toolchain = Join-Path $ndkRoot 'toolchains/llvm/prebuilt/windows-x86_64'
$clang = Join-Path $toolchain 'bin/clang.exe'
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "../benchmarks/vulkan/$Workload.c")).Path
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot "../.velox/vulkan-$Workload" }
$output = (New-Item -ItemType Directory -Force -Path $OutputDir).FullName
$headers = (New-Item -ItemType Directory -Force -Path (Join-Path $output 'headers')).FullName
Copy-Item -Recurse -Force -LiteralPath (Join-Path $toolchain 'sysroot/usr/include/vulkan') -Destination $headers
$video = Join-Path $toolchain 'sysroot/usr/include/vk_video'
if (Test-Path -LiteralPath $video) {
    Copy-Item -Recurse -Force -LiteralPath $video -Destination $headers
}

$glslc = Join-Path $ndkRoot 'shader-tools/windows-x86_64/glslc.exe'
$shaders = @{ 'compute' = @('compute'); 'syncbench' = @('syncbench_ubo', 'syncbench_busy'); 'ray_query' = @('ray_query');
              'ray_bench' = @('ray_bench', 'ray_pipeline.rgen', 'ray_pipeline.rmiss', 'ray_shadow.rmiss', 'ray_pipeline.rchit');
              'render' = @('render.vert', 'render.frag');
              'game_demo' = @('game_demo', 'game_demo.vert', 'game_demo.frag') }
if ($shaders.ContainsKey($Workload)) {
    foreach ($name in $shaders[$Workload]) {
        # 拡張子のない名前はコンピュートシェーダー（name.comp → name.spv.h）、それ以外は name_stage.spv.h
        $file = if ($name.Contains('.')) { $name } else { "$name.comp" }
        $header = if ($name.Contains('.')) { $name.Replace('.', '_') + '.spv.h' } else { "$name.spv.h" }
        $shader = Join-Path (Split-Path $source) $file
        & $glslc --target-env=vulkan1.2 -O -mfmt=c $shader -o (Join-Path $output $header)
        if ($LASTEXITCODE) { throw "コンピュートシェーダー $name のビルドに失敗しました ($LASTEXITCODE)" }
    }
}
if ($Workload -eq 'cooperative_matrix') {
    $glslang = Join-Path $PSScriptRoot '../third_party/aosp/emu-main-win/prebuilts/android-emulator-build/common/vulkan/windows-x86_64/glslangValidator.exe'
    $shader = [IO.Path]::ChangeExtension($source, '.comp')
    foreach ($format in 'fp16', 'fp16_accum', 'int8') {
        $defines = @()
        if ($format -eq 'int8') { $defines += '-DVELOX_INT8=1' }
        if ($format -eq 'fp16_accum') { $defines += '-DVELOX_FP16_ACCUM=1' }
        & $glslang -V --target-env vulkan1.2 @defines --vn "cooperative_matrix_$format" $shader -o (Join-Path $output "cooperative_matrix_$format.spv.h")
        if ($LASTEXITCODE) { throw "行列演算シェーダー $format のビルドに失敗しました ($LASTEXITCODE)" }
    }
}
& $clang --target=x86_64-linux-android28 -std=c17 -O2 -Wall -Wextra -Werror "-I$output" $source -ldl -lm -o (Join-Path $output "vk-$Workload-android")
if ($LASTEXITCODE) { throw "Android ビルドに失敗しました ($LASTEXITCODE)" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio の x64 C++ ツールが見つかりません' }
$vsDevCmd = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
$binary = Join-Path $output "vk-$Workload.exe"
$object = Join-Path $output "vk-$Workload.obj"
# VsDevCmd.bat は内部で vswhere.exe を呼ぶので、そのフォルダを PATH に入れておく
$command = 'set "PATH={6};%PATH%" && call "{0}" -arch=amd64 -host_arch=amd64 && cl /nologo /utf-8 /std:c11 /O2 /W4 /WX /I"{1}" /I"{2}" "{3}" /Fo"{4}" /Fe"{5}"' -f $vsDevCmd, $headers, $output, $source, $object, $binary, (Split-Path $vswhere)
cmd.exe /d /c $command
if ($LASTEXITCODE) { throw "Windows ビルドに失敗しました ($LASTEXITCODE)" }
Get-ChildItem -LiteralPath $output -File | Where-Object Name -Like "vk-$Workload*"
