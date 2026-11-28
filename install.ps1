param(
    [string] $InstallDir,
    [string] $Tag,
    [string] $Repository = 'isyuah/lyra',
    [string] $ArchivePath,
    [string] $Sha256,
    [switch] $NoPath,
    [switch] $Help
)

$ErrorActionPreference = 'Stop'

if ($Help) {
    Write-Output 'Usage: install.ps1 [-InstallDir <directory>] [-Tag vX.Y.Z]'
    Write-Output '       install.ps1 -InstallDir <directory> -Tag vX.Y.Z -ArchivePath <zip> -Sha256 <hash> [-NoPath]'
    exit 0
}

if (-not [Environment]::Is64BitOperatingSystem) { throw 'The Lyra SDK installer requires 64-bit Windows.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw "Invalid GitHub repository: $Repository" }

function Test-SdkDirectory([string] $Directory) {
    return (Test-Path -LiteralPath (Join-Path $Directory 'bin/lyra.exe') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Directory 'share/lyra/sdk-sha256.txt') -PathType Leaf)
}

function Find-ExistingSdk {
    $candidates = [Collections.Generic.List[string]]::new()
    foreach ($commandName in 'lyra.exe', 'lyra') {
        $command = Get-Command $commandName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { $candidates.Add((Split-Path (Split-Path $command.Source -Parent) -Parent)) }
    }
    foreach ($pathEntry in @($env:Path, [Environment]::GetEnvironmentVariable('Path', 'User'), [Environment]::GetEnvironmentVariable('Path', 'Machine'))) {
        foreach ($entry in ($pathEntry -split ';')) {
            if ([string]::IsNullOrWhiteSpace($entry)) { continue }
            $bin = $entry.Trim().Trim('"')
            if ((Split-Path $bin -Leaf) -ieq 'bin') { $candidates.Add((Split-Path $bin -Parent)) }
        }
    }
    foreach ($candidate in $candidates | Select-Object -Unique) {
        if ((Test-Path -LiteralPath $candidate -PathType Container) -and (Test-SdkDirectory $candidate)) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Assert-SdkPayload([string] $Directory, [string] $Version) {
    $binary = Join-Path $Directory 'bin/lyra.exe'
    $manifest = Join-Path $Directory 'share/lyra/sdk-sha256.txt'
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf) -or -not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw 'The downloaded archive is missing the Lyra executable or SDK manifest.'
    }
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $entries = @{}
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') { throw 'The SDK file manifest is malformed.' }
        $relative = $Matches[2].Replace('/', [IO.Path]::DirectorySeparatorChar)
        if ([IO.Path]::IsPathRooted($relative) -or $relative.Split([IO.Path]::DirectorySeparatorChar) -contains '..') {
            throw "The SDK manifest contains an unsafe path: $relative"
        }
        $file = [IO.Path]::GetFullPath((Join-Path $Directory $relative))
        if (-not $file.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "The SDK manifest refers to a missing or unsafe file: $relative"
        }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash -ine $Matches[1]) {
            throw "The SDK file failed its SHA-256 check: $relative"
        }
        $entries[$relative.Replace('\', '/')] = $true
    }
    if (-not $entries.ContainsKey('bin/lyra.exe')) { throw 'The SDK manifest does not cover bin/lyra.exe.' }
    $actualVersion = (& $binary --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualVersion -ne "lyra $Version") {
        throw "Expected 'lyra $Version', got '$actualVersion'."
    }
}

function Test-ManagedSdkDirectory([string] $Directory) {
    try {
        $manifest = Join-Path $Directory 'share/lyra/sdk-sha256.txt'
        $root = [IO.Path]::GetFullPath($Directory).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        $expected = @{}
        foreach ($line in Get-Content -LiteralPath $manifest) {
            if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') { return $false }
            $expected[$Matches[2].Replace('\', '/')] = $Matches[1]
        }
        foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse) {
            $relative = $file.FullName.Substring($root.Length).Replace('\', '/')
            if ($relative -eq 'share/lyra/sdk-sha256.txt') { continue }
            if (-not $expected.ContainsKey($relative) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash -ine $expected[$relative]) {
                return $false
            }
            $expected.Remove($relative)
        }
        return $expected.Count -eq 0
    } catch {
        return $false
    }
}

function Add-UserPath([string] $Directory) {
    $bin = [IO.Path]::GetFullPath((Join-Path $Directory 'bin')).TrimEnd('\')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($userPath -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($entries | Where-Object { [IO.Path]::GetFullPath($_.Trim().Trim('"')).TrimEnd('\') -ieq $bin }) { return }
    $updated = (@($entries) + $bin) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $updated, 'User')
    $env:Path = "$bin;$env:Path"
}

if ($ArchivePath) {
    if (-not $Tag -or -not $Sha256) { throw 'Local archives require both -Tag and -Sha256.' }
    if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) { throw "SDK archive not found: $ArchivePath" }
    if ($Sha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Expected a 64-character SHA-256 hash.' }
    $archive = [IO.Path]::GetFullPath($ArchivePath)
    $expectedHash = $Sha256
} else {
    if ($Sha256) { throw '-Sha256 is only used with -ArchivePath.' }
    $headers = @{ 'User-Agent' = 'Lyra-SDK-Installer'; Accept = 'application/vnd.github+json' }
    if (-not $Tag) {
        $release = @(Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases?per_page=1" -Headers $headers |
            Where-Object { -not $_.draft } | Select-Object -First 1)[0]
        if (-not $release) { throw 'No published Lyra release was found.' }
        $Tag = $release.tag_name
    }
    if ($Tag -notmatch '^v(?<version>[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)$') {
        throw "Invalid release tag: $Tag"
    }
    $Version = $Matches.version
    $assetName = "lyra-$Version-windows-x64.zip"
    $baseUri = "https://github.com/$Repository/releases/download/$Tag"
    $workRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $work = Join-Path $workRoot ".lyra-download-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        $archive = Join-Path $work $assetName
        $sums = Join-Path $work 'SHA256SUMS'
        Invoke-WebRequest -UseBasicParsing -Uri "$baseUri/$assetName" -OutFile $archive
        Invoke-WebRequest -UseBasicParsing -Uri "$baseUri/SHA256SUMS" -OutFile $sums
        $sumLine = Get-Content -LiteralPath $sums | Where-Object { $_ -match "^([0-9A-Fa-f]{64})\s+\*?$([Regex]::Escape($assetName))$" } | Select-Object -First 1
        if (-not $sumLine) { throw "SHA256SUMS does not contain $assetName." }
        $sumLine -match '^([0-9A-Fa-f]{64})' | Out-Null
        $expectedHash = $Matches[1]
    } catch {
        if (Test-Path -LiteralPath $work -PathType Container) {
            $resolvedWork = [IO.Path]::GetFullPath($work)
            if ([IO.Path]::GetDirectoryName($resolvedWork) -ieq $workRoot -and $resolvedWork.StartsWith((Join-Path $workRoot '.lyra-download-'), [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $resolvedWork -Recurse -Force
            }
        }
        throw
    }
}

try {
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash
    if ($actualHash -ine $expectedHash) { throw 'The SDK archive failed its SHA-256 check.' }
    if (-not $Tag -or $Tag -notmatch '^v(?<version>[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)$') {
        throw "Invalid release tag: $Tag"
    }
    $Version = $Matches.version
    if (-not $InstallDir) { $InstallDir = Find-ExistingSdk }
    if (-not $InstallDir) { $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs/Lyra' }
    $target = [IO.Path]::GetFullPath($InstallDir).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $parent = [IO.Path]::GetDirectoryName($target)
    if (-not $parent -or $target -eq [IO.Path]::GetPathRoot($target)) { throw "Unsafe SDK install directory: $target" }
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $parent = (Resolve-Path -LiteralPath $parent).Path
    if ([IO.Path]::GetDirectoryName($target) -ine $parent) { throw 'The SDK install directory must not use a reparse-point parent.' }
    if (Test-Path -LiteralPath $target -PathType Leaf) { throw "The SDK install path is a file: $target" }
    if ((Test-Path -LiteralPath $target -PathType Container) -and -not (Test-SdkDirectory $target)) {
        throw "The existing install directory is not a Lyra SDK: $target"
    }
    foreach ($process in Get-Process -Name lyra -ErrorAction SilentlyContinue) {
        try { $runningPath = $process.Path } catch { $runningPath = $null }
        if ($runningPath -and $runningPath.StartsWith($target.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Lyra is running from $target. Close the Lyra language server or CLI and retry."
        }
    }

    $stage = Join-Path $parent ".lyra-stage-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $stage | Out-Null
    try {
        Expand-Archive -LiteralPath $archive -DestinationPath $stage
        $payloads = @(Get-ChildItem -LiteralPath $stage -Directory)
        if ($payloads.Count -ne 1) { throw 'The SDK archive must contain exactly one top-level directory.' }
        $payload = $payloads[0].FullName
        Assert-SdkPayload $payload $Version

        $backup = Join-Path $parent ".lyra-backup-$([Guid]::NewGuid().ToString('N'))"
        $hadPrevious = Test-Path -LiteralPath $target -PathType Container
        if ($hadPrevious) { [IO.Directory]::Move($target, $backup) }
        try {
            [IO.Directory]::Move($payload, $target)
            $installedVersion = (& (Join-Path $target 'bin/lyra.exe') --version | Out-String).Trim()
            if ($LASTEXITCODE -ne 0 -or $installedVersion -ne "lyra $Version") { throw "Expected 'lyra $Version', got '$installedVersion'." }
        } catch {
            if (Test-Path -LiteralPath $target -PathType Container) {
                $resolvedTarget = [IO.Path]::GetFullPath($target)
                if ([IO.Path]::GetDirectoryName($resolvedTarget) -ieq $parent) { Remove-Item -LiteralPath $resolvedTarget -Recurse -Force }
            }
            if ($hadPrevious -and (Test-Path -LiteralPath $backup -PathType Container)) { [IO.Directory]::Move($backup, $target) }
            throw
        }
        if ($hadPrevious -and (Test-Path -LiteralPath $backup -PathType Container) -and (Test-ManagedSdkDirectory $backup)) {
            $resolvedBackup = [IO.Path]::GetFullPath($backup)
            if ([IO.Path]::GetDirectoryName($resolvedBackup) -ieq $parent -and $resolvedBackup.StartsWith((Join-Path $parent '.lyra-backup-'), [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $resolvedBackup -Recurse -Force
            }
        } elseif ($hadPrevious -and (Test-Path -LiteralPath $backup -PathType Container)) {
            Write-Warning "The previous directory contained modified or unmanaged files; it was preserved at $backup."
        }
    } finally {
        if (Test-Path -LiteralPath $stage -PathType Container) {
            $resolvedStage = [IO.Path]::GetFullPath($stage)
            if ([IO.Path]::GetDirectoryName($resolvedStage) -ieq $parent -and $resolvedStage.StartsWith((Join-Path $parent '.lyra-stage-'), [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $resolvedStage -Recurse -Force
            }
        }
    }

    if (-not $NoPath) {
        try { Add-UserPath $target }
        catch { Write-Warning "SDK installed, but updating the user PATH failed: $_" }
    }
    Write-Output "Installed Lyra $Version to $target"
    if (-not $NoPath) { Write-Output 'Open a new terminal to use the updated PATH.' }
} finally {
    if ($work -and (Test-Path -LiteralPath $work -PathType Container)) {
        $resolvedWork = [IO.Path]::GetFullPath($work)
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
        if ([IO.Path]::GetDirectoryName($resolvedWork) -ieq $tempRoot -and $resolvedWork.StartsWith((Join-Path $tempRoot '.lyra-download-'), [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedWork -Recurse -Force
        }
    }
}
