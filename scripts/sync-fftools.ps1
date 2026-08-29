# Syncs the pristine fftools snapshot and regenerates shim/ from the FFmpeg
# commit recorded in UPSTREAM.toml (strict alignment policy: the fftools ref
# moves with the gyan package, never on its own).
#
# Fully network-based by default — no local FFmpeg checkout is needed:
#   - gyan release builds (version = X.Y.Z): the official release tarball from
#     ffmpeg.org, verified against the recorded commit via `git ls-remote`;
#     on mismatch it falls back to a GitHub archive of the exact commit.
#   - gyan git-master builds: a GitHub archive of the exact commit (release
#     tarballs do not exist for arbitrary commits).
# A source tarball cannot be verified locally against a commit hash by itself,
# so the ls-remote check is what keeps the release shortcut honest.
#
# The vendored fftools/*.c|h files must stay byte-identical to upstream. Every
# local adaptation lives in shim/ (regenerated here) so a sync is a plain
# overwrite with no merging:
#   - shim/ holds only headers the gyan dev package does not install, rebuilt
#     as the closure of the "compat/..." / "libavutil/..." includes; upstream
#     copies with in-tree relative includes rewritten to their public
#     "libavutil/..." location
#   - libm.h is the one exception: upstream's version keys off configure-time
#     HAVE_* math macros that a synthesized config.h cannot reproduce and
#     collides with UCRT declarations, so a small hand-written stub is used
#     instead (it only exposes <math.h>, sufficient for MSVC/UCRT).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sync-fftools.ps1
#   ... [-SourceDir <ffmpeg-tree>]   # use a local checkout instead of network
#   ... [-Commit <sha-or-tag>]       # override the commit from UPSTREAM.toml
param(
    [string]$SourceDir = "",
    [string]$Commit = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Headers = @{ "User-Agent" = "ffplay-standalone" }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---- read identity from UPSTREAM.toml ----
$Toml = Get-Content "$Root\UPSTREAM.toml" -Raw
function Get-TomlString([string]$key) {
    if ($Toml -match ('(?m)^\s*' + [regex]::Escape($key) + '\s*=\s*"([^"]*)"')) {
        return $Matches[1]
    }
    return ""
}

$GyanVersion = Get-TomlString "version"
$Repository = Get-TomlString "repository"
if (-not $Repository) { throw "UPSTREAM.toml: [upstream] repository is missing" }

$GyanInclude = Join-Path $Root "third_party\ffmpeg-$GyanVersion-full_build-shared\include"
if (-not (Test-Path "$GyanInclude\libavutil\avutil.h")) {
    throw "gyan tree not found at $GyanInclude - run scripts\update-gyan.ps1 first"
}

if (-not $Commit) {
    $Override = Get-TomlString "fftools_ref"
    if ($Override) {
        $Commit = $Override
        if (-not (Get-TomlString "reason")) {
            Write-Warning "UPSTREAM.toml sets fftools_ref without a reason; the alignment policy expects one."
        }
    } else {
        $Commit = Get-TomlString "commit"
    }
}
if (-not $Commit -or $Commit -eq "TBD") {
    throw "No upstream commit recorded in UPSTREAM.toml - run scripts\update-gyan.ps1 first."
}

# ---- network helpers ----
function Expand-Sha([string]$Sha) {
    # fetch-by-ref archives need the full sha; GitHub API expands short hashes
    if ($Sha -match "^[0-9a-f]{40}$") { return $Sha }
    try {
        $api = $Repository -replace "^https://github\.com/", "https://api.github.com/repos/"
        $c = Invoke-RestMethod -Headers $Headers -Uri "$api/commits/$Sha"
        if ($c.sha -match "^[0-9a-f]{40}$") { return $c.sha }
    } catch {
        Write-Warning "Could not expand commit '$Sha' via GitHub API."
    }
    return ""
}

function Download-File([string]$Url, [string]$Dest) {
    Write-Host "Downloading $Url"
    $Curl = Join-Path $env:WINDIR "System32\curl.exe"
    if (Test-Path $Curl) {
        $CurlArgs = @("-sSL")
        if ((& $Curl -V) -match "Schannel") { $CurlArgs += "--ssl-no-revoke" }
        & $Curl @CurlArgs -o $Dest $Url
        if ($LASTEXITCODE -eq 0 -and (Get-Item $Dest -ErrorAction SilentlyContinue).Length -gt 0) { return }
        Remove-Item $Dest -ErrorAction SilentlyContinue
    }
    Invoke-WebRequest -Uri $Url -OutFile $Dest -Headers $Headers
}

function Test-TagMatchesCommit([string]$Ver, [string]$Sha) {
    # best-effort: $true also when git or the network is unavailable (warn only)
    if (-not $Sha) { return $true }  # nothing to verify against
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Warning "git not found; cannot verify the release tarball matches commit $Sha."
        return $true
    }
    $out = & git ls-remote $Repository ("refs/tags/n$Ver^{}") ("refs/tags/n$Ver") 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $out) {
        Write-Warning "Could not query tags of $Repository; cannot verify the release tarball matches commit $Sha."
        return $true
    }
    $peeled = ($out | ForEach-Object { ($_ -split "`t")[0] })[-1]  # ^{} line (peeled) sorts last
    if ($peeled -and ($peeled.ToLower() -ne $Sha.ToLower())) {
        Write-Warning "Tag n$Ver points at $peeled, but the gyan package declares $Sha - using the exact commit."
        return $false
    }
    return $true
}

function Get-SourceTree {
    # downloads + extracts a source snapshot, returns the tree root directory
    $Tmp = Join-Path ([IO.Path]::GetTempPath()) ("ffplay-src-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $Tmp | Out-Null
    $Cache = Join-Path $Root "third_party\.cache"
    New-Item -ItemType Directory -Force $Cache | Out-Null
    $Tar = Join-Path $env:WINDIR "System32\tar.exe"
    if (-not (Test-Path $Tar)) { throw "tar.exe not found (Windows 10 1803+ ships it)" }

    try {
        if ($GyanVersion -match "^\d+(\.\d+)+$" -and (Test-TagMatchesCommit $GyanVersion $FullSha)) {
            # .tar.gz on purpose: bsdtar decompresses .tar.xz through an external
            # xz child process, which hangs in non-console contexts on Windows
            $archive = Join-Path $Cache "ffmpeg-$GyanVersion.tar.gz"
            if (-not (Test-Path $archive)) {
                Download-File "https://ffmpeg.org/releases/ffmpeg-$GyanVersion.tar.gz" $archive
            }
            & $Tar -xf $archive -C $Tmp
        } else {
            if (-not $FullSha) {
                throw "Cannot archive an abbreviated hash '$Commit'; expand it first (network to github.com required)."
            }
            $archive = Join-Path $Cache "ffmpeg-src-$($FullSha.Substring(0, 12)).tar.gz"
            if (-not (Test-Path $archive)) {
                Download-File "$Repository/archive/$FullSha.tar.gz" $archive
            }
            & $Tar -xf $archive -C $Tmp
        }
        $top = Get-ChildItem $Tmp -Directory | Select-Object -First 1
        if (-not $top -or -not (Test-Path "$($top.FullName)\fftools\ffplay.c")) {
            throw "Unexpected archive layout under $Tmp"
        }
        return $top.FullName
    } catch {
        Remove-Item $Tmp -Recurse -Force -ErrorAction SilentlyContinue
        throw
    }
}

# ---- stage the source snapshot ----
$FullSha = ""
if (-not $SourceDir) { $FullSha = Expand-Sha $Commit }
$TmpRoot = ""
if ($SourceDir) {
    if (-not (Test-Path "$SourceDir\fftools\ffplay.c")) {
        throw "fftools/ffplay.c not found under -SourceDir '$SourceDir'"
    }
    $TreeRoot = $SourceDir
    Write-Host "Using local source tree: $SourceDir (expected to be at $Commit)"
} else {
    $TreeRoot = Get-SourceTree
    $TmpRoot = $TreeRoot
    Write-Host "Source snapshot for $Commit staged at $TreeRoot"
}

try {
    # ---- copy the fftools whitelist ----
    New-Item -ItemType Directory -Force "$Root\fftools" | Out-Null
    $Want = @("ffplay.c", "ffplay_renderer.c", "ffplay_renderer.h",
              "cmdutils.c", "cmdutils.h", "opt_common.c", "opt_common.h", "fopen_utf8.h")
    $Copied = @()
    foreach ($f in $Want) {
        if (Test-Path "$TreeRoot\fftools\$f") {
            Copy-Item "$TreeRoot\fftools\$f" "$Root\fftools\" -Force
            $Copied += $f
        } else {
            Write-Host "  skip fftools/$f (not present at $Commit)"
        }
    }

    # ---- regenerate shim/ from scratch ----
    if (Test-Path "$Root\shim") { Remove-Item "$Root\shim" -Recurse -Force }
    New-Item -ItemType Directory -Force "$Root\shim\compat", "$Root\shim\libavutil" | Out-Null

    function Write-ShimFile([string]$Rel, [string]$Content) {
        $dst = Join-Path "$Root\shim" ($Rel -replace "/", "\")
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        [IO.File]::WriteAllText($dst, $Content, $Utf8)
        Write-Host "  shim/$Rel"
    }

    function Get-InternalIncludes([string[]]$Paths) {
        $set = @{}
        foreach ($p in $Paths) {
            if (-not (Test-Path $p)) { continue }
            $text = [IO.File]::ReadAllText($p)
            foreach ($m in [regex]::Matches($text, '#include\s+"((?:compat|libavutil)/[^"]+)"')) {
                $set[$m.Groups[1].Value] = $true
            }
        }
        return @($set.Keys)
    }

    # headers replaced with hand-written equivalents instead of upstream copies
    $Stubs = @{
        "libavutil/libm.h" = @'
/**
 * The gyan dev package omits libavutil/libm.h. Upstream's version keys off
 * configure-time HAVE_* math macros that a synthesized config.h cannot
 * reproduce and collides with UCRT declarations, so this stub just exposes
 * the standard C math API; MSVC/UCRT provides all of it.
 */

#ifndef AVUTIL_LIBM_H
#define AVUTIL_LIBM_H

#include <math.h>

#endif /* AVUTIL_LIBM_H */
'@
    }

    for ($round = 0; $round -lt 4; $round++) {
        $scan = @(Get-ChildItem "$Root\fftools" -File | Where-Object { $_.Extension -in ".c", ".h" } | ForEach-Object { $_.FullName })
        $scan += @(Get-ChildItem "$Root\shim" -File -Recurse | Where-Object { $_.Extension -eq ".h" } | ForEach-Object { $_.FullName })
        $internal = Get-InternalIncludes $scan
        $new = 0
        foreach ($h in ($internal | Sort-Object)) {
            $rel = $h -replace "/", "\"
            $dst = Join-Path "$Root\shim" $rel
            if (Test-Path $dst) { continue }
            if (Test-Path "$GyanInclude\$rel") { continue }  # installed public header, gyan provides it
            if ($Stubs.ContainsKey($h)) {
                Write-ShimFile $h ($Stubs[$h] + "`r`n")
                $new++
                continue
            }
            $src = Join-Path $TreeRoot $rel
            if (Test-Path $src) {
                Write-ShimFile $h ([IO.File]::ReadAllText($src))
                $new++
            } else {
                Write-Warning "referenced internal header not found upstream: $h"
            }
        }
        if ($new -eq 0) { break }
    }

    # ---- local adaptation: rewrite in-tree relative includes ----
    # Internal headers include siblings like "mem.h" relative to the libavutil
    # source tree; rewrite them to their public "libavutil/..." location so they
    # resolve against gyan's include dir. config.h lives at the include root and
    # must not be touched.
    Get-ChildItem "$Root\shim\libavutil" -Filter *.h -Recurse | ForEach-Object {
        $text = [IO.File]::ReadAllText($_.FullName)
        $fixed = [regex]::Replace($text, '#include\s+"([a-z0-9_]+\.h)"', {
            param($m)
            $name = $m.Groups[1].Value
            if ($name -eq "config.h" -or $name -eq "config_components.h") { return $m.Value }
            return '#include "libavutil/' + $name + '"'
        })
        if ($fixed -cne $text) {
            [IO.File]::WriteAllText($_.FullName, $fixed, $Utf8)
            Write-Host "  rewrote relative includes in shim/libavutil/$($_.Name)"
        }
    }

    # ---- record the sync ----
    $Toml = [IO.File]::ReadAllText("$Root\UPSTREAM.toml")
    $Toml = $Toml -replace '(?m)^last_synced = ".*"', ('last_synced = "' + (Get-Date -Format "yyyy-MM-dd") + '"')
    [IO.File]::WriteAllText("$Root\UPSTREAM.toml", $Toml, $Utf8)
} finally {
    if ($TmpRoot) { Remove-Item $TmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host ("Synced fftools from {0} ({1} files)" -f $Commit, $Copied.Count)
if (Test-Path "$Root\.git") {
    git -C $Root diff --stat -- fftools shim
}
