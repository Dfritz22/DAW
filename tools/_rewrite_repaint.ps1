param([Parameter(Mandatory=$true)][string[]]$Files,
      [string]$StateExpr = 'state')

# Phase 19 / Step F helper: replace InvalidateRect(<hwnd-expr>, nullptr, FALSE);
# with daw::ui::RequestRepaintAll($StateExpr); in handler files.
# Adds #include "ui/repaint.h" if not already present.

foreach ($f in $Files) {
  if (-not (Test-Path $f)) { Write-Host "SKIP missing: $f"; continue }
  $orig = [System.IO.File]::ReadAllText($f)
  $txt = $orig

  $patterns = @(
    'InvalidateRect\(hwnd,\s*nullptr,\s*FALSE\);',
    'InvalidateRect\(state\.hwnd,\s*nullptr,\s*FALSE\);',
    'InvalidateRect\(state->hwnd,\s*nullptr,\s*FALSE\);'
  )
  foreach ($p in $patterns) {
    $txt = [regex]::Replace($txt, $p, "daw::ui::RequestRepaintAll($StateExpr);")
  }

  if ($txt -ne $orig) {
    if ($txt -notmatch '#include\s+"ui/repaint\.h"') {
      $lines = [System.Collections.Generic.List[string]]::new()
      foreach ($l in ($txt -split "`r?`n")) { [void]$lines.Add($l) }
      $lastInc = -1
      for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*#include\s+["<]') { $lastInc = $i }
      }
      if ($lastInc -ge 0) {
        $lines.Insert($lastInc + 1, '#include "ui/repaint.h"')
      }
      $txt = ($lines -join "`r`n")
    }
    [System.IO.File]::WriteAllText($f, $txt)
    Write-Host "rewrote: $f"
  } else {
    Write-Host "unchanged: $f"
  }
}
