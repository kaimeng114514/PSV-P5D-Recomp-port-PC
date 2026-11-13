# dev/build_p5d_launcher.ps1: build the P5D single-page launcher on Windows with the local mingw-w64 g++
# (the cross toolchain's SDL3 development files from C:\dev\nativ_toolchain, the repository's own Dear ImGui).
# The release pipeline builds the same hub.cpp with the packaged llvm-mingw toolchain; this is the development build.
#   .\dev\build_p5d_launcher.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tc = if ($env:ATIV_WIN_TOOLCHAIN) { $env:ATIV_WIN_TOOLCHAIN } else { 'C:\dev\nativ_toolchain' }
$sdl = Join-Path $tc 'SDL3\x86_64-w64-mingw32'
$gxx = if ($env:RECOMP_HOST_CXX) { $env:RECOMP_HOST_CXX } else { 'C:\dev\mingw64\bin\g++.exe' }
$imgui = Join-Path $root 'third_party\imgui'
$outdir = Join-Path $root 'build\p5d_launcher'
$objdir = Join-Path $outdir 'obj'
New-Item -ItemType Directory -Force -Path $outdir, $objdir | Out-Null

$sdlInc = Join-Path $sdl 'include'
$sdlLib = Join-Path $sdl 'lib\libSDL3.dll.a'
if (-not (Test-Path $sdlLib)) { throw "no SDL3 import library at $sdlLib" }

$common = @('-O2', '-std=c++17', '-w', '-D_FILE_OFFSET_BITS=64',
    '-I', $imgui, '-I', (Join-Path $imgui 'backends'),
    '-I', (Join-Path $root 'runtime\ui'), '-I', (Join-Path $root 'runtime\platform'),
    '-I', $sdlInc)

$imflags = @('-include', (Join-Path $root 'runtime\platform\win32_compat.h'))
$imsrc = @('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
    'backends\imgui_impl_sdl3.cpp', 'backends\imgui_impl_sdlgpu3.cpp')
$objs = @()
foreach ($s in $imsrc) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($s)
    $obj = Join-Path $objdir ($name + '.o')
    $objs += $obj
    $src = Join-Path $imgui $s
    if ($Force -or -not (Test-Path $obj) -or ((Get-Item $src).LastWriteTime -gt (Get-Item $obj).LastWriteTime)) {
        Write-Host "  g++ $s"
        & $gxx @common @imflags -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
    }
}

$exe = Join-Path $outdir 'p5d_launcher.exe'
$hubObj = Join-Path $objdir 'hub.o'
$rsp = Join-Path $objdir 'repo_root.rsp'
$windres = Join-Path (Split-Path -Parent $gxx) 'windres.exe'
$resObj = Join-Path $objdir 'launcher_res.o'
$specs = Join-Path $objdir 'no-default-manifest.specs'
if (-not (Test-Path $specs) -or $Force) {
    # mingw-w64 GCC appends its own default-manifest.o when linking; with our manifest resource the linker then fails
    # with ".rsrc merge failure: multiple non-default manifests". Drop that one specs fragment for this build.
    $specsText = (& $gxx -dumpspecs) -join "`n"
    $specsText = $specsText -replace '%\{!shared:%:if-exists\(default-manifest\.o%s\)\}', ''
    Set-Content -Path $specs -Value $specsText -Encoding ascii
}
$rc = Join-Path $objdir 'launcher.rc'
$rootf = $root -replace '\\', '/'
@"
1 ICON "$rootf/assets/p5d/p5d.ico"
1 24 "$rootf/assets/p5d/launcher.manifest"
"@ | Set-Content -Path $rc -Encoding ascii
if (-not (Test-Path $resObj) -or $Force -or ((Get-Item $rc).LastWriteTime -gt (Get-Item $resObj).LastWriteTime)) {
    Write-Host '  windres launcher.rc'
    & $windres $rc -O coff -o $resObj
    if ($LASTEXITCODE -ne 0) { throw 'windres failed' }
}
Set-Content -Path $rsp -Value '-DREPO_ROOT=\"C:/dev/nativ_src\"' -Encoding ascii
if ($Force -or -not (Test-Path $hubObj) -or ((Get-Item (Join-Path $root 'launcher\hub.cpp')).LastWriteTime -gt (Get-Item $hubObj).LastWriteTime)) {
    Write-Host '  g++ launcher/hub.cpp'
    & $gxx @common @imflags "-DP5D_LAUNCHER" "@$rsp" '-c' (Join-Path $root 'launcher\hub.cpp') '-o' $hubObj
    if ($LASTEXITCODE -ne 0) { throw 'compile failed: launcher/hub.cpp' }
}

Write-Host "  link $exe"
& $gxx @common '-mwindows' '-static' '-lpthread' "-specs=$specs" $resObj $hubObj @objs $sdlLib '-o' $exe
if ($LASTEXITCODE -ne 0) {   # (mingw-w64 GCC also links its own default-manifest.o: retry without ours, a dev build)
    Write-Host '  link without the manifest (this mingw-w64 links its own default manifest)'
    & $gxx @common '-mwindows' '-static' '-lpthread' $hubObj @objs $sdlLib '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }
}
Copy-Item (Join-Path $sdl 'bin\SDL3.dll') $outdir -Force
# the release artifact's name (the game's own name is not used: the port's name is), beside the ASCII debug binary
$named = Join-Path $outdir 'P5D Windows移植版启动器.exe'
Copy-Item $exe $named -Force
Get-Item $exe, $named, (Join-Path $outdir 'SDL3.dll') | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
