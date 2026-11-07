$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build/phase5'

cmake -S $projectRoot -B $buildDirectory
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
cmake --build $buildDirectory --config Debug --target ledgercore_concurrency
if ($LASTEXITCODE -ne 0) { throw 'Concurrency runner build failed' }

# libpq depends on DLLs alongside the existing PostgreSQL installation.
$savedPath = $env:PATH
try {
    $env:PATH = 'C:\Program Files\PostgreSQL\18\bin;' + $savedPath
    & (Join-Path $buildDirectory 'Debug/ledgercore_concurrency.exe')
    $runnerExitCode = $LASTEXITCODE
} finally {
    $env:PATH = $savedPath
}
if ($runnerExitCode -ne 0) { throw 'Phase 5 did not validate both experiments. Review the output above.' }
