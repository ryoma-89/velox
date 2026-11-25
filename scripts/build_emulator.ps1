param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../third_party/aosp/emu-main-win'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../.velox/emulator-builds'),
    [ValidateSet('release', 'debug')]
    [string]$Configuration = 'release',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')]
    [string]$Tag = (Get-Date -Format 'yyyyMMdd-HHmmss'),
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$qemu = Join-Path $source 'external/qemu'
$rebuild = Join-Path $qemu 'android/rebuild.cmd'
if (-not (Test-Path -LiteralPath $rebuild -PathType Leaf)) {
    throw 'AOSP ワークスペースに external/qemu/android/rebuild.cmd が必要です'
}
$destination = Join-Path $OutputRoot "$Tag-$Configuration"
if ((Test-Path -LiteralPath $destination) -and -not $Resume) {
    throw "既存の成果物を保持するため別の Tag を指定してください $destination"
}
$destination = (New-Item -ItemType Directory -Force -Path $destination).FullName
$objs = Join-Path $destination 'objs'
$suffix = if ($Resume) { '-resume-' + (Get-Date -Format 'yyyyMMdd-HHmmss') } else { '' }
$dist = Join-Path $destination "dist$suffix"
$logName = "build$suffix.log"
$log = Join-Path $destination $logName
New-Item -ItemType Directory -Force -Path $objs | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio の x64 C++ ツールが見つかりません' }
$vsDevCmd = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'

$projects = @('external/qemu', 'hardware/google/aemu', 'hardware/google/gfxstream', 'external/angle')
$revisions = [ordered]@{}
foreach ($project in $projects) {
    $projectPath = Join-Path $source $project
    if (Test-Path -LiteralPath (Join-Path $projectPath '.git')) {
        $sha = & git -C $projectPath rev-parse HEAD
        if ($LASTEXITCODE) { throw "リビジョンを取得できません $project" }
        $revisions[$project] = $sha
        $patchPath = Join-Path $destination ($project.Replace('/', '-') + '.patch')
        & git -C $projectPath --no-pager diff --binary --no-ext-diff "--output=$patchPath" HEAD
        if ($LASTEXITCODE) { throw "パッチを取得できません $project" }
    }
}
$revisions | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'revisions.json') -Encoding utf8
$lockedManifest = Join-Path $source 'manifest.lock.xml'
if (Test-Path -LiteralPath $lockedManifest -PathType Leaf) {
    Copy-Item -LiteralPath $lockedManifest -Destination $destination
}
Push-Location -LiteralPath $qemu
$previousPythonIoEncoding = $env:PYTHONIOENCODING
try {
    $env:PYTHONIOENCODING = 'utf-8'
    $command = 'call "{0}" -arch=amd64 -host_arch=amd64 && call "{1}" --target windows-x86_64 --config {2} --crash none --ccache none --task-disable clean --task-disable RemoveLeftover --cmake_option CMAKE_OBJECT_PATH_MAX=180 --out "{3}" --dist "{4}"' -f $vsDevCmd, $rebuild, $Configuration, $objs, $dist
    cmd.exe /d /c $command 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE) { throw "ソースビルドに失敗しました ($LASTEXITCODE) $log" }
} finally {
    $env:PYTHONIOENCODING = $previousPythonIoEncoding
    Pop-Location
}
Write-Output "Windows $Configuration $destination"
