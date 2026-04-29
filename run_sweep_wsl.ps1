<#
.SYNOPSIS
    Run the CI threshold-sweep benchmark inside WSL (Linux g++) and
    capture the output for PR attachment.

.DESCRIPTION
    The P1/P2 threshold tuning work needs cross-platform data: one set
    from Windows MSVC (run natively) and one from Linux g++. This
    script automates the Linux side by driving a WSL distro.

    Steps it performs:
      1. Verify the target WSL distro exists and is WSL2.
      2. (Optional) apt-install build-essential / cmake / rsync.
      3. rsync the repo from the Windows workspace to WSL's native
         ext4 at ~/fastbitcopy-sweep (/mnt/e is 5-10x slower and
         breaks CMake in subtle ways).
      4. Configure + build CI/ in Release.
      5. Run fastbitcopy_ci, tee-ing output to .agent/sweep-linux.log
         on the Windows side so it can be pasted into the PR.

    Defaults are chosen to match CI (ubuntu-latest == 24.04 right now):
    if no -Distro is specified the script auto-picks the first WSL2
    Ubuntu distro available on the host.

.PARAMETER Distro
    WSL distro name, as shown by `wsl --list --verbose`. When omitted
    (default) the script picks the first WSL2 distro whose name starts
    with 'Ubuntu' from `wsl --list --verbose`; this avoids hard-coding
    a release name that may not exist on every machine.

.PARAMETER Jobs
    Parallel build jobs. Default: $(nproc) inside the distro.

.PARAMETER SkipDepsInstall
    Skip the `apt install` step. Use this on repeat runs once the
    toolchain is known to be present.

.PARAMETER Clean
    Remove the build directory in WSL before configuring. Use when
    switching compiler flags, not needed for a plain re-run.

.PARAMETER LogFile
    Windows-side path for the run log. Default: .agent/sweep-linux.log.

.EXAMPLE
    # First run: install deps, build, run bench.
    .\run_sweep_wsl.ps1

.EXAMPLE
    # Subsequent runs after editing sources on the Windows side.
    .\run_sweep_wsl.ps1 -SkipDepsInstall

.EXAMPLE
    # Use a different distro.
    .\run_sweep_wsl.ps1 -Distro Ubuntu-WSL2 -SkipDepsInstall

.NOTES
    This script never modifies the Windows working tree; it only
    syncs *from* Windows *to* WSL. Edits made in WSL are NOT copied
    back.
#>

[CmdletBinding()]
param(
    [string]$Distro = '',
    [int]$Jobs = 0,
    [switch]$SkipDepsInstall,
    [switch]$Clean,
    [switch]$ProbeOnly,
    [string]$LogFile = '.agent/sweep-linux.log'
)

$ErrorActionPreference = 'Stop'

# --- Locate repo root (directory that contains this script) ---------------
# $PSScriptRoot is the canonical "directory this script lives in" and works
# whether the script is run via `.\run_sweep_wsl.ps1`, dot-sourced, or
# invoked through `pwsh -File`. Unlike $MyInvocation.MyCommand.Path it is
# not affected by the caller's invocation style.
$RepoRoot = $PSScriptRoot
if (-not $RepoRoot) {
    # Fallback for the very unusual case of being pasted into a REPL.
    $RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
Push-Location -LiteralPath $RepoRoot

# Resolve the log path relative to repo root so -LogFile can be relative.
if (-not [System.IO.Path]::IsPathRooted($LogFile)) {
    $LogFile = Join-Path $RepoRoot $LogFile
}
$LogDir = Split-Path -Parent $LogFile
if ($LogDir -and -not (Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

# --- Sanity-check WSL and the distro -------------------------------------
function Write-Step($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

Write-Step "Checking WSL distros"
# Parse `wsl --list --verbose` to learn which distros exist and which
# are WSL2. The output is UTF-16LE with no BOM; PowerShell's pipeline
# decodes it using [Console]::OutputEncoding, which on most systems
# leaves stray NUL (\0) bytes between every character. We first capture
# the raw bytes, decode as Unicode, and then scan the resulting text
# with a regex instead of relying on `-split`-then-`foreach`, which has
# historically interacted poorly with the embedded NULs on some boxes.

function Get-WslListVerbose {
    # Capture stdout as a single byte array via cmd.exe redirection, so
    # we can decode it ourselves regardless of the console code page.
    $tmp = [IO.Path]::GetTempFileName()
    try {
        $null = & cmd.exe /c "wsl.exe --list --verbose > `"$tmp`" 2>nul"
        $bytes = [IO.File]::ReadAllBytes($tmp)
    } finally {
        Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        # UTF-16LE BOM
        return [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
    }
    # wsl.exe on recent Windows emits UTF-16LE even without a BOM.
    # If every other byte is NUL assume UTF-16LE; otherwise treat as ASCII.
    $even = 0; $odd = 0
    for ($i = 0; $i -lt [Math]::Min($bytes.Length, 64); $i++) {
        if ($bytes[$i] -eq 0) { if ($i % 2) { $odd++ } else { $even++ } }
    }
    if ($odd -gt 4 -and $even -le 1) {
        return [Text.Encoding]::Unicode.GetString($bytes)
    }
    return [Text.Encoding]::UTF8.GetString($bytes)
}

$verboseText = Get-WslListVerbose

# Pull distro names out with a single regex pass. The columns are
# whitespace-separated: [* ] NAME STATE VERSION.
$allDistros  = New-Object System.Collections.Generic.List[string]
$wsl2Distros = New-Object System.Collections.Generic.List[string]
foreach ($m in ([regex]'(?m)^\s*\*?\s*(\S+)\s+\S+\s+(\d+)\s*$').Matches($verboseText)) {
    $name = $m.Groups[1].Value
    if ($name -eq 'NAME') { continue }
    $allDistros.Add($name) | Out-Null
    if ($m.Groups[2].Value -eq '2') {
        $wsl2Distros.Add($name) | Out-Null
    }
}

if (-not $Distro) {
    # Auto-pick: prefer an Ubuntu* WSL2 distro, else any WSL2 distro.
    $Distro = $wsl2Distros | Where-Object { $_ -like 'Ubuntu*' } | Select-Object -First 1
    if (-not $Distro) {
        $Distro = $wsl2Distros | Select-Object -First 1
    }
    if (-not $Distro) {
        Pop-Location
        Write-Error @"
No WSL2 distro found. ``wsl --list --verbose`` reports:
$verboseText
Install one with e.g. 'wsl --install -d Ubuntu-24.04', or pass
-Distro <name> explicitly.
"@
    }
    Write-Host "Auto-selected distro: $Distro (first Ubuntu* WSL2 found)" -ForegroundColor DarkGray
} elseif (-not $allDistros.Contains($Distro)) {
    Pop-Location
    Write-Error @"
WSL distro '$Distro' not found. Available distros:
  $($allDistros -join "`n  ")

Full ``wsl --list --verbose`` output:
$verboseText
Install one with e.g. 'wsl --install -d Ubuntu-24.04', or pass
-Distro <name> matching one of the above. Omit -Distro to auto-pick.
"@
}
# --- Translate Windows repo path to the distro's /mnt path ---------------
# E:\FastBitCopy  ->  /mnt/e/FastBitCopy
# UNC paths (\\server\share\...) can't be translated to /mnt/<drive>/...
# WSL does mount them under /mnt/wsl/... / \\wsl$\ but rsync performance
# is terrible and the semantics are confusing; bail out with a clear
# message instead of silently producing a broken path.
if ($RepoRoot.StartsWith('\\')) {
    Pop-Location
    Write-Error @"
Repo is on a UNC path ($RepoRoot); WSL /mnt translation only handles
local drive letters (C:, D:, E:, ...). Map the share to a drive letter
('net use X: \\server\share') and rerun from that drive, or clone the
repo onto a local disk.
"@
}
if ($RepoRoot.Length -lt 3 -or $RepoRoot[1] -ne ':') {
    Pop-Location
    Write-Error "Cannot parse drive letter from RepoRoot='$RepoRoot'."
}
$drive = $RepoRoot.Substring(0, 1).ToLowerInvariant()
$tail  = $RepoRoot.Substring(2).Replace('\', '/')
$WinRepoInWsl = "/mnt/$drive$tail"

$WslWorkDir = '$HOME/fastbitcopy-sweep'

Write-Step "Windows repo seen from WSL: $WinRepoInWsl"
Write-Step "WSL working copy:           $WslWorkDir"

# --- Build the bash script we will run inside WSL ------------------------
# Everything is written as a single heredoc-ish string; we pass it to
# `wsl -d $Distro bash -c "..."`. We avoid PowerShell string
# interpolation inside the bash body by using placeholders and a
# -replace pass.

$bashTemplate = @'
set -euo pipefail

SRC="__WIN_REPO__"
DST="__WSL_WORK__"
JOBS=__JOBS__
SKIP_DEPS=__SKIP_DEPS__
CLEAN=__CLEAN__

if [ "$JOBS" = "0" ]; then
    JOBS="$(nproc)"
fi

echo ""
echo "=== fastbitcopy sweep (WSL) ==="
echo "distro : $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
echo "kernel : $(uname -r)"
echo "cpu    : $(nproc) cores"
echo "src    : $SRC"
echo "dst    : $DST"
echo "jobs   : $JOBS"
echo ""

if [ "$SKIP_DEPS" != "1" ]; then
    echo "--- apt install build tools ---"
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential cmake rsync ca-certificates
fi

command -v rsync  >/dev/null || { echo "rsync missing; rerun without -SkipDepsInstall"; exit 1; }
command -v cmake  >/dev/null || { echo "cmake missing; rerun without -SkipDepsInstall"; exit 1; }
command -v g++    >/dev/null || { echo "g++ missing; rerun without -SkipDepsInstall"; exit 1; }

echo ""
echo "--- versions ---"
g++ --version | head -n 1
cmake --version | head -n 1
echo ""

mkdir -p "$DST"

echo "--- sync sources (rsync, excluding build/agent/VCS scratch) ---"
rsync -a --delete \
    --exclude='/.git/' \
    --exclude='/.agent/' \
    --exclude='/CI/build/' \
    --exclude='/CI/build-msvc/' \
    --exclude='/CI/build-sweep/' \
    --exclude='/CI/build-verify/' \
    --exclude='/CI/build-linux/' \
    --exclude='/TestHost/Binaries/' \
    --exclude='/TestHost/Intermediate/' \
    --exclude='/TestHost/DerivedDataCache/' \
    --exclude='/TestHost/Saved/' \
    "$SRC/" "$DST/"

cd "$DST"

if [ "$CLEAN" = "1" ]; then
    echo "--- clean CI/build-linux ---"
    rm -rf CI/build-linux
fi

echo ""
echo "--- cmake configure (Release) ---"
cmake -S CI -B CI/build-linux -DCMAKE_BUILD_TYPE=Release

echo ""
echo "--- cmake build ---"
cmake --build CI/build-linux -j"$JOBS"

echo ""
echo "--- commit fingerprint ---"
if command -v git >/dev/null && [ -d .git ]; then
    echo "git HEAD  : $(git rev-parse --short=12 HEAD) ($(git rev-parse --abbrev-ref HEAD))"
    echo "worktree  : $(git status --porcelain | wc -l) modified files vs HEAD"
else
    echo "(no git metadata in rsynced copy)"
fi

echo ""
echo "--- run fastbitcopy_ci ---"
./CI/build-linux/fastbitcopy_ci
'@

$bash = $bashTemplate `
    -replace '__WIN_REPO__', $WinRepoInWsl `
    -replace '__WSL_WORK__', $WslWorkDir `
    -replace '__JOBS__',     "$Jobs" `
    -replace '__SKIP_DEPS__', $(if ($SkipDepsInstall) { '1' } else { '0' }) `
    -replace '__CLEAN__',     $(if ($Clean)           { '1' } else { '0' })

# bash is whitespace-sensitive in a way that PowerShell isn't: a trailing
# CR on `set -euo pipefail\r` turns into `set: pipefail<CR>: invalid option
# name` because bash treats the CR as part of the word. Normalise to LF
# *before* we hand the script over to wsl.exe, and write it with a
# UTF-8-no-BOM encoding so `#!` / heredocs aren't disturbed by a BOM.
$bash = $bash -replace "`r`n", "`n" -replace "`r", "`n"

$tmpBash = Join-Path $env:TEMP ("fbc-sweep-{0}.sh" -f ([IO.Path]::GetRandomFileName()))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($tmpBash, $bash, $utf8NoBom)

# Translate the temp file's Windows path to a WSL mount path so we can
# run `bash <path>` instead of `bash -c "<long string>"` (which is
# fragile w.r.t. quoting and newline handling on cmd/PowerShell).
$tmpDrive = $tmpBash.Substring(0, 1).ToLowerInvariant()
$tmpTail  = $tmpBash.Substring(2).Replace('\', '/')
$tmpBashInWsl = "/mnt/$tmpDrive$tmpTail"

# --- Execute ------------------------------------------------------------
Write-Step "Launching WSL build + benchmark (this may take a few minutes)"
Write-Host "Log file:    $LogFile"   -ForegroundColor DarkGray
Write-Host "Bash script: $tmpBashInWsl" -ForegroundColor DarkGray

if ($ProbeOnly) {
    Write-Host ""
    Write-Host "[-ProbeOnly] Skipping actual WSL invocation. Checks passed:" -ForegroundColor Yellow
    Write-Host "  distro '$Distro' recognised" -ForegroundColor Yellow
    Write-Host "  bash script size    : $($bash.Length) chars" -ForegroundColor Yellow
    Write-Host "  bash script path    : $tmpBash" -ForegroundColor Yellow
    Remove-Item -LiteralPath $tmpBash -ErrorAction SilentlyContinue
    Pop-Location
    exit 0
}

# Tee output to both console and file. We rely on Tee-Object to avoid
# writing a half-finished log on Ctrl-C.
$wslArgs = @('-d', $Distro, '--', 'bash', $tmpBashInWsl)
$exit = 1
try {
    & wsl.exe @wslArgs 2>&1 | Tee-Object -FilePath $LogFile
    $exit = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $tmpBash -ErrorAction SilentlyContinue
    Pop-Location
}
Write-Host ""
if ($exit -eq 0) {
    Write-Host "==> Sweep finished OK. Log: $LogFile" -ForegroundColor Green
    Write-Host "    Paste the [sweep] section of the log into your PR description." -ForegroundColor Green
} else {
    Write-Host "==> Sweep failed (exit=$exit). See $LogFile for details." -ForegroundColor Red
}
exit $exit
