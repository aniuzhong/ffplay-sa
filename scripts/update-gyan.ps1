# Fetches the newest (or requested) gyan.dev release build, extracts it into
# third_party/, and records the source commit declared in its README.txt into
# UPSTREAM.toml. Run scripts/sync-fftools.ps1 afterwards to align the vendored
# fftools snapshot with that commit.
#
# Fully network-based: no local FFmpeg checkout is needed. gyan's README.txt
# only carries an abbreviated commit hash, which is expanded to the full sha
# via the GitHub API (when available; otherwise the short hash is recorded).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\update-gyan.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\update-gyan.ps1 -Version 9.0.1
param([string]$Version = "")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Headers = @{ "User-Agent" = "ffplay-standalone" }

if (-not $Version) {
    $rel = Invoke-RestMethod -Headers $Headers `
        -Uri "https://api.github.com/repos/GyanD/codexffmpeg/releases/latest"
    $Version = $rel.tag_name
    Write-Host "Latest gyan release: $Version"
}

$Name = "ffmpeg-$Version-full_build-shared.zip"
$Url = "https://github.com/GyanD/codexffmpeg/releases/download/$Version/$Name"
$ThirdParty = Join-Path $Root "third_party"
$Tree = Join-Path $ThirdParty "ffmpeg-$Version-full_build-shared"
$Cache = Join-Path $ThirdParty ".cache"
$Zip = Join-Path $Cache $Name

New-Item -ItemType Directory -Force $Cache | Out-Null

if (-not (Test-Path "$Tree\include\libavutil\avutil.h")) {
    if (-not (Test-Path $Zip)) {
        Write-Host "Downloading $Url"
        $Curl = Join-Path $env:WINDIR "System32\curl.exe"
        if (Test-Path $Curl) {
            # schannel builds fail behind TLS-intercepting proxies when revocation
            # servers are unreachable; --ssl-no-revoke only exists on those builds.
            $CurlArgs = @("-sSL")
            if ((& $Curl -V) -match "Schannel") { $CurlArgs += "--ssl-no-revoke" }
            & $Curl @CurlArgs -o $Zip $Url
            if ($LASTEXITCODE -ne 0) { throw "curl failed downloading $Url" }
        } else {
            Invoke-WebRequest -Uri $Url -OutFile $Zip
        }
    }
    Write-Host "Extracting $Name ..."
    $Tar = Join-Path $env:WINDIR "System32\tar.exe"
    if (Test-Path $Tar) {
        & $Tar -xf $Zip -C $ThirdParty
    } else {
        Expand-Archive -Path $Zip -DestinationPath $ThirdParty
    }
    if (-not (Test-Path "$Tree\include\libavutil\avutil.h")) {
        throw "Extraction did not produce the expected tree layout at $Tree"
    }
} else {
    Write-Host "gyan $Version tree already present: $Tree"
}

$Readme = Get-Content "$Tree\README.txt" -Raw
# gyan README.txt prints an abbreviated hash (".../commit/bf1b838f2a")
if ($Readme -notmatch "commit/([0-9a-f]{8,40})") {
    throw "Could not find the source commit line in $Tree\README.txt"
}
$Sha = $Matches[1]
# expand to the full sha via the GitHub API (fetch-by-sha needs the full form)
if ($Sha -notmatch "^[0-9a-f]{40}$") {
    try {
        $c = Invoke-RestMethod -Headers $Headers -Uri "https://api.github.com/repos/FFmpeg/FFmpeg/commits/$Sha"
        if ($c.sha -match "^[0-9a-f]{40}$") { $Sha = $c.sha }
    } catch {
        Write-Warning "Could not expand commit via GitHub API; recording the abbreviated hash."
    }
}
Write-Host "gyan $Version was built from FFmpeg commit $Sha"

$Utf8 = New-Object System.Text.UTF8Encoding($false)
$TomlPath = Join-Path $Root "UPSTREAM.toml"
$Toml = [IO.File]::ReadAllText($TomlPath)
$Toml = $Toml -replace '(?m)^version = ".*"', ('version = "' + $Version + '"')
$Toml = $Toml -replace '(?m)^url = ".*"', ('url = "' + $Url + '"')
$Toml = $Toml -replace '(?m)^commit = ".*"', ('commit = "' + $Sha + '"')
[IO.File]::WriteAllText($TomlPath, $Toml, $Utf8)
Write-Host "UPSTREAM.toml updated."
Write-Host "Next: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sync-fftools.ps1"
