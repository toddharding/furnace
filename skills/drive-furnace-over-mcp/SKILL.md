---
name: drive-furnace-over-mcp
description: Author, drive, inspect, and render chiptune/tracker music in Furnace through its built-in MCP server (JSON-RPC 2.0 over stdio or TCP) — create songs, manage chips, write patterns, edit FM/PSG/sample instruments as JSON, generate wavetables, run sample DSP, control playback, observe live state (channel states, chip registers, oscilloscopes), literally listen via capture_audio, checkpoint/rollback, and export WAV/VGM/ROM/.fur. Use when asked to compose or edit tracker music programmatically, QA Furnace, author game music assets on real sound chips (YM2612, SN76489, Game Boy, NES, C64 SID, OPL...), or drive the Furnace GUI from an agent.
---

# Drive Furnace over MCP

Furnace (the multi-system chiptune tracker, `repos/furnace`) ships an MCP server:
JSON-RPC 2.0 (`initialize` / `tools/list` / `tools/call`) in the same shape as the
engine's `devkit::mcp` and the hub's MCP. One live `DivEngine` is bound; every tool
operates on it statefully, so authoring is create → edit → listen → read back → render,
with real audio out of a real chip emulator at every step.

## Build & launch

Build once (Windows/MinGW; the console subsystem flag matters — without it the binary
cannot print the ready line):

```bash
export PATH="/c/ProgramData/mingw64/mingw64/bin:$PATH"
export TMPDIR="$LOCALAPPDATA/Temp" TMP="$TMPDIR" TEMP="$TMPDIR"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCONSOLE_SUBSYSTEM=ON
ninja -C build
```

Launch (TCP is the primary transport; port 0 picks a free port):

```bash
./build/furnace.exe --mcp-tcp 127.0.0.1:0 --loglevel info   # headless engine + real audio
./build/furnace.exe --mcp                                    # stdio (pipes)
./build/furnace.exe --mcp-selftest                           # in-process test, exit 0 = ok
```

Scrape stdout for the ready line, then connect and speak **newline-delimited JSON-RPC**:

```
furnace-mcp ready 127.0.0.1:63774
```

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"song_info","arguments":{}}}
```

Tool results arrive as `result.content[0].text` (a JSON document — parse it);
tool failures set `result.isError` with the message as the text; unknown tools are
JSON-RPC errors. Always start with `tools/list` — every tool carries a schema and an
agent-facing description; that listing is the authoritative surface.

## Tool domains (what you can do)

- **Inspect**: `song_info`, `song_json` (full song as saveJSON-shape JSON; section
  toggles: `{"patterns":false,...}` to fetch less). Every write below has a matching
  read — verify by reading back, not by trusting your own call.
- **Song/files**: `new_song`, `open_song`, `save_song`; metadata (`get/set_song_meta`),
  subsongs, compat flags (`get/set_compat_flags`), config (`get/set/save_config`).
- **Chips**: `list_available_systems` (113 chips), `add/remove/change/swap_systems`,
  `get/set_chip_flags`, `get_channels`, mute/solo, `get/set_mixer`.
- **Patterns**: `read_pattern`/`write_pattern` (bulk rows; notes as `"C-4"`, `"OFF"`,
  `"REL"`, `"MACRO_REL"`, `"..."`; per-column `effects` as `[[code,value],...]`),
  `get/set_pattern_meta` (pattern length!), `read/write_orders`, `order_ops`,
  `get/set_speeds`, `get/set_grooves`, `set_effect_columns`.
- **Instruments**: `add/del/duplicate/move_instrument`, `get_instrument` (exact
  song_json shape), `set_instrument` (full) / `update_instrument` (partial merge —
  set one FM operator field without resending the patch; unknown keys are rejected
  with their dotted path), `describe_instrument_schema` (per-type blocks + all
  macros), `export/import_instrument` (.fui and 14 other formats).
- **Assets**: wavetables (CRUD, `generate_wavetable` shapes/harmonics/FM, import/
  export) and samples (CRUD, PCM in/out as base64 s16le, props/loops, `sample_dsp`:
  amplify/normalize/fade/trim/resample/reverse/filter/crossfade-loop..., WAV
  import/export).
- **Transport & live input**: `play` (optional order), `stop`, `panic`, `set/get_order`,
  `get_position` (order/row/tick/speed live), `note_on`/`note_off` (jam on a channel).
- **Observe (see what a user sees)**: `get_channel_states` (live note/ins/vol/effects
  per channel), `get_registers` (chip register pool), `get_oscilloscope` (master),
  `get_channel_oscilloscope` (per-channel, 65536 Hz), `get_memory_composition`,
  `get_stats`, `get_audio_config`, `read_log`.
- **Hear what a user hears**: `capture_audio {seconds}` while playing → base64 WAV of
  the live master output + honest `peak`/`silent` fields. This is the listen half of
  the listen–edit–listen loop; assert `silent:false` before judging a change audible.
- **Rollback**: `checkpoint_save/restore/list/drop` — full-song snapshots (headless
  undo). Take a checkpoint before destructive experiments.
- **Render/export**: `render_wav` (one/per-system/per-channel; WAV/Opus/FLAC/Vorbis/
  MP3), `export_vgm` (chip mask), `export_rom` (+`list_rom_exports`), `export_cmdstream`,
  `export_text`, `export_json`, `export_dmf`.
- **Window mode** (`--mcp-window <addr>`): the real GUI runs and every tool above is
  marshalled to the GUI frame boundary, plus `screenshot`, `list/open/close_window`,
  `gui_action`/`list_gui_actions` (the full keyboard-action set), cursor/selection/
  edit-controls get/set, and real `undo`/`redo`.

## The authoring loop (recipe)

1. `new_song {"systems":["Yamaha YM2612 (OPN2)","TI SN76489"]}` (or `open_song`).
2. `describe_instrument_schema {"type":1}` → `add_instrument` → `update_instrument`
   (operators, macros) → `note_on` to audition → `get_instrument` to verify.
3. `set_pattern_meta {"patLen":64}` → `write_pattern` phrases (use `list_available_systems`
   channel info + effect codes; read back) → `write_orders` / `order_ops` → `set_speeds`.
4. `play` → `get_position` + `get_channel_states` + `capture_audio {"seconds":4}` —
   listen, then iterate (checkpoint first when experimenting).
5. `save_song` + `render_wav` / `export_vgm` for deliverables.

## Gotchas

- Use `--loglevel`, never `-L`: upstream short flags that take values are broken.
- `capture_audio` needs a real audio backend (default SDL/WASAPI is fine); in DUMMY
  audio it times out with a clear error — render_wav still works (offline render).
- The engine saves the user's real `furnace.cfg` on quit — avoid trashing config; use
  `set_config` deliberately, not experimentally.
- Windows: the default (non-console) build has NO stdio at all — the MCP stdio
  transport and ready-line need `-DCONSOLE_SUBSYSTEM=ON` (dev/harness builds).
- Note ints: `60`=C-0 in raw form; prefer the string forms. Raw ↔ string mapping:
  `octave = value/12 - 5`.
- `song_json` is the ground truth for every read-back dispute; parity with it is a
  contract every domain test asserts.

## Operational rules (learned in production)

- **Audible-review rule**: a headless instance's `play` + `capture_audio` outputs to
  the REAL audio device — it doubles over whatever a live window instance is playing.
  Silent verification always uses `render_wav` (offline). `capture_audio` belongs on
  the live window instance only, where it records what is already audible.
- **Window-mode long tools run off-thread**: `capture_audio` / `render_wav` /
  `export_rom` execute directly on the net thread in `--mcp-window` mode (they only
  touch the self-locking engine), so they may exceed 30s without freezing the GUI.
  All OTHER tools marshal to the GUI frame boundary with a 30s budget — don't write
  new long-running tools without adding them to the long-tool set in
  `src/mcp/window.cpp`. Note `render_wav` still pauses live playback while rendering.
  **Dispatch-swap phases must marshal back**: any long tool whose engine calls tear
  down/rebuild dispatch cores (`saveAudio`, `finishAudioFile` — quitDispatch/
  initDispatch) must wrap those phases in `furnaceMCPRunOnGUIOrInline` (mcp.h): the
  GUI thread reads dispatch pointers mid-frame and racing it segfaults. render_wav
  is the reference implementation.
- Per-channel rendering runs the song once per channel — long multi-channel songs
  exceed `render_wav`'s timeout; use master-mode + per-channel solo renders (mutes).
- `order_ops add` inserts after the CURRENT order (not at the end; `deep_clone_end`
  appends). Verify with `read_orders`; `write_orders` with a full matrix is safest.
- `write_pattern` writes only the rows given — clear old rows explicitly when
  replacing a phrase, or leftovers bleed through.
- Instrument JSON: keys must match `saveJSON` exactly (`fm.operators` not `ops`,
  `gb.envVol` not `gb.vol`); **macro objects require `"length"`** or they silently
  don't apply; `get_instrument` wraps as `{index, instrument}`.
- `list_effects {channel}` enumerates the channel's full effect vocabulary with
  descriptions — consult it before writing effect columns (effects are chip- and
  channel-specific). `export_text` without separatePatterns is an upstream stub.
- Volume column ranges differ per chip (OPM/2612/PCM 0-127, OPL 0-63, SN 0-15).
- **Channel references**: MCP tools take 0-based indices, but the GUI shows
  chip-specific display names ("FM 7", "Square 2", "Noise", "Channel 3"…) and
  users speak in those. Never arithmetic-guess the mapping: resolve the user's
  words against `get_channels` (each entry carries `name`, `abbrev`, and the
  MCP `index`) and confirm the part back by role ("channel 3 — the high
  pad?") before editing. Per-channel render files are 1-based song-order
  position (`_c03.wav` = index 2) regardless of display names. Editing the
  neighbor via off-by-one/wrong-convention is a real, observed failure.
- For composing music (chip choice, tempo/groove math, arrangement craft, the
  stems/spectrogram verification loop), use the **compose-chiptune-over-mcp** skill.
