# Build the current QA integration branch and refresh its local EXE snapshot.
# This script intentionally creates no installer and no archive.

[CmdletBinding()]
param(
    [string]$BuildDir = "build_qa",
    [string]$DistDir = "dist_qa",
    [string]$Config = "Release",
    [string]$Generator = "",
    [string]$QaNotePath = "QA说明.md",
    [string]$PackageKeyId = $env:STABLE_UPDATE_PACKAGE_KEY_ID,
    [string]$PackagePublicKeyHex = $env:ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX,
    [ValidateSet("enabled", "disabled")]
    [string]$FeedbackMode = "enabled",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$ExpectedBranch = "qa/current"
$feedbackCachePath = $null

Push-Location $Root
try {
    $branch = (& git branch --show-current).Trim()
    if ($branch -ne $ExpectedBranch) {
        throw "QA EXE packaging is restricted to '$ExpectedBranch'; current branch is '$branch'"
    }

    $gitCommonDir = (& git rev-parse --path-format=absolute --git-common-dir).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $gitCommonDir) {
        throw "Unable to resolve the shared Git directory"
    }
    $sharedRoot = if ((Split-Path $gitCommonDir -Leaf) -ieq ".git") {
        Split-Path $gitCommonDir -Parent
    } else {
        $Root
    }

    $buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) {
        [IO.Path]::GetFullPath($BuildDir)
    } else {
        [IO.Path]::GetFullPath((Join-Path $Root $BuildDir))
    }
    $distPath = if ([IO.Path]::IsPathRooted($DistDir)) {
        [IO.Path]::GetFullPath($DistDir)
    } else {
        [IO.Path]::GetFullPath((Join-Path $sharedRoot $DistDir))
    }
    $qaExe = Join-Path $distPath "Airs\HarkenSearch.exe"
    $qaDir = Split-Path $qaExe -Parent
    if ((Split-Path $qaDir -Parent) -ine $distPath -or (Split-Path $qaDir -Leaf) -ine "Airs") {
        throw "Refusing to deploy outside the exact QA target directory '$distPath\Airs'"
    }
    $buildId = (& git rev-parse --short=12 HEAD).Trim()
    $sourceCommit = (& git rev-parse HEAD).Trim()
    $channel = "qa"
    if ($PackageKeyId -notmatch '^[A-Za-z0-9_.-]{1,64}$') {
        throw "A provisioned Bootstrap package key id is required"
    }
    if ($PackagePublicKeyHex -notmatch '^[0-9a-f]{64}$' -or
        $PackagePublicKeyHex -match '^0+$') {
        throw "A provisioned nonzero lowercase Bootstrap public key is required"
    }

    $feedbackConfig = [ordered]@{
        AIRS_CUEVI_FEEDBACK_ENDPOINT = $env:AIRS_CUEVI_FEEDBACK_ENDPOINT
        AIRS_CUEVI_FEEDBACK_KEY = $env:AIRS_CUEVI_FEEDBACK_KEY
        AIRS_CUEVI_PROJECT_ID = $env:AIRS_CUEVI_PROJECT_ID
        AIRS_CUEVI_BOARD_ID = $env:AIRS_CUEVI_BOARD_ID
        AIRS_CUEVI_MILESTONE_ID = $env:AIRS_CUEVI_MILESTONE_ID
    }
    if ($FeedbackMode -eq "enabled") {
        foreach ($name in $feedbackConfig.Keys) {
            if ([string]::IsNullOrWhiteSpace($feedbackConfig[$name])) {
                throw "$name is required for the QA feedback build"
            }
        }
        if ($feedbackConfig.AIRS_CUEVI_FEEDBACK_ENDPOINT -notmatch '^https?://[^\s"<>]+$') {
            throw "AIRS_CUEVI_FEEDBACK_ENDPOINT must be an HTTP(S) URL"
        }
        foreach ($name in @("AIRS_CUEVI_PROJECT_ID", "AIRS_CUEVI_BOARD_ID", "AIRS_CUEVI_MILESTONE_ID")) {
            if ($feedbackConfig[$name] -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$') {
                throw "$name must be a safe Cuevi identifier"
            }
        }
        if ($feedbackConfig.AIRS_CUEVI_FEEDBACK_KEY -notmatch '^[\x20-\x7E]{1,4096}$') {
            throw "AIRS_CUEVI_FEEDBACK_KEY must be a printable HTTP header value"
        }
    }

    if (-not $SkipBuild) {
        $feedbackCachePath = Join-Path ([IO.Path]::GetTempPath()) "aris-qa-feedback-$([Guid]::NewGuid().ToString('N')).cmake"
        $feedbackCacheLines = if ($FeedbackMode -eq "enabled") {
            @(
                'set(ARIS_PUBLIC_RELEASE OFF CACHE BOOL "Build a public-release-safe surface" FORCE)',
                'set(AIRS_CUEVI_FEEDBACK_ENDPOINT "$ENV{AIRS_CUEVI_FEEDBACK_ENDPOINT}" CACHE STRING "Cuevi feedback endpoint" FORCE)',
                'set(_airs_cuevi_feedback_key "$ENV{AIRS_CUEVI_FEEDBACK_KEY}")',
                'string(REPLACE "\\" "\\\\" _airs_cuevi_feedback_key "${_airs_cuevi_feedback_key}")',
                'string(REPLACE "\"" "\\\"" _airs_cuevi_feedback_key "${_airs_cuevi_feedback_key}")',
                'set(AIRS_CUEVI_FEEDBACK_KEY "${_airs_cuevi_feedback_key}" CACHE STRING "Cuevi feedback key" FORCE)',
                'set(AIRS_CUEVI_PROJECT_ID "$ENV{AIRS_CUEVI_PROJECT_ID}" CACHE STRING "Cuevi project id" FORCE)',
                'set(AIRS_CUEVI_BOARD_ID "$ENV{AIRS_CUEVI_BOARD_ID}" CACHE STRING "Cuevi board id" FORCE)',
                'set(AIRS_CUEVI_MILESTONE_ID "$ENV{AIRS_CUEVI_MILESTONE_ID}" CACHE STRING "Cuevi milestone id" FORCE)'
            )
        } else {
            @(
                'set(ARIS_PUBLIC_RELEASE OFF CACHE BOOL "Build a public-release-safe surface" FORCE)',
                'set(AIRS_CUEVI_FEEDBACK_ENDPOINT "" CACHE STRING "Cuevi feedback endpoint" FORCE)',
                'set(AIRS_CUEVI_FEEDBACK_KEY "" CACHE STRING "Cuevi feedback key" FORCE)',
                'set(AIRS_CUEVI_PROJECT_ID "" CACHE STRING "Cuevi project id" FORCE)',
                'set(AIRS_CUEVI_BOARD_ID "" CACHE STRING "Cuevi board id" FORCE)',
                'set(AIRS_CUEVI_MILESTONE_ID "" CACHE STRING "Cuevi milestone id" FORCE)'
            )
        }
        [IO.File]::WriteAllText($feedbackCachePath, ($feedbackCacheLines -join [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
        $feedbackCmakeArgs = @(
            "-DARIS_PUBLIC_RELEASE=OFF",
            "-DAIRS_BUILD_ID=$buildId",
            "-DAIRS_RELEASE_CHANNEL=qa",
            "-DARIS_BOOTSTRAP_RELEASE_BUILD=ON",
            "-DARIS_BOOTSTRAP_PACKAGE_KEY_ID=$PackageKeyId",
            "-DARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX=$PackagePublicKeyHex"
        )
        $genArgs = if ($Generator) { @("-G", $Generator) } else { @() }
        $cmakeInitialCacheArgs = @("-C", $feedbackCachePath)
        try {
            & cmake -S $Root -B $buildPath @genArgs @cmakeInitialCacheArgs @feedbackCmakeArgs
            if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }
        }
        finally {
            if ($feedbackCachePath -and (Test-Path -LiteralPath $feedbackCachePath)) {
                Remove-Item -LiteralPath $feedbackCachePath -Force -ErrorAction SilentlyContinue
            }
            $feedbackCachePath = $null
        }

        & cmake --build $buildPath --config $Config --target HarkenSearch --parallel
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }
    }

    $exeCandidates = @(
        (Join-Path $buildPath "$Config\HarkenSearch.exe"),
        (Join-Path $buildPath "HarkenSearch.exe")
    )
    $exePath = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $exePath) {
        throw "HarkenSearch.exe not found under '$buildPath'"
    }
    $binDir = Split-Path $exePath -Parent
    $pairFiles = @(
        "HarkenSearch.exe",
        "HarkenSearch.exe.sig",
        "HarkenSearchBootstrap.exe",
        "HarkenSearchBootstrap.exe.sig",
        "harken-runtime-pair.json"
    )
    foreach ($name in $pairFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $binDir $name) -PathType Leaf)) {
            throw "Required runtime-pair artifact is missing: $name"
        }
    }
    $legalDocumentRelativePaths = @(
        "docs\legal\EULA.md",
        "docs\legal\PRIVACY_POLICY.md",
        "docs\legal\OPEN_SOURCE_RELEASE_NOTICE.md",
        "docs\legal\EULA.en.md",
        "docs\legal\PRIVACY_POLICY.en.md",
        "docs\legal\OPEN_SOURCE_RELEASE_NOTICE.en.md"
    )
    foreach ($relativePath in $legalDocumentRelativePaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $binDir $relativePath) -PathType Leaf)) {
            throw "Required legal document is missing: $relativePath"
        }
    }
    $qaNoteName = "QA说明.md"
    $qaNoteSource = Join-Path $Root $QaNotePath
    if (-not (Test-Path -LiteralPath $qaNoteSource -PathType Leaf)) {
        throw "Required QA note is missing: QA说明.md"
    }
    $qaNoteText = [IO.File]::ReadAllText($qaNoteSource)
    $qaNoteHeadings = @(
        "## 要查哪些部分",
        "## 怎么查",
        "## 怎样算过",
        "## 这次不查什么",
        "## 没通过时留下什么"
    )
    foreach ($qaNoteHeading in $qaNoteHeadings) {
        $qaNoteFound = $false
        foreach ($qaNoteLine in [regex]::Split($qaNoteText, "\r?\n")) {
            if ($qaNoteLine.Trim() -eq $qaNoteHeading) {
                $qaNoteFound = $true
            }
        }
        if (-not $qaNoteFound) {
            throw "Required QA note section is missing: $qaNoteHeading"
        }
    }
    $pairTool = Join-Path $Root "tools\generate_runtime_pair.py"
    & python -B $pairTool verify `
        --aris (Join-Path $binDir "HarkenSearch.exe") `
        --bootstrap (Join-Path $binDir "HarkenSearchBootstrap.exe") `
        --bootstrap-sig (Join-Path $binDir "HarkenSearchBootstrap.exe.sig") `
        --receipt (Join-Path $binDir "harken-runtime-pair.json") `
        --source-commit $sourceCommit `
        --build-id $buildId `
        --channel $channel `
        --config $Config `
        --key-id $PackageKeyId `
        --public-key-hex $PackagePublicKeyHex
    if ($LASTEXITCODE -ne 0) { throw "Build runtime-pair verification failed" }

    # Only stop the exact QA snapshot process that must be replaced.
    $qaExeFullPath = [IO.Path]::GetFullPath($qaExe)
    $runningQa = @(Get-CimInstance Win32_Process -Filter "Name='HarkenSearch.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $qaExeFullPath)
        })
    foreach ($process in $runningQa) {
        Stop-Process -Id $process.ProcessId -Force
    }

    $unlocked = $false
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        $stillRunning = @(Get-CimInstance Win32_Process -Filter "Name='HarkenSearch.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $qaExeFullPath)
            })
        if ($stillRunning.Count -eq 0) {
            if (-not (Test-Path -LiteralPath $qaExe)) {
                $unlocked = $true
                break
            }
            try {
                $probe = [IO.File]::Open($qaExe, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $probe.Dispose()
                $unlocked = $true
                break
            } catch {
                # Windows may release an image mapping shortly after process exit.
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $unlocked) {
        throw "QA HarkenSearch.exe did not unlock after the exact target process stopped"
    }

    $publishDir = "$qaDir.runtime-pair.publish.$([Guid]::NewGuid().ToString('N'))"
    $previousDir = "$qaDir.runtime-pair.previous"
    $notePublishPath = "$qaDir.runtime-pair.note.publish.$([Guid]::NewGuid().ToString('N'))"
    $notePreviousPath = "$qaDir.runtime-pair.note.previous"
    $qaNoteDestination = Join-Path $distPath $qaNoteName
    if (Test-Path -LiteralPath $publishDir) {
        throw "Refusing to reuse a runtime-pair publish directory"
    }
    if (Test-Path -LiteralPath $previousDir) {
        throw "A previous runtime-pair recovery directory still exists"
    }
    if (Test-Path -LiteralPath $notePublishPath) {
        throw "Refusing to reuse a runtime-pair note publish file"
    }
    if (Test-Path -LiteralPath $notePreviousPath) {
        throw "A previous runtime-pair note recovery file still exists"
    }
    New-Item -ItemType Directory -Path $publishDir | Out-Null
    $published = $false
    $notePublished = $false
    $movedPrevious = $false
    $movedNotePrevious = $false
    try {
        $runtimeFileAllowlist = @(
            "HarkenSearch.exe", "HarkenSearch.exe.sig", "HarkenSearchBootstrap.exe",
            "HarkenSearchBootstrap.exe.sig", "harken-runtime-pair.json",
            "HarkenSearchProToolsBridge.exe",
            "WebView2Loader.dll", "reaper_harken.dll", "lucide.ttf",
            "splash_beta.png", "harken_thesaurus.json", "LICENSE",
            "aris_release_notes.json",
            "THIRD_PARTY_NOTICES.md", "ASSET_PROVENANCE.md",
            "BINARY_PROVENANCE.md", "PRIVACY.md", "AI_FEATURE_DISCLOSURE.md"
        )
        # The QA notice panel reads aris_release_notes.json next to the exe; bundle it
        # from the canonical release history so local-version notes stay truthful.
        $qaNotesVersion = (& python -B -c "import sys; sys.path.insert(0, sys.argv[1]); from tools.pack_engine import read_current_version; print(read_current_version())" $Root).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $qaNotesVersion) {
            throw "Unable to read the current version for QA release notes"
        }
        $notesPrep = & python -B (Join-Path $Root "tools/pack_engine.py") --prepare-release-notes `
            --version $qaNotesVersion --release-dir $binDir 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "QA release notes preparation failed: $notesPrep"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $binDir "aris_release_notes.json") -PathType Leaf)) {
            throw "QA release notes file was not prepared next to the build"
        }
        foreach ($name in $runtimeFileAllowlist) {
            $source = Join-Path $binDir $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Copy-Item -LiteralPath $source -Destination $publishDir
            }
        }
        foreach ($subdir in @("icons", "models", "assets", "LICENSES")) {
            $source = Join-Path $binDir $subdir
            if (Test-Path -LiteralPath $source -PathType Container) {
                Copy-Item -LiteralPath $source -Destination $publishDir -Recurse
            }
        }
        foreach ($relativePath in $legalDocumentRelativePaths) {
            $source = Join-Path $binDir $relativePath
            $destination = Join-Path $publishDir $relativePath
            New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
            if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
                throw "Published legal document is missing: $relativePath"
            }
            $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
            $publishHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
            if ($sourceHash -ne $publishHash) {
                throw "Published legal document differs from current build: $relativePath"
            }
        }
        & python -B (Join-Path $Root "tools/build_offline_manual.py") --stage-dir $publishDir
        if ($LASTEXITCODE -ne 0) { throw "Offline manual staging failed (exit $LASTEXITCODE)" }
        & python -B $pairTool verify `
            --aris (Join-Path $publishDir "HarkenSearch.exe") `
            --bootstrap (Join-Path $publishDir "HarkenSearchBootstrap.exe") `
            --bootstrap-sig (Join-Path $publishDir "HarkenSearchBootstrap.exe.sig") `
            --receipt (Join-Path $publishDir "harken-runtime-pair.json") `
            --source-commit $sourceCommit `
            --build-id $buildId `
            --channel $channel `
            --config $Config `
            --key-id $PackageKeyId `
            --public-key-hex $PackagePublicKeyHex
        if ($LASTEXITCODE -ne 0) { throw "Published runtime-pair verification failed" }
        foreach ($name in $pairFiles) {
            $sourceHash = (Get-FileHash -LiteralPath (Join-Path $binDir $name) -Algorithm SHA256).Hash
            $publishHash = (Get-FileHash -LiteralPath (Join-Path $publishDir $name) -Algorithm SHA256).Hash
            if ($sourceHash -ne $publishHash) {
                throw "Published runtime-pair artifact differs from current build: $name"
            }
        }
        Copy-Item -LiteralPath $qaNoteSource -Destination $notePublishPath
        if (-not (Test-Path -LiteralPath $notePublishPath -PathType Leaf)) {
            throw "Required QA note is missing: QA说明.md"
        }
        $qaNoteSourceHash = (Get-FileHash -LiteralPath $qaNoteSource -Algorithm SHA256).Hash
        $notePublishHash = (Get-FileHash -LiteralPath $notePublishPath -Algorithm SHA256).Hash
        if ($qaNoteSourceHash -ne $notePublishHash) {
            throw "Published QA note differs from the prepared note: QA说明.md"
        }
        if (Test-Path -LiteralPath $qaNoteDestination) {
            Move-Item -LiteralPath $qaNoteDestination -Destination $notePreviousPath
            $movedNotePrevious = $true
        }
        if (Test-Path -LiteralPath $qaDir) {
            Move-Item -LiteralPath $qaDir -Destination $previousDir
            $movedPrevious = $true
        }
        Move-Item -LiteralPath $publishDir -Destination $qaDir
        $published = $true
        Move-Item -LiteralPath $notePublishPath -Destination $qaNoteDestination
        $notePublished = $true
        if ($movedPrevious) {
            Remove-Item -LiteralPath $previousDir -Recurse -Force
        }
        if ($movedNotePrevious) {
            Remove-Item -LiteralPath $notePreviousPath -Force
        }
    } catch {
        if (-not $notePublished) {
            if ($published -and (Test-Path -LiteralPath $qaDir)) {
                Remove-Item -LiteralPath $qaDir -Recurse -Force -ErrorAction SilentlyContinue
                $published = $false
            }
            if ($movedPrevious -and -not (Test-Path -LiteralPath $qaDir) -and
                (Test-Path -LiteralPath $previousDir)) {
                Move-Item -LiteralPath $previousDir -Destination $qaDir
            }
            if ($movedNotePrevious -and -not (Test-Path -LiteralPath $qaNoteDestination) -and
                (Test-Path -LiteralPath $notePreviousPath)) {
                Move-Item -LiteralPath $notePreviousPath -Destination $qaNoteDestination
            }
        }
        throw
    } finally {
        if (-not $published -and (Test-Path -LiteralPath $publishDir)) {
            Remove-Item -LiteralPath $publishDir -Recurse -Force
        }
        if (-not $notePublished -and (Test-Path -LiteralPath $notePublishPath)) {
            Remove-Item -LiteralPath $notePublishPath -Force -ErrorAction SilentlyContinue
        }
    }
    $sourceHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    $qaHash = (Get-FileHash -LiteralPath $qaExe -Algorithm SHA256).Hash
    if ($sourceHash -ne $qaHash) { throw "QA HarkenSearch.exe hash does not match the build artifact" }

    Write-Host ">> QA branch: $branch"
    Write-Host ">> build id: $buildId"
    Write-Host ">> QA EXE: $qaExe"
    Write-Host ">> SHA256: $qaHash"
}
finally {
    if ($feedbackCachePath -and (Test-Path -LiteralPath $feedbackCachePath)) {
        Remove-Item -LiteralPath $feedbackCachePath -Force -ErrorAction SilentlyContinue
    }
    Pop-Location
}
