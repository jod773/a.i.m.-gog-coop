# Builds the mod DLLs with llvm-mingw (i686).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cc = Join-Path $root '..\ext\llvm-mingw-20261006-msvcrt-x86_64\bin\i686-w64-mingw32-gcc.exe'
$out = Join-Path $root '..\build'
New-Item -ItemType Directory -Force $out | Out-Null

$common = @("$root\common\util.c", "$root\common\lang.c", "$root\common\display.c", "$root\common\devtools.c", "$root\common\net.c", "$root\common\crashlog.c", "$root\common\audio.c", "$root\common\gamepad.c", "$root\common\coop_core.c", "$root\common\textinput.c", "$root\common\modopts.c", "$root\common\d3d9wrap.c", "$root\common\d9light.c", "$root\common\overlay.c")
$flags = @('-O2', '-Wall', '-Wno-unused-function', '-shared', '-static', '-static-libgcc',
           '-s', '-Wl,--enable-stdcall-fixup', '-Wl,--kill-at', "-I$out")

# The version shown in the main menu: the release tag (tools/release.py sets MOD_VERSION), else a
# development build named after its commit.
$ver = $env:MOD_VERSION
if (-not $ver) {
    $hash = ''
    try { $hash = (& git -C $root rev-parse --short HEAD 2>$null) } catch {}
    $ver = if ($hash) { "dev-$hash" } else { 'dev' }
}
Set-Content -Path "$out\version.h" -Value "#define MOD_VERSION `"$ver`"" -Encoding ascii

# A.I.M. 2 Clan Wars (GOG) co-op -> dinput8.dll
$aim2 = @("$root\aim2\main.c", "$root\aim2\options.c", "$root\aim2\vfs.c", "$root\aim2\settings.c", "$root\aim2\coop.c", "$root\aim2\lobby.c", "$root\aim2\mirror.c", "$root\aim2\questlog.c", "$root\aim2\notify.c", "$root\aim2\economy.c", "$root\aim2\debugmenu.c", "$root\aim2\lights.c") + $common
New-Item -ItemType Directory -Force "$out\aim2cw" | Out-Null
& $cc @flags -Wno-null-dereference -o "$out\aim2cw\dinput8.dll" "-Wl,-Map=$out\aim2cw\dinput8.map" @aim2 "$root\aim2\dinput8.def" -lkernel32 -luser32 -lgdi32 -lws2_32 -lshlwapi
if ($LASTEXITCODE) { throw "aim2cw build failed" }
Write-Host "built $out\aim2cw\dinput8.dll"

# A.I.M. (GOG) co-op -> dinput8.dll
New-Item -ItemType Directory -Force "$out\aim1" | Out-Null
$aim1common = @("$root\common\util.c", "$root\common\display.c", "$root\common\devtools.c", "$root\common\net.c", "$root\common\crashlog.c", "$root\common\audio.c", "$root\common\gamepad.c", "$root\common\coop_core.c", "$root\common\textinput.c", "$root\common\modopts.c", "$root\common\d3d9wrap.c", "$root\common\d9light.c", "$root\common\overlay.c")
$aim1 = (Get-ChildItem "$root\aim1\*.c" | ForEach-Object { $_.FullName }) + $aim1common
& $cc @flags -o "$out\aim1\dinput8.dll" "-Wl,-Map=$out\aim1\dinput8.map" @aim1 "$root\aim2\dinput8.def" -lkernel32 -luser32 -lgdi32 -lws2_32 -lshlwapi
if ($LASTEXITCODE) { throw "aim1 build failed" }
Write-Host "built $out\aim1\dinput8.dll"
