#requires -Version 5.1
# Build all hook artifacts with the Android NDK + CMake/Ninja.
#   libpicoet_hook.so : payload (ShadowHook static + gate + plugin + eye-mode)
#   picoet-inject     : arm64 ptrace injector
#   picoet-target     : benign control target (libdl + pause)
# No PICO binary is used or produced here.
#
# Toolchain discovery order:
#   -Ndk / -CmakeBin parameters, then PICOET_NDK / PICOET_CMAKE_BIN,
#   then a standard SDK layout under ANDROID_HOME / ANDROID_SDK_ROOT.
[CmdletBinding()]
param(
    [string]$Ndk = $env:PICOET_NDK,
    [string]$CmakeBin = $env:PICOET_CMAKE_BIN,
    [string]$Api = '29',
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build' }

$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { $env:ANDROID_SDK_ROOT }
if (-not $Ndk -and $env:ANDROID_NDK_HOME) { $Ndk = $env:ANDROID_NDK_HOME }
if (-not $Ndk -and $env:ANDROID_NDK_ROOT) { $Ndk = $env:ANDROID_NDK_ROOT }
if (-not $Ndk -and $sdk) {
    $candidate = Get-ChildItem (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($candidate) { $Ndk = $candidate.FullName }
}
if (-not $CmakeBin -and $sdk) {
    $candidate = Get-ChildItem (Join-Path $sdk 'cmake') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($candidate) { $CmakeBin = Join-Path $candidate.FullName 'bin' }
}
if (-not $Ndk) { throw 'Android NDK not found. Pass -Ndk or set PICOET_NDK / ANDROID_NDK_HOME.' }
if (-not $CmakeBin) { throw 'CMake/Ninja not found. Pass -CmakeBin or set PICOET_CMAKE_BIN.' }

$cmake = Join-Path $CmakeBin 'cmake.exe'
$ninja = Join-Path $CmakeBin 'ninja.exe'
$toolchain = Join-Path $Ndk 'build\cmake\android.toolchain.cmake'
$readelf = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
$nm = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
$objdump = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe'
$strip = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
foreach ($t in @($cmake, $ninja, $toolchain, $readelf, $nm, $objdump, $strip)) {
    if (-not (Test-Path -LiteralPath $t)) { throw "missing tool: $t" }
}

Write-Host "[*] cmake configure..."
& $cmake -G Ninja -S $root -B $BuildDir `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$Api" `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$ninja"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "[*] cmake build..."
& $cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$payload = Join-Path $BuildDir 'libpicoet_hook.so'
$injector = Join-Path $BuildDir 'picoet-inject'
$ctl = Join-Path $BuildDir 'picoet-target'
# Debug sections contain build-machine paths. Keep ordinary symbols for the
# structural checks and diagnostics, but remove debug-only data before packaging.
& $strip --strip-debug $payload $injector $ctl
if ($LASTEXITCODE -ne 0) { throw 'failed to strip build-time debug information' }
Get-Item $injector, $payload, $ctl | Format-Table Name, Length -AutoSize | Out-String | Write-Host

# --- build-time shape assertions (not tests; fail the build on wrong output) ---
function Assert-AArch64([string]$f, [string]$tag) {
    $h = & $readelf -h $f | Out-String
    if ($h -notmatch 'AArch64') { throw "$tag is not AArch64" }
    if ($h -notmatch 'DYN')     { throw "$tag is not DYN/PIE" }
}
Assert-AArch64 $injector 'injector'
Assert-AArch64 $payload 'payload'
Assert-AArch64 $ctl 'control target'

$injDyn = & $readelf -d $injector | Out-String
if ($injDyn -notmatch 'libdl\.so') { throw 'injector does not link libdl.so' }

$ctlDyn = & $readelf -d $ctl | Out-String
if ($ctlDyn -notmatch 'libdl\.so') { throw 'control target does not link libdl.so' }

$payDyn = & $readelf -d $payload | Out-String
if ($payDyn -notmatch 'INIT_ARRAY') { throw 'payload has no INIT_ARRAY (constructor)' }
if ($payDyn -notmatch 'liblog\.so') { throw 'payload does not link liblog.so (ShadowHook needs it)' }

$syms = & $readelf --dyn-syms $payload | Out-String
foreach ($s in @('picoet_hook_marker', 'picoet_gate_calls', 'picoet_gate_installed',
                 'picoet_plugin_base', 'picoet_hooka_calls', 'picoet_hooka_installed',
                 'picoet_eye_mode_patched', 'picoet_eye_mode',
                 'picoet_g', 'picoet_dual_installed', 'picoet_dual_leaf_installed')) {
    if ($syms -notmatch $s) { throw "payload does not export $s" }
}
$all = & $readelf -s $payload | Out-String
if ($all -notmatch 'shadowhook_init') { throw 'payload does not contain ShadowHook (static link failed)' }
if ($all -notmatch 'infer_proxy') { throw 'payload has no Hook A proxy (P4)' }
if ($all -notmatch 'picoet_leaf_start') { throw 'payload has no Hook B leaf template (P4)' }

# --- P4 shape checks: leaf template layout + Hook A proxy frame pointer --------
$nmOut = & $nm $payload | Out-String
function Get-NmAddr([string]$name) {
    $m = [regex]::Match($nmOut, "(?m)^([0-9a-fA-F]+)\s+\S+\s+" + [regex]::Escape($name) + "\r?$")
    if (-not $m.Success) { throw "payload has no symbol $name" }
    return [Convert]::ToInt64($m.Groups[1].Value, 16)
}
$leafStart = Get-NmAddr 'picoet_leaf_start'
$leafEnd = Get-NmAddr 'picoet_leaf_end'
$leafRet = Get-NmAddr 'picoet_leaf_ret_pc'
$leafG = Get-NmAddr 'picoet_leaf_apply_slot'
$leafLen = $leafEnd - $leafStart
if ($leafLen -le 0 -or $leafLen -gt 512) { throw "leaf size $leafLen B out of range (1..512)" }
$retOff = $leafRet - $leafStart
$gOff = $leafG - $leafStart
if ($retOff -lt 0 -or $retOff + 4 -gt $leafLen) { throw "leaf return slot at $retOff out of range" }
if ($gOff -lt 0 -or $gOff + 8 -gt $leafLen -or ($gOff % 8) -ne 0) { throw "leaf G slot at $gOff out of range/misaligned" }
$proxyAddr = Get-NmAddr 'infer_proxy'
$proxyDis = & $objdump -d ("--start-address=0x{0:x}" -f $proxyAddr) ("--stop-address=0x{0:x}" -f ($proxyAddr + 0x40)) $payload | Out-String
if ($proxyDis -notmatch 'stp\s+x29, x30' -or $proxyDis -notmatch 'add\s+x29, sp') {
    throw 'infer_proxy has no frame-pointer prologue (-fno-omit-frame-pointer missing?)'
}

Write-Host '--- shape checks ---'
Write-Host '  injector : AArch64 PIE, NEEDED libdl.so                      OK'
Write-Host '  payload  : AArch64 DYN, INIT_ARRAY, ShadowHook, exports      OK'
Write-Host '  payload  : P4 leaf layout + proxy frame pointer              OK'
Write-Host '  control  : AArch64 PIE, NEEDED libdl.so                      OK'
Write-Host "[+] done: $BuildDir"
