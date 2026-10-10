$IdfArguments=@($args)
$ErrorActionPreference='Stop'
$env:PYTHONUTF8='1'
$idfActivation='C:\Espressif\tools\Microsoft.v5.4.4.PowerShell_profile.ps1'
if (-not (Test-Path -LiteralPath $idfActivation)) { throw '未找到 ESP-IDF 5.4.4 激活脚本，请按本机安装位置调整 idf544.ps1。' }
. $idfActivation
$installedIdf=$env:IDF_PATH
$idfLocalDirectory=Join-Path $PSScriptRoot '.idf-env'
$idfLocalPath=Join-Path $idfLocalDirectory 'esp-idf'
New-Item -ItemType Directory -Path $idfLocalDirectory -Force | Out-Null
if (-not (Test-Path -LiteralPath $idfLocalPath)) {
    # 原安装路径含空格。目录联接仅提供无空格入口，不复制或移动 SDK。
    New-Item -ItemType Junction -Path $idfLocalPath -Target $installedIdf | Out-Null
}
$env:IDF_PATH=$idfLocalPath
$idfVersionConfiguration=Get-Content -LiteralPath (Join-Path $idfLocalPath 'tools/cmake/version.cmake') -Raw
if ($idfVersionConfiguration -notmatch 'set\(IDF_VERSION_MAJOR 5\)' -or $idfVersionConfiguration -notmatch 'set\(IDF_VERSION_MINOR 4\)' -or $idfVersionConfiguration -notmatch 'set\(IDF_VERSION_PATCH 4\)') {
    throw '当前目录联接不是 ESP-IDF 5.4.4，请核对 SDK 安装路径。'
}
$idfPython=Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
if (-not $IdfArguments) { $IdfArguments=@('build') }
Push-Location $PSScriptRoot
try {
    # 新目录避免复用旧 SDK 的 CMake 缓存；沿用 sdkconfig 中的设备与联网参数。
    & $idfPython (Join-Path $env:IDF_PATH 'tools/idf.py') -B build-v5.4.4 @IdfArguments
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF 命令失败，请查看上述日志。' }
} finally { Pop-Location }
