# build.ps1 - MiPower 插件构建脚本（PowerShell 版，避免 bat 编码/换行问题）
param(
    [ValidateSet('release','debug')]
    [string]$Config = 'release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $root 'src'
$out  = Join-Path $root 'build'

# ---- 定位 MSVC ----
$vsRoot = $null
foreach ($c in @(
    "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise")) {
    if (Test-Path $c) { $vsRoot = $c; break }
}
if (-not $vsRoot) { Write-Host "[ERROR] 未找到 Visual Studio" -ForegroundColor Red; exit 1 }

$msvcVer = (Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory | Select-Object -First 1).Name
$cl      = Join-Path $vsRoot "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64\cl.exe"
$link    = Join-Path $vsRoot "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64\link.exe"

$sdkRoot = "$env:ProgramFiles(x86)\Windows Kits\10"
$sdkVer  = (Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Select-Object -Last 1).Name
$sdkInc  = Join-Path $sdkRoot "Include\$sdkVer"
$sdkLib  = Join-Path $sdkRoot "Lib\$sdkVer"

Write-Host "[INFO] MSVC  $msvcVer"
Write-Host "[INFO] SDK   $sdkVer"
Write-Host "[INFO] cl    $cl"

if (-not (Test-Path $cl))    { Write-Host "[ERROR] cl.exe 不存在" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $sdkInc)) { Write-Host "[ERROR] SDK 头文件不存在" -ForegroundColor Red; exit 1 }

New-Item -ItemType Directory -Force -Path $out | Out-Null

$env:PATH = "$(Split-Path $cl);$env:PATH"

$commonFlags = @(
    '/nologo','/std:c++17','/W3','/EHsc','/utf-8',
    '/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS'
)
if ($Config -eq 'debug') {
    $commonFlags += @('/Od','/Zi','/MTd','/D_DEBUG')
} else {
    $commonFlags += @('/O2','/MT','/DNDEBUG')
}

$envvars = @(
    "INCLUDE=$((Join-Path $vsRoot 'VC\Tools\MSVC\' + $msvcVer + '\include'));$sdkInc\ucrt;$sdkInc\shared;$sdkInc\um;$sdkInc\winrt",
    "LIB=$((Join-Path $vsRoot 'VC\Tools\MSVC\' + $msvcVer + '\lib\x64'));$sdkLib\ucrt\x64;$sdkLib\um\x64"
)

function Invoke-Cl {
    param([string[]]$Args)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'cmd.exe'
    $psi.Arguments = '/c ' + ($Args -join ' ') + ' 2>&1'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $root
    foreach ($e in $envvars) {
        $kv = $e.Split('=', 2)
        [Environment]::SetEnvironmentVariable($kv[0], $kv[1], 'Process')
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return @{ Code = $p.ExitCode; Out = $stdout + $stderr }
}

# ---- 编译 selftest ----
Write-Host "`n[INFO] 编译 selftest.exe ..." -ForegroundColor Cyan
$r = Invoke-Cl (@('cl') + $commonFlags + @(
    "/I`"$src`"",
    "`"$src\selftest.cpp`"",
    "`"$src\Crypto.cpp`"",
    "/Fe:`"$out\selftest.exe`""
))
Write-Host $r.Out
if ($r.Code -ne 0) { Write-Host "[ERROR] selftest 编译失败" -ForegroundColor Red; exit 1 }

# ---- 编译插件 DLL ----
Write-Host "[INFO] 编译 MiPower.dll ..." -ForegroundColor Cyan
$sources = @('Crypto.cpp','MiioClient.cpp','MiotSpec.cpp','Config.cpp',
             'Aggregator.cpp','Collector.cpp','MiPowerPlugin.cpp','OptionsDlg.cpp')
$srcArgs = $sources | ForEach-Object { "`"$src\$_`"" }

$r = Invoke-Cl (@('cl') + $commonFlags + @(
    '/LD',
    "/I`"$src`""
) + $srcArgs + @(
    "/Fe:`"$out\MiPower.dll`"",
    "/link", '/nologo',
    'ws2_32.lib', 'comctl32.lib', 'shell32.lib', 'user32.lib', 'gdi32.lib', 'advapi32.lib'
))
Write-Host $r.Out
if ($r.Code -ne 0) { Write-Host "[ERROR] MiPower.dll 编译失败" -ForegroundColor Red; exit 1 }

Write-Host "`n[OK] 构建完成" -ForegroundColor Green
Get-ChildItem $out -Filter '*.dll' | ForEach-Object { Write-Host "     $($_.Name)  $([math]::Round($_.Length/1KB,1)) KB" }
Get-ChildItem $out -Filter '*.exe' | ForEach-Object { Write-Host "     $($_.Name)  $([math]::Round($_.Length/1KB,1)) KB" }
exit 0
