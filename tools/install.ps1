# install.ps1: install rmp, the one command of raylib_multiplatform, on Windows.
#
#   irm https://omardev29.github.io/rmp-docs/install.ps1 | iex
#
# Windows PowerShell 5.1 or PowerShell 7, as yourself: no administrator. It
# clones the framework into %LOCALAPPDATA%\rmp (with git, without the file
# contents of old commits) and runs `rmp.cmd install`, which writes a .cmd
# shim into %LOCALAPPDATA%\Programs\rmp and adds that folder to your user
# PATH. Run again, it updates the clone with `rmp.cmd update` instead. It
# needs git and a Python 3.11+ and stops, saying how to get them, without
# either; it never installs anything. What a build still needs -- CMake,
# Ninja, a C++ compiler -- is listed at the end with the winget command.
#
# Under `irm | iex` this runs inside the session it was typed in. So it never
# calls exit, which would close that window: a failure is a throw. And all of
# it is one script block, so none of its variables are left behind in the
# session. The only thing it changes there is PATH, so that rmp works in the
# same window straight away.
#
# RMP_INSTALL_SOURCE and RMP_INSTALL_BRANCH clone from somewhere else: CI
# installs from its own checkout. Linux, macOS and the BSDs: install.sh.
#
# ASCII and CRLF: Windows PowerShell 5.1 reads a file without a BOM in the
# ANSI code page, and tests/rmp_test.py checks both.

& {
    $url = 'https://github.com/omardev29/raylib_multiplatform'
    $source = if ($env:RMP_INSTALL_SOURCE) { $env:RMP_INSTALL_SOURCE } else { $url }
    $branch = if ($env:RMP_INSTALL_BRANCH) { $env:RMP_INSTALL_BRANCH } else { 'main' }
    if (-not $env:LOCALAPPDATA) { throw 'rmp: LOCALAPPDATA is not set, so there is no folder to install rmp into.' }
    $dest = Join-Path $env:LOCALAPPDATA 'rmp'
    $shims = Join-Path $env:LOCALAPPDATA 'Programs\rmp'

    function Test-Tool([string] $name) {
        [bool](Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)
    }

    if (-not (Test-Tool 'git')) {
        Write-Host 'rmp needs git, and there is none on PATH. Nothing was installed.'
        Write-Host 'Install it, open a new PowerShell, and run this again:'
        Write-Host '  winget install --id Git.Git -e'
        throw 'rmp was not installed: git is missing.'
    }

    # The same names, in the same order, as rmp.ps1 and rmp.cmd, and each one
    # ASKED for 3.11: python3 can be the Microsoft Store stub, which exits 9009.
    $probe = 'import sys; sys.exit(sys.version_info < (3, 11))'
    $python = $null
    foreach ($candidate in 'py -3', 'python3', 'python') {
        $exe, $pre = $candidate -split ' ', 2
        if (-not (Test-Tool $exe)) { continue }
        $pre = @($pre | Where-Object { $_ })
        & $exe @pre -c $probe *> $null
        if ($LASTEXITCODE -eq 0) { $python = $candidate; break }
    }
    if (-not $python) {
        Write-Host 'rmp needs Python 3.11 or newer (py, python3 or python), and there is none.'
        Write-Host 'Nothing was installed. Install it, open a new PowerShell, and run this again:'
        Write-Host '  winget install --id Python.Python.3.13 -e'
        throw 'rmp was not installed: no Python 3.11 or newer.'
    }

    $rmp = Join-Path $dest 'rmp.cmd'
    if (Test-Path -LiteralPath (Join-Path $dest '.git')) {
        Write-Host "rmp: updating the framework in $dest"
        & $rmp update
        if ($LASTEXITCODE -ne 0) {
            throw "rmp was not updated: $dest could not be (see above). Fix it, or move it away and run this again."
        }
    } else {
        if (Test-Path -LiteralPath $dest) {
            if (@(Get-ChildItem -LiteralPath $dest -Force).Count -ne 0) {
                throw "rmp was not installed: $dest is there and is not a clone of the framework. Move it away and run this again."
            }
            Remove-Item -LiteralPath $dest
        }
        Write-Host "rmp: cloning $source ($branch) into $dest"
        $prompt = $env:GIT_TERMINAL_PROMPT
        $env:GIT_TERMINAL_PROMPT = '0'
        try {
            & git clone --quiet --filter=blob:none --branch $branch $source $dest
            $cloned = $LASTEXITCODE -eq 0
        } finally {
            $env:GIT_TERMINAL_PROMPT = $prompt
        }
        if (-not $cloned) { throw 'rmp was not installed: git clone failed (see above).' }
    }
    & $rmp install
    if ($LASTEXITCODE -ne 0) { throw 'rmp install did not finish (see above).' }

    # This window too, not only the next one.
    $current = "$env:Path"
    if (@($current -split ';' | Where-Object { $_.TrimEnd('\') -eq $shims }).Count -eq 0) {
        $env:Path = ($current.TrimEnd(';') + ';' + $shims).TrimStart(';')
    }

    # What a build needs beyond what rmp itself does. Said, never installed.
    $missing = @()
    $commands = @()
    if (-not (Test-Tool 'cmake')) {
        $missing += 'CMake'
        $commands += 'winget install --id Kitware.CMake -e'
    }
    if (-not (Test-Tool 'ninja')) {
        $missing += 'Ninja'
        $commands += 'winget install --id Ninja-build.Ninja -e'
    }
    $compiler = (Test-Tool 'cl') -or (Test-Tool 'clang++') -or (Test-Tool 'g++')
    $vswhere = if (${env:ProgramFiles(x86)}) {
        Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    } else { '' }
    if (-not $compiler -and $vswhere -and (Test-Path -LiteralPath $vswhere)) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $compiler = [bool]$found
    }
    if (-not $compiler) {
        $missing += 'a C++ compiler'
        $commands += 'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
    }
    Write-Host ''
    if ($missing.Count -eq 0) {
        Write-Host 'Everything a build needs is here: CMake, Ninja and a C++ compiler.'
    } else {
        Write-Host ('To build a game, this machine still needs: ' + ($missing -join ', ') + '. Install them with:')
        foreach ($line in $commands) { Write-Host "  $line" }
    }
    Write-Host 'rmp works in this window already: rmp help'
}
