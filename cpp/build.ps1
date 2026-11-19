param(
    [string]$Toolchain = "",
    [switch]$NoStrip
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $build, $dist | Out-Null

function Resolve-Tool([string]$name, [string]$explicit) {
    if ($explicit) {
        if (Test-Path $explicit -PathType Leaf) { return (Resolve-Path $explicit).Path }
        $cand = Join-Path $explicit "bin\$name.exe"
        if (Test-Path $cand) { return (Resolve-Path $cand).Path }
        throw "找不到 $name：$explicit"
    }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $fallback = Join-Path $root "..\_tools\w64devkit\w64devkit\bin\$name.exe"
    if (Test-Path $fallback) { return (Resolve-Path $fallback).Path }
    throw "找不到 $name.exe。请安装 MinGW-w64（g++ 13+），或用 -Toolchain 指定工具链目录。"
}

$gxx = Resolve-Tool 'g++' $Toolchain
$windres = Resolve-Tool 'windres' $Toolchain
$src = Join-Path $root 'src'
$third = Join-Path $root 'third_party'

Write-Host "g++     : $gxx"
Write-Host "windres : $windres"

& $windres -I $root (Join-Path $root 'app.rc') -O coff -o (Join-Path $build 'app_res.o')
if ($LASTEXITCODE -ne 0) { throw 'windres 编译资源失败' }

$common = @(
    '-std=c++17', '-O2', '-pthread',
    '-finput-charset=UTF-8',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    "-I$src", "-I$third"
)
$static = @('-static', '-static-libgcc', '-static-libstdc++')
$strip = @()
if (-not $NoStrip) { $strip = @('-s') }

Write-Host '编译 GUI 版本 ...'
& $gxx @common '-municode' '-mwindows' @static @strip `
    (Join-Path $src 'main_gui.cpp') (Join-Path $src 'gui.cpp') (Join-Path $src 'core.cpp') `
    (Join-Path $build 'app_res.o') `
    '-o' (Join-Path $dist 'VR180SBS.exe') `
    '-lcomctl32' '-lole32' '-luuid' '-lshell32' '-ladvapi32' '-luser32' '-lgdi32'
if ($LASTEXITCODE -ne 0) { throw 'GUI 版本编译失败' }

Write-Host '编译 CLI 版本 ...'
& $gxx @common '-municode' @static @strip `
    (Join-Path $src 'main_cli.cpp') (Join-Path $src 'core.cpp') `
    '-o' (Join-Path $dist 'VR180SBS_cli.exe') `
    '-ladvapi32' '-luser32' '-lgdi32'
if ($LASTEXITCODE -ne 0) { throw 'CLI 版本编译失败' }

# 工作区文件夹可能带“低完整性（Low）”标签，exe 会继承它；低完整性进程
# 无法写入普通文件夹（例如 D:\samples\...），表现为“无法创建输出文件”。
# 这里把产物显式标回 Medium（普通程序级别）。失败不影响构建。
foreach ($exe in @('VR180SBS.exe', 'VR180SBS_cli.exe')) {
    $path = Join-Path $dist $exe
    try {
        & icacls $path /setintegritylevel Medium *> $null
        if ($LASTEXITCODE -ne 0) { Write-Warning "无法把 $exe 标为 Medium 完整性" }
    } catch {
        Write-Warning "无法把 $exe 标为 Medium 完整性：$($_.Exception.Message)"
    }
}

Write-Host ''
Write-Host '构建完成：'
Get-ChildItem $dist -Filter '*.exe' | ForEach-Object {
    Write-Host ("  {0}  ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB))
}
