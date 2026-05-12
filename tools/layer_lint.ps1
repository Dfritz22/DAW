# ── layer_lint.ps1 ──────────────────────────────────────────────────────────
# Enforces the one-way dependency direction declared in
# ARCHITECTURE_OVERHAUL.tmp.md §1.
#
# Each layer below is a directory under libs/ (and eventually apps/). A layer
# may include from itself or any layer strictly *below* it. Forbidden upward
# includes cause the script to exit non-zero.
#
# Today's coverage is intentionally minimal — only `base` and `platform`
# exist. Phases 2+ will add core, dsp, io, engine, services, vm, ui, shell,
# app to the table below.

$ErrorActionPreference = 'Stop'

# Ordered low → high. A layer may include from itself or any layer with a
# strictly lower index.
$LayerOrder = @(
    'base',
    'platform',
    'io',
    'dsp',
    'core',
    'engine',
    'services'
    # Future: 'vm', 'ui',
    #         'shell', 'app'
)

$RepoRoot = Split-Path -Parent $PSScriptRoot
$LibsRoot = Join-Path $RepoRoot 'libs'

if (-not (Test-Path $LibsRoot)) {
    Write-Host "layer_lint: libs/ does not exist yet, skipping."
    exit 0
}

$violations = @()

foreach ($layer in $LayerOrder) {
    $layerDir = Join-Path $LibsRoot $layer
    if (-not (Test-Path $layerDir)) { continue }

    $layerIdx = [Array]::IndexOf($LayerOrder, $layer)
    $allowed  = $LayerOrder[0..$layerIdx]   # self + everything below

    $files = Get-ChildItem -Path $layerDir -Recurse -File `
        -Include *.h,*.hpp,*.cpp,*.cc,*.cxx -ErrorAction SilentlyContinue

    foreach ($file in $files) {
        $lines = Get-Content $file.FullName
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            # Match `#include "X/..."` or `#include <X/...>` where X is one of
            # the layer names. Anything not in $allowed is a violation.
            if ($line -match '^\s*#\s*include\s+[<"]([A-Za-z_][A-Za-z0-9_]*)/') {
                $included = $Matches[1]
                if ($LayerOrder -contains $included -and -not ($allowed -contains $included)) {
                    $violations += [pscustomobject]@{
                        File   = (Resolve-Path -Relative $file.FullName)
                        Line   = $i + 1
                        Layer  = $layer
                        Bad    = $included
                        Source = $line.Trim()
                    }
                }
            }
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host ""
    Write-Host "layer_lint: FORBIDDEN UPWARD INCLUDES" -ForegroundColor Red
    $violations | Format-Table -AutoSize
    exit 1
}

Write-Host "layer_lint: clean ($($LayerOrder.Count) layers checked)" -ForegroundColor Green
exit 0
