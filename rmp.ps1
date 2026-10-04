# rmp: build, run, test and ship this project. `rmp help` lists the commands.
#
# PowerShell's launcher: it only finds a Python 3.11+ and hands every argument
# to tools\rmp.py, the whole implementation. `rmp` (POSIX sh) and rmp.cmd are
# the same launcher for the other shells.
#
# No $ErrorActionPreference = 'Stop': in Windows PowerShell 5.1 that turns a
# native program's stderr into a terminating error, and the probe may fail.
$rmp = Join-Path $PSScriptRoot 'tools\rmp.py'
$probe = 'import sys; sys.exit(sys.version_info < (3, 11))'
foreach ($candidate in 'py -3', 'python3', 'python') {
    $exe, $pre = $candidate -split ' ', 2
    if (-not (Get-Command $exe -CommandType Application -ErrorAction SilentlyContinue)) { continue }
    $pre = @($pre | Where-Object { $_ })
    & $exe @pre -c $probe *> $null
    if ($LASTEXITCODE -eq 0) {
        & $exe @pre $rmp @args
        exit $LASTEXITCODE
    }
}
[Console]::Error.WriteLine('rmp: needs Python 3.11 or newer on PATH (py, python3 or python).')
[Console]::Error.WriteLine('     https://www.python.org/downloads/')
exit 127
