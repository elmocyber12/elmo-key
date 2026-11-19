param(
    [int]$SampleLines = 5000,
    [switch]$SkipDownload,
    [string]$BashPath = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

Write-Host "Repo root: $RepoRoot"
Write-Host ""

if (-not $SkipDownload) {
    Write-Host "Step 1: Import merge-ut sample profile..."
    $ImportMergeUtArgs = @{
        Profile = "sample"
        SampleLines = $SampleLines
    }

    if ($BashPath) {
        $ImportMergeUtArgs.BashPath = $BashPath
    }

    & (Join-Path $RepoRoot "tools\dictionary\import_merge_ut.ps1") @ImportMergeUtArgs

    # mozc-live: the nico/pixiv dictionary is excluded because the reuse terms of
    # its source data (Nico Nico / pixiv encyclopedias) are unclear.

    Write-Host ""
    Write-Host "Step 2b: Import personal names dictionary..."
    & (Join-Path $RepoRoot "tools\dictionary\import_personal_names.ps1")
} else {
    Write-Host "Skipping downloads."
}

Write-Host ""
Write-Host "Step 3: Generate merge-ut daily profile..."
python (Join-Path $RepoRoot "tools\dictionary\profile_merge_ut.py") --profile daily
if ($LASTEXITCODE -ne 0) {
    throw "profile_merge_ut.py failed."
}

# mozc-live: nico/pixiv delta is not generated (see above). Remove a stale file so
# profile_personal_names.py does not deduplicate against it.
$NicoPixivDelta = Join-Path $RepoRoot "src\data\dictionary_koyasi\generated\profiled\dic-nico-pixiv-delta.txt"
if (Test-Path $NicoPixivDelta) { Remove-Item $NicoPixivDelta }

Write-Host ""
Write-Host "Step 4b: Profile personal names dictionary..."
python (Join-Path $RepoRoot "tools\dictionary\profile_personal_names.py")
if ($LASTEXITCODE -ne 0) { throw "profile_personal_names.py failed." }

Write-Host ""
Write-Host "Step 5: Check daily profile..."
python (Join-Path $RepoRoot "tools\dictionary\check_merge_ut_profile.py") --profile daily
if ($LASTEXITCODE -ne 0) {
    throw "check_merge_ut_profile.py failed."
}

Write-Host ""
Write-Host "Step 6: Generate syntax guard dictionary..."
python (Join-Path $RepoRoot "tools\dictionary\generate_syntax_guard_dictionary.py")
if ($LASTEXITCODE -ne 0) {
    throw "generate_syntax_guard_dictionary.py failed."
}

Write-Host ""
Write-Host "Step 7: Switch active profile to daily..."
& (Join-Path $RepoRoot "tools\dictionary\use_merge_ut_profile.ps1") -Profile daily

Write-Host ""
Write-Host "Daily dictionary is ready."
Write-Host ""
Write-Host "Next build commands:"
Write-Host "  cd src"
Write-Host "  bazelisk build --config oss_windows --config release_build package"
Write-Host "  python build_tools/open.py bazel-bin/win32/installer/Mozc64.msi"
Write-Host "  cd .."
Write-Host ""
Write-Host "Before committing unrelated work, run:"
Write-Host '  .\tools\dictionary\use_merge_ut_profile.ps1 -Profile sample'
