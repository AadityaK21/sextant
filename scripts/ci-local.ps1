# Run what CI runs, on Windows, before pushing.
#
# This does NOT execute the GitHub workflow YAML. It runs the same COMMANDS the
# workflow runs, which is what actually matters and is far faster than spinning
# up containers.
#
# What it can and cannot cover on Windows:
#
#   Include hygiene      yes
#   Build + test (MSVC)  yes    - this is the windows-latest job
#   ASan + UBSan         no     - CMakeLists only wires GCC/Clang sanitizer
#                                 flags; MSVC has /fsanitize=address but not
#                                 UBSan, so the job is Linux-only by design
#   ThreadSanitizer      no     - MSVC has no TSan at all
#
# For the two sanitizer jobs use WSL and scripts/ci-local.sh. They are the ones
# that caught the memtable use-after-free and the compaction resurrection bug,
# so they are worth running before anything you would call finished.
#
# Usage (from Developer PowerShell for VS 2022):
#     .\scripts\ci-local.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# Bare --parallel uses one compile job per core. Each test translation unit
# pulls in GoogleTest and costs several hundred MB, so a many-core machine with
# modest RAM can OOM the compiler. Override with $env:SEXTANT_JOBS = 2
$jobs = if ($env:SEXTANT_JOBS) { $env:SEXTANT_JOBS } else { $env:NUMBER_OF_PROCESSORS }

$failed = @()

function Step($name, $block) {
    Write-Host ""
    Write-Host "=== $name ===" -ForegroundColor Cyan
    try {
        & $block
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
            throw "exit code $LASTEXITCODE"
        }
        Write-Host "PASS  $name" -ForegroundColor Green
    } catch {
        Write-Host "FAIL  $name : $_" -ForegroundColor Red
        $script:failed += $name
    }
}

Step "Include hygiene" {
    python scripts/check_includes.py
}

Step "Configure" {
    cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
}

Step "Build" {
    cmake --build build --config RelWithDebInfo --parallel $jobs
}

Step "Test" {
    ctest --test-dir build --output-on-failure --build-config RelWithDebInfo --no-tests=error
}

Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "All local CI steps passed." -ForegroundColor Green
    Write-Host "Sanitizer jobs still only run on Linux - see scripts/ci-local.sh" -ForegroundColor Yellow
    exit 0
} else {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
