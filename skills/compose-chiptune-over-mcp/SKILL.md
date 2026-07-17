---
name: compose-chiptune-over-mcp
description: Compose, refine, and verify complete chiptune tracks in Furnace over MCP — chip/tempo planning (groove math for BPM and swing), instrument selection from the bundled libraries with level auditioning, section-arc arrangement, and the agent perception loop (per-channel stems, spectrograms read as images, RMS timelines, register probes) that substitutes for ears. Use when asked to compose a track, battle theme, or song on a sound chip (Genesis/YM2612, OPL3, YM2151/OPM, SN76489, SegaPCM...), to fix how a track sounds (silent/droning/harsh/muddy channels), or to translate a listener's complaint into a structural fix. Requires the drive-furnace-over-mcp skill for server basics.
---

# Compose chiptune over MCP

You cannot hear. Composing anyway works because every audible property has a
measurable proxy, every patch has provenance, and the user's ear closes the
loop. The discipline below was learned across three full tracks (Genesis
Thunder Force style, OPL3 trance, OPM acid-jazz); skipping any step re-earns
its lesson the hard way.

## Session shape

- **One live `--mcp-window` instance for the user** (they watch the pattern
  editor fill and hear the result), **headless instances for everything
  else** — renders, probes, experiments. NEVER `play`+`capture_audio` on a
  second instance: it plays through the real speakers over the live window.
  Silent verification = `render_wav` (offline) only.
- Compose → verify → present → iterate on the user's words. Fix the worst
  thing per cycle. The user is the quality gate on timbre, pocket, and taste;
  you are the gate on structure, levels, tuning, and liveness.

## Plan first (chip, tempo, meter)

- Choose the chip for the genre (funk/jazz → OPM+SegaPCM; trance/DOS → OPL3;
  16-bit action → YM2612+SN76489). Read `references/chip-gotchas.md` for the
  chosen chip BEFORE writing anything.
- Tempo math: `rows/s = hz / avg(speeds)`; with N rows per beat,
  `BPM = rows/s * 60 / N`. Groove = the speeds list: `[8,8,9]` @60Hz, 3 rows
  per beat = exactly 144 BPM in 12/8; `[7,5]` @52Hz, 4 rows/beat = 130 BPM
  with 58% swung 16ths. Verify with `get_speeds` read-back; assert it.
- Pattern length = bars x rows-per-bar; set `highlights` [beat, bar].

## Viability check BEFORE composing (non-negotiable)

On a scratch headless instance: import every instrument, place one note per
(instrument, intended channel), `render_wav` per_channel, assert every stem
peaks. This catches type mismatches, 4-op channel traps, and dead-quiet
patches in 30 seconds instead of after a full arrangement exists.
Also **level-audition** competing patches (same note, same volume, measure
stem peaks) — library patches vary >10x in baked-in loudness; pick by number,
then let the user judge character.

## Craft rules (each earned)

- **Note-OFF hygiene**: every phrase generator emits OFFs at phrase ends, and
  a final sweep adds an OFF at the start of any section where a previously
  active melodic channel goes silent. Sustaining patches otherwise drone
  discordantly across chord changes ("FM-8 drones" = this).
- **Gate stabs** (OFF 2-3 rows after) or they smear; **delay vibrato** (plain
  attack, `04xy` two+ rows later, depth 2-3) or every hold wobbles from birth.
- **Vary or it's cheesy**: cycle 3-4 comping rhythms and 2-3 bass variants;
  change drum ghost placements per bar; harmonize only phrase peaks, not every
  note; add one section of genuinely different harmony (circle-of-fifths
  bridge, borrowed chords). Exact 16-row loops read as preset-demo.
- Idioms that translate well: shell voicings (3rd+7th dyads across two
  channels) for jazz comping; fake sidechain (volume-column dips after each
  kick, recovering over the beat); chromatic approach notes into chord
  changes; unison riff turnarounds; `0Bxx` jump for seamless loops (skip the
  intro on repeat).
- Panning `08xy` at row 0 per order: split stab/comp voices L/R, keep
  bass/lead/drums center.

## Verify (the perception loop)

Recipes with parameters in `references/verification-recipes.md`:
- **Stems**: `render_wav` per_channel headless; peak + %-silence per channel.
  Any unexpected dead/quiet channel is a bug — run the debug ladder.
- **Spectrograms as images**: numpy FFT → PIL PNG → Read the image. You can
  SEE constant-tone whines (horizontal lines), buzzy periodic noise (combs),
  percussive health (vertical strikes), vibrato (straight-then-wavy), section
  structure. This is your closest sense to hearing.
- **Pitch truth**: FFT peaks → note names + cents; compare against the score
  (catches octave-off patches, wrong scales). `song_json` is the score of
  record.
- **RMS timelines** (2s windows) prove the dynamic arc and expose dead air;
  per-channel activity timelines prove drones are fixed (longest continuous
  run vs scored phrase length).
- **Targeted solo renders**: mute all but one channel, render master-mode —
  cheap truth for one voice incl. tails. Full per-channel renders of long
  multi-channel songs can exceed render timeouts; prefer master + solos then.
- Master: peak in 0.6-0.9 (no clipping, no whisper); balance FM vs PCM chips
  with `set_mixer` chip volumes, not just note velocities.

## Debug ladder for a silent/wrong channel

In order, each step halves the hypothesis space: stems → `get_channel_oscilloscope`
→ `get_registers` (is the chip programmed?) → jam via `note_on` (engine path
vs pattern path) → mute/solo isolation → **play-position timeline** (poll osc
during playback; the row where it dies names the killer) → pattern-index vs
order-position probes. Instrument diffs via `get_instrument` walk. When theory
and evidence disagree, run the next experiment, not the next theory.

## Translating listener complaints

"Harsh/saw-like" → patch character or maxed volume: level-audition
replacements. "Silent" → debug ladder. "Drones/discordant" → OFF hygiene.
"Cheesy" → variation + harmony + vibrato depth. "Sounds twice/doubled" → a
second instance is playing audibly (see Session shape). "Too sparse drums" →
density (lock kick to the bass gallop), not sample length.
