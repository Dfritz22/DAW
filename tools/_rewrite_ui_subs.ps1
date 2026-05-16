param([Parameter(Mandatory=$true)][string[]]$Files)

# Phase 18 helper: rewrite bare `state.ui.<field>` to qualified `state.ui.<sub>.<field>`.
# Idempotent: skips if already qualified. Used by Phases 18c-18e.

$map = @{
  view = @('playheadBeat','viewStartBeat','viewBeatsVisible','tracksScrollY','selectedClipIndex','selectedTrackIndex')
  topBar = @('playRect','stopRect','recordRect','importRect','automixRect','vocalCheckRect','autoMasterRect','metPlayRect','metRecRect','monitorRect','bpmDownRect','bpmUpRect','countInRect','fileMenuRect','viewMenuRect','audioMenuRect','trackMenuRect')
  tools = @('draggingClip','dragClipIndex','dragOffsetBeats','draggingFader','dragFaderTrack','dragFaderStartY','dragFaderStartDb','draggingPan','dragPanIsBus','dragPanIndex','dragPanStartY','dragPanStartVal','draggingParamKnob','paramKnobParamId','paramKnobDragStartY','paramKnobDragStartVal','draggingPlayhead','trimmingClip','trimClipIndex','trimIsLeft','trimOrigStart','trimOrigLen','trimOrigSourceOffset')
  inspector = @('fxInspectorOpen','fxInspectorIsTrack','fxInspectorIndex','fxInspectorSelectedSlot')
  dock = @('dockRoot','dockLayout','dockSplitters','dockTabs','draggingSplitter','dragSplitterNode','dragSplitterHorizontal','dragTabArmed','dragTabActive','dragTabSource','dragTabIndex','dragTabPanel','dragTabStartPt','dragTabCurPt','dropTargetLeaf','dropTargetSide','dropTargetTabAt','dropPreviewRect','floatingPanels')
}

foreach ($f in $Files) {
  if (-not (Test-Path $f)) { Write-Host "SKIP missing: $f"; continue }
  $orig = [System.IO.File]::ReadAllText($f)
  $txt = $orig
  foreach ($sub in $map.Keys) {
    foreach ($field in $map[$sub]) {
      # Match (.|->)ui.<field> NOT already followed by another dot-segment matching a sub name.
      $pattern = "(\.|->)ui\.${field}\b"
      $replacement = "`${1}ui.${sub}.${field}"
      $txt = [regex]::Replace($txt, $pattern, $replacement)
    }
  }
  if ($txt -ne $orig) {
    [System.IO.File]::WriteAllText($f, $txt)
    Write-Host "rewrote: $f"
  } else {
    Write-Host "unchanged: $f"
  }
}
