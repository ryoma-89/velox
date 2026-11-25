param(
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$Jdk = 'C:/Program Files/Android/Android Studio/jbr',
    [string]$OutputDir = (Join-Path $PSScriptRoot '../.velox/display-bench')
)
$ErrorActionPreference = 'Stop'
if (!$Sdk) { throw 'ANDROID_HOME または -Sdk で Android SDK を指定してください' }
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../benchmarks/display')).Path
$out = (New-Item -ItemType Directory -Force -Path $OutputDir).FullName
$classes = (New-Item -ItemType Directory -Force -Path "$out/classes").FullName
$android = Join-Path $Sdk 'platforms/android-36/android.jar'
$build = Join-Path $Sdk 'build-tools/36.1.0'
$env:JAVA_HOME = (Resolve-Path -LiteralPath $Jdk).Path
& "$Jdk/bin/javac.exe" --release 8 -classpath $android -d $classes "$source/DisplayActivity.java"
if ($LASTEXITCODE) { throw 'Java コンパイル失敗' }
$inputs = @(Get-ChildItem -LiteralPath "$classes/org/velox/displaybench" -Filter '*.class' | ForEach-Object FullName)
& "$build/d8.bat" --release --min-api 30 --lib $android --output $out @inputs
if ($LASTEXITCODE) { throw 'D8 失敗' }
& "$build/aapt2.exe" link -I $android --manifest "$source/AndroidManifest.xml" -o "$out/display-unsigned.apk"
if ($LASTEXITCODE) { throw 'aapt2 失敗' }
python -c "import sys,zipfile; out=sys.argv[1]; z=zipfile.ZipFile(out+'/display-unsigned.apk','a',zipfile.ZIP_DEFLATED); z.write(out+'/classes.dex','classes.dex'); z.close()" $out
if ($LASTEXITCODE) { throw 'DEX 格納失敗' }
& "$build/zipalign.exe" -f 4 "$out/display-unsigned.apk" "$out/display-aligned.apk"
if ($LASTEXITCODE) { throw 'zipalign 失敗' }
if (!(Test-Path -LiteralPath "$out/bench-debug.jks")) {
    & "$Jdk/bin/keytool.exe" -genkeypair -keystore "$out/bench-debug.jks" -storetype JKS `
        -storepass android -keypass android -alias bench -keyalg RSA -dname 'CN=Velox Local Benchmark'
    if ($LASTEXITCODE) { throw 'ローカル試験用キーの生成に失敗' }
}
& "$build/apksigner.bat" sign --ks "$out/bench-debug.jks" --ks-key-alias bench --ks-pass pass:android `
    --key-pass pass:android --out "$out/velox-display-bench.apk" "$out/display-aligned.apk"
if ($LASTEXITCODE) { throw 'APK 署名失敗' }
& "$build/apksigner.bat" verify "$out/velox-display-bench.apk"
if ($LASTEXITCODE) { throw 'APK 署名検証失敗' }
Get-FileHash -LiteralPath "$out/velox-display-bench.apk"
