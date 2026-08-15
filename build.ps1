<#
.SYNOPSIS
    Build SleepBlockTool (Win32 GUI app) with the Visual Studio C++ toolchain.

.DESCRIPTION
    Designed to run from the "Visual Studio 2026 Developer PowerShell".
    If cl.exe is not on PATH (e.g. plain PowerShell), it auto-detects the latest
    Visual Studio installation via vswhere and enters the dev shell with
    Enter-VsDevShell.

    Steps:
      1. Remove stale intermediate files from previous builds (.obj/.res/.pdb).
      2. Compile resources with rc.exe  -> SleepBlockTool.res  (embeds icon + app.manifest).
      3. Compile and link with cl.exe  -> SleepBlockTool.exe (Win32 GUI, no console).
      4. Clean up intermediates again (unless -KeepIntermediates).

    The manifest is embedded through the .rc (1 24 "app.manifest"), so the exe
    already requests admin rights / DPI awareness - no mt.exe step needed.

.PARAMETER Arch
    Target architecture: x64 (default), x86 or arm64.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Clean
    Delete all build outputs (exe/obj/res/pdb) and exit without building.

.PARAMETER KeepIntermediates
    Keep .obj/.res (and .pdb in Debug) after a successful build.

.EXAMPLE
    .\build.ps1                      # Release x64, auto clean intermediates
    .\build.ps1 -Arch x86            # 32-bit build
    .\build.ps1 -Configuration Debug -KeepIntermediates
    .\build.ps1 -Clean               # remove all artifacts
#>
[CmdletBinding()]
param(
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Arch = "x64",

    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",

    [switch]$Clean,
    [switch]$KeepIntermediates
)

$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$SrcName   = "SleepBlockTool"
$SrcCpp    = Join-Path $ScriptDir "$SrcName.cpp"
$SrcRc     = Join-Path $ScriptDir "$SrcName.rc"
$Icon      = Join-Path $ScriptDir "icon.ico"
$Manifest  = Join-Path $ScriptDir "app.manifest"
$Exe       = Join-Path $ScriptDir "$SrcName.exe"
$Obj       = Join-Path $ScriptDir "$SrcName.obj"
$Res       = Join-Path $ScriptDir "$SrcName.res"
$Pdb       = Join-Path $ScriptDir "$SrcName.pdb"

function Remove-ItemIfExists {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force
        Write-Host "  removed : $Path"
    }
}

function Assert-Present {
    param([string]$Path, [string]$What)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing ${What}: ${Path}"
    }
}

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
Assert-Present $SrcCpp "source file"
Assert-Present $SrcRc  "resource script"
Assert-Present $Manifest "manifest"
if (-not (Test-Path -LiteralPath $Icon)) {
    Write-Warning "icon.ico not found next to the .rc - the icon resource will fail to compile: $Icon"
}

Write-Host ""
Write-Host "Visual Studio 2026 Developer PowerShell - Build script"
Write-Host ("Architecture : {0}"  -f $Arch)
Write-Host ("Configuration: {0}"  -f $Configuration)
Write-Host ("Output       : {0}"  -f $Exe)
Write-Host ("----------------------------------------")

# ---------------------------------------------------------------------------
# 1. Clean mode
# ---------------------------------------------------------------------------
if ($Clean) {
    Write-Host "Cleaning build artifacts..."
    foreach ($f in @($Exe, $Obj, $Res, $Pdb)) { Remove-ItemIfExists $f }
    Write-Host "Clean finished."
    exit 0
}

# ---------------------------------------------------------------------------
# 2. Enter the Visual Studio developer environment if needed
# ---------------------------------------------------------------------------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "cl.exe not found on PATH - entering the Visual Studio Developer shell..."
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found. Please run this script from the Visual Studio Developer PowerShell."
    }
    $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsInstallPath) {
        throw "No Visual Studio installation with the C++ toolset was found."
    }
    $devShellDll = Join-Path $vsInstallPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Assert-Present $devShellDll "Developer PowerShell module"
    Import-Module $devShellDll
    Enter-VsDevShell -VsInstallPath $vsInstallPath -SkipAutomaticLocation `
                     -DevCmdArguments "-arch=$Arch"
}

# ---------------------------------------------------------------------------
# 3. Remove stale intermediates from any previous build
# ---------------------------------------------------------------------------
Write-Host "Cleaning stale intermediate files..."
foreach ($f in @($Obj, $Res, $Pdb)) { Remove-ItemIfExists $f }

# ---------------------------------------------------------------------------
# 4. Compile resources (icon + embedded manifest)
# ---------------------------------------------------------------------------
Write-Host "==> rc.exe $SrcRc"
& rc.exe /nologo /fo "$Res" "$SrcRc"
if ($LASTEXITCODE -ne 0) {
    Write-Host "rc.exe failed with exit code $LASTEXITCODE"
    if (-not $KeepIntermediates) { Remove-ItemIfExists $Res }
    exit $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# 5. Compile & link
# ---------------------------------------------------------------------------
$clArgs = @("/nologo", "/EHsc", "/W3")
if ($Configuration -eq "Release") {
    $clArgs += @("/O2", "/DNDEBUG")
} else {
    $clArgs += @("/Od", "/Zi", "/MDd", "/D_DEBUG")
}
$clArgs += @("/DUNICODE", "/D_UNICODE", "/Fo:$Obj", "/Fe:$Exe", $SrcCpp, $Res,
             "/link", "/subsystem:windows",
             "kernel32.lib", "user32.lib", "gdi32.lib", "shell32.lib",
             "advapi32.lib", "comctl32.lib")

Write-Host "==> cl.exe $SrcCpp"
& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "cl.exe failed with exit code $LASTEXITCODE"
    if (-not $KeepIntermediates) {
        Write-Host "Cleaning intermediates after failed build..."
        foreach ($f in @($Obj, $Res)) { Remove-ItemIfExists $f }
    }
    exit $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# 6. Success + clean intermediates
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Build OK: $Exe"

if (-not $KeepIntermediates) {
    Write-Host "Cleaning intermediate files..."
    foreach ($f in @($Obj, $Res)) { Remove-ItemIfExists $f }
    if ($Configuration -eq "Release") { Remove-ItemIfExists $Pdb }
    Write-Host "Done."
} else {
    Write-Host "Keeping intermediates (-KeepIntermediates)."
}
