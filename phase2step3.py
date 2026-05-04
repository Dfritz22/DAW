"""
Phase 2 Step 3 migration script:
- Remove SaveProject/LoadProject sync blocks from main.cpp
- Apply field-level substitutions in main.cpp, draw.cpp, timeline_edit.cpp, layout.cpp
"""
import re, sys

BASE = r'C:\Users\Destin\Desktop\Daw\apps\daw_gui'

def read_utf8(path):
    with open(path, 'r', encoding='utf-8-sig') as f:
        return f.read()

def write_utf8(path, content):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)

# ────────────────────────────────────────────────────────────
# Field substitution engine
# ────────────────────────────────────────────────────────────

def apply_field_subs(content):
    # ── bpm ──────────────────────────────────────────────────
    # Must handle float cast first (layout.cpp pattern)
    content = content.replace(
        'static_cast<float>(state.bpm)',
        'state.project.bpm')
    # BPM button assignments (main.cpp event handler, state is a pointer)
    content = content.replace(
        'state->bpm = std::max(40, state->bpm - (coarse ? 5 : 1));',
        'state->project.bpm = static_cast<float>(std::max(40, static_cast<int>(state->project.bpm) - (coarse ? 5 : 1)));')
    content = content.replace(
        'state->bpm = std::min(260, state->bpm + (coarse ? 5 : 1));',
        'state->project.bpm = static_cast<float>(std::min(260, static_cast<int>(state->project.bpm) + (coarse ? 5 : 1)));')
    # Generic bpm reads (now safe – all assignments handled above)
    content = re.sub(r'\bstate(\.|->)bpm\b',
                     r'static_cast<int>(state\1project.bpm)',
                     content)

    # ── projectSampleRate ─────────────────────────────────────
    content = re.sub(r'\bstate(\.|->)projectSampleRate\b',
                     r'state\1project.projectSampleRate',
                     content)

    # ── audio (word-boundary guards against audioThread etc.) ─
    content = re.sub(r'\bstate(\.|->)audio\b',
                     r'state\1project.audio',
                     content)

    # ── clips ─────────────────────────────────────────────────
    content = re.sub(r'\bstate(\.|->)clips\b',
                     r'state\1project.clips',
                     content)

    # ── tracks (name access when indexed, vector ops otherwise) ─
    # Indexed access: state.tracks[i] → state.project.tracks[i].name
    content = re.sub(r'\bstate(\.|->)tracks\[([^\]]+)\]',
                     r'state\1project.tracks[\2].name',
                     content)
    # Non-indexed: state.tracks.xxx → state.project.tracks.xxx
    content = re.sub(r'\bstate(\.|->)tracks\b',
                     r'state\1project.tracks',
                     content)

    # ── per-track parallel vectors ────────────────────────────
    track_fields = [
        ('trackGainDb',      'gainDb'),
        ('trackMute',        'mute'),
        ('trackSolo',        'solo'),
        ('trackRecordArm',   'recordArm'),
        ('trackBusIndex',    'busIndex'),
        ('trackPan',         'pan'),
        ('trackInsertSlots', 'insertSlots'),
        ('trackInsertEffects','insertEffects'),
        ('trackInsertBypass','insertBypass'),
        ('trackInsertParams','insertParams'),
    ]
    for field, member in track_fields:
        # Indexed: state.trackXxx[expr] → state.project.tracks[expr].member
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\[([^\]]+)\]',
            rf'state\1project.tracks[\2].{member}',
            content)
        # .size() / .empty() → project.tracks.size() / .empty()
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\.size\(\)',
            r'state\1project.tracks.size()',
            content)
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\.empty\(\)',
            r'state\1project.tracks.empty()',
            content)
        # Bare field reference (push_back / erase / clear in rewritten functions):
        # replace to catch leftovers like state.trackXxx.push_back  →  state.project.tracks.push_back
        # (these will produce compile errors for wrong element type, caught at build time)
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\b',
            rf'state\1project.tracks_LEGACY_{field}',
            content)

    # ── per-bus parallel vectors ──────────────────────────────
    bus_fields = [
        ('busGainDb',      'gainDb'),
        ('busMute',        'mute'),
        ('busPan',         'pan'),
        ('busInsertSlots', 'insertSlots'),
        ('busInsertEffects','insertEffects'),
        ('busInsertBypass','insertBypass'),
        ('busInsertParams','insertParams'),
    ]
    for field, member in bus_fields:
        # Indexed
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\[([^\]]+)\]',
            rf'state\1project.buses[\2].{member}',
            content)
        # .size() / .empty()
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\.size\(\)',
            r'state\1project.buses.size()',
            content)
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\.empty\(\)',
            r'state\1project.buses.empty()',
            content)
        # Bare field reference
        content = re.sub(
            rf'\bstate(\.|->){re.escape(field)}\b',
            rf'state\1project.buses_LEGACY_{field}',
            content)

    return content


# ────────────────────────────────────────────────────────────
# main.cpp: remove SaveProject and LoadProject sync blocks
# ────────────────────────────────────────────────────────────

SAVE_SYNC_START = '    // Sync legacy UiState fields to ProjectData before saving\n'
SAVE_SYNC_END   = '\n    // Materialize recorded in-memory takes'

LOAD_SYNC_START = '\n    // Sync ProjectData back to legacy UiState fields for backward compatibility\n'
LOAD_SYNC_END   = '\n\n    return true;'

# In the materialize block, remove the legacy-audio sync lines
MATERIALIZE_LEGACY_AUDIO = (
    '            if (i < state.audio.size()) {\n'
    '                state.audio[i].sourcePath = a.sourcePath;\n'
    '            }\n'
)

def remove_block(content, start_marker, end_marker):
    idx_start = content.find(start_marker)
    if idx_start == -1:
        print(f'WARNING: start marker not found: {start_marker[:60]!r}')
        return content
    idx_end = content.find(end_marker, idx_start)
    if idx_end == -1:
        print(f'WARNING: end marker not found: {end_marker[:60]!r}')
        return content
    return content[:idx_start] + content[idx_end:]

def transform_main_cpp(content):
    # 1. Remove SaveProject sync block (up to // Materialize)
    content = remove_block(content, SAVE_SYNC_START, SAVE_SYNC_END)

    # 2. Remove legacy audio sync inside materialize block
    content = content.replace(MATERIALIZE_LEGACY_AUDIO, '')

    # 3. Remove LoadProject sync block (from sync comment to just before return true)
    content = remove_block(content, LOAD_SYNC_START, LOAD_SYNC_END)
    # After removal, we need to keep 'return true;'
    # The function now ends with: state.projectModified = false;\n\n    return true;
    # check that 'return true;' is still present
    if '    return true;\n}' not in content:
        print('WARNING: return true; may be missing from LoadProject')

    return content


# ────────────────────────────────────────────────────────────
# layout.cpp: static_cast<float>(state.bpm) already handled in
#             apply_field_subs, but projectSampleRate also there
# ────────────────────────────────────────────────────────────


# ────────────────────────────────────────────────────────────
# Main
# ────────────────────────────────────────────────────────────

files = {
    'main.cpp':          BASE + r'\main.cpp',
    'draw.cpp':          BASE + r'\draw.cpp',
    'timeline_edit.cpp': BASE + r'\core\timeline_edit.cpp',
    'layout.cpp':        BASE + r'\ui\layout.cpp',
}

for name, path in files.items():
    print(f'Processing {name} ...')
    content = read_utf8(path)

    if name == 'main.cpp':
        content = transform_main_cpp(content)

    content = apply_field_subs(content)

    write_utf8(path, content)
    print(f'  Written {path}')

print('Done.')
