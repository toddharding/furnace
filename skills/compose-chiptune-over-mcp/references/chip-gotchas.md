# Per-chip gotchas (all learned empirically — trust these over intuition)

## Volume column ranges (writing 127 to an OPL channel ≠ loud, it clamps)

| Chip | vol range |
|---|---|
| YM2612 (Genesis FM) | 0-127 |
| SN76489 (Genesis PSG) | 0-15 |
| OPL2/OPL3 | 0-63 |
| YM2151 (OPM) | 0-127 |
| SegaPCM / sample chips | 0-127 |

## YM2612 + SN76489 (Genesis)

- Channels: 0-5 FM (5 doubles as DAC when a Sample-type instrument plays on
  it), 6-8 PSG squares, 9 PSG noise.
- **SN noise channel accepts ONLY tracked notes C / C# / D** (3 preset rates;
  D = highest/crispest). Any other note silently switches to tone3-shared
  mode: square 3 gets programmed audibly (a constant whine at a pitch that
  follows your "hat" note) and the noise turns periodic/buzzy. Force preset
  white noise with effect `20 01` and/or a duty macro value 1.
- **YM2612 panning is a hard binary L/center/R switch (2 register bits), not
  a blend**: `08xy` with both x and y nonzero always reads as center
  (measured: `08 B3` produced byte-identical L/R — a silent no-op). For real
  separation use `08 F0` (left only) / `08 0F` (right only); anything
  in-between has no effect on this chip. Verify with an L-R diff render, not
  by eyeballing the effect value — a plausible-looking xy pans nothing.
- DAC drums: Sample-type instrument (type 4) + `amiga.initSample`; note C-4
  plays at natural rate.

## OPL3 (YMF262)

- 18 channels, but **4-op library patches are a trap**: check
  `instrument.fm.ops` after EVERY import. A 4-op patch consumes a hardware
  channel pair — on the wrong channel it silently kills a NEIGHBOR (observed:
  ch6 4-op killed ch7) or is itself silent. The safe placements are
  empirical, not documented (a Furnace allocation quirk): place 4-op voices
  one at a time and verify stems after each; when a channel stays dead, use a
  2-op patch or move to the high singles (15-17). Different 4-op patches can
  behave differently on the same channel.
- Waveforms 4-7 need OPL3 mode (fine in Furnace's OPL3, absent on OPL2).
- Build dev binaries `-DCONSOLE_SUBSYSTEM=ON` or there is no stdout at all.

## YM2151 (OPM) + SegaPCM

- OPM: 8 channels, ALL native 4-op — no pairing traps. SegaPCM: 16 sample
  channels after the OPM block (channel 8+); Sample-type instruments work.
- **OPM library .dmp patches vary >10x in baked-in loudness** ("trashy
  guitar" inaudible at 78, harsh at 127; "Organ 2 (Percussive)" healthy at
  106). Level-audition before committing (render candidates at equal volume,
  compare stem peaks).
- PCM drums overpower OPM FM by ~10:1 at equal note volumes — balance with
  chip mixer volumes (e.g. FM 1.0 / PCM 0.35), then master ~1.5.

## Amiga (Paula) — the reliable sample chip (use this when PCM fails elsewhere)

When SegaPCM-compat and the YM2612 DAC would not play samples (see the PCM
section above), the **Amiga (Paula, id 18)** played them perfectly on the
first try — it is Furnace's reference sample chip (the `amiga` instrument
block is literally named for it). Reach for it for any sample-driven genre
(jungle/DnB, breakbeat, anything needing real drum/instrument samples).

- 4 sample channels per chip; `add_system` a **second Amiga for 8 channels**.
  Chip 0 = channels 0-3, chip 1 = channels 4-7. New Amiga song starts with
  **0 instruments** (not 1) — `add_instrument` before `set_instrument`.
- Instruments are `type:4`, block `amiga:{useSample:true, initSample:<idx>}`.
  Trigger a sample at note `C-4` = played at its `centerRate`; other notes
  transpose. Verified working via `get_channel_oscilloscope` (real waveform,
  not the flat ramp/silence the other PCM routes gave).
- **Sustained/looped voices** (pads, sub, flute): `set_sample_props` with
  `loop:true` + `loopStart`/`loopEnd`. Seamless loop trick: synthesize an
  integer number of cycles at a rate where C-4 = exactly 64 samples
  (`SR=16744`), start/end at the same phase, then loop the whole sample
  (`loopStart:0, loopEnd:len`) — no click. One-shot voices (kick, snare,
  struck Rhodes) bake their decay and need no loop.
- **Envelopes = a volume macro** (macro `code:0`, Amiga vol range **0-64**).
  Give looped voices attack/sustain/release via the macro's `loop` (sustain
  step) and `release` (index the fade starts on) so key-off fades smoothly —
  without a release the looped sample cuts abruptly. This is the Amiga's
  ADSR; there is no per-note hardware envelope.
- **Panning needs the chip flag, not an effect.** Per-row `08xy` AND `80xx`
  are silent no-ops by default (the render is dead mono). Enable
  `set_chip_flags {index, flags:{stereo:true, stereoSep:<width>}}` on each
  chip. Then the **fixed Paula hardware layout** applies: within each 4-ch
  chip, positions **0,3 -> LEFT, 1,2 -> RIGHT** (you cannot center an
  individual channel). Assign channels to sides by loudness to balance:
  split the loud low end (kick one side, sub the other), split stereo-pair
  voices (dual Rhodes L/R for width). A lone loud element (a prominent sub)
  will tip its side — lower it (better mix anyway) rather than piling
  counterweight. **Verify with an L-R diff render + per-side RMS balance**
  (target within ~1 dB); do not trust that a pan value did anything.

## SNES (S-SMP/DSP)

- 8 sample channels; instruments are type 29 with `amiga.useSample` +
  `amiga.initSample` and the `snes` ADSR block (`a` 0-15, `d` 0-7, `s` 0-7,
  `r` = sustain rate, 0 holds forever until key-off).
- **The DSP voice sum clips internally** (16-bit accumulator, hardware-
  authentic saturation) and the mixer master volume is applied AFTER that
  clip — turning master down just scales the distortion. Keep voice volumes
  around HALF the range (peaks ~55/127, pads ~30) so the 8-voice sum stays
  inside headroom; verify with a flat-top count on the rendered wave (samples
  pinned at ±peak), not just the peak value.
- Synthesized single-cycle loops work great (BRR-friendly: loop length a
  multiple of 16, exact integer periods loop clean without crossfade; e.g.
  64-frame cycle at 16 kHz, `centerRate` 16744 puts C-4 at 261.6 Hz).
- **When a loop you did NOT author lands off the 16-frame grid** (an imported
  or DSP-mangled sample), don't hand-nudge the loop points — that detunes or
  clicks. `sample_dsp {op:"tune_loop", target:1}` resamples so the loop length
  hits the chip's block size, snaps both points onto it, and retunes
  `centerRate` to hold the pitch (targets: 0 Amiga, 1 SNES, 2 Namco C219,
  3/4/5 NDS 16-bit/8-bit/IMA, 6 GBA DMA). Read back the reported
  `centerRate`/`loopStart`/`loopEnd` — the rate change is real and any note
  math you did earlier must be redone against it.
- **`snes.r` (sustain rate) 0 = hold forever = "drony"** — looped-sample
  voices sustain at level `s` until the next note. Set `r>0` for a natural
  fade after sustain (measured: r=12 on a pad/choir ≈ −12 dB ~2 s after the
  note; r=16 on a decaying bell ≈ −38 dB at 1 s). Long held notes under a
  written crescendo then fade mid-swell — re-key the note halfway (reads as
  a singer re-breathing). Verify decay with a solo render + 100 ms RMS
  windows, not by eyeballing the envelope numbers.
- Echo is the character of the chip: `18 01` enable buffer, `19 xx` delay
  (xx*16 ms of RAM!), `1C` feedback, `1A/1B` L/R echo volume, `30-37` an
  8-tap FIR (a decaying tap series = dark lowpassed cavern repeats), and
  `12 01` per channel to opt in. Echo RAM comes out of the 64 KB sample
  budget — size samples accordingly.
- Hardware noise per channel: `11 01` toggle + `1D xx` frequency (0-1F).

## C64 (SID 6581/8580)

- 3 channels; instruments are type 3 and the feature block is keyed `"64"`
  (not `"c64"`): `waveforms` {tri,saw,pulse,noise}, `duty` 0-4095,
  `envelope` {attack,decay,sustain,release} all 0-15, `filter`
  {init,lowPass/bandPass/highPass,cutoff 0-2047,resonance,to,isAbsolute}.
- **The volume column IS per-channel in Furnace** (measured: vol 8 ≈ 0.49x
  the RMS of vol 15) — the real SID's global-volume-register limitation does
  not constrain authoring here. Range 0-15.
- **Triangle is ~40% the amplitude of pulse** at equal settings; noise is
  quieter still. Drum bodies want pulse (wave macro e.g. `[8,4,4,4]` =
  noise crack then square thump), not triangle, or they vanish under a
  pulse bass. Drums = wave macro (code 3, waveform bitmask: 1 tri, 2 saw,
  4 pulse, 8 noise) + relative arp macro for the pitch drop
  (`[24,-2,-6,...]` works; measured kick 233→89 Hz) + ADSR decaying to
  sustain 0 (self-terminating: droning impossible by construction).
- **One filter, whole chip.** An instrument with `filter.init:true` + `to`
  owns/re-inits it on every key-on, so only ONE part at a time should claim
  the filter (bass in the groove, pad in a bridge). Sweep it during a held
  note with `24xx`/`25xx` cutoff slides (init only fires on key-on); cancel
  with `2400` before the next section. Absolute cutoff is `4xxx` (0-7FF,
  code byte 0x40|hi, value lo); pulse width is `3xxx` likewise.
- Arps: `00xy` + `E0xx` arp speed (1 = 60 Hz shimmer). Live ADSR rewrites:
  `20xy` attack/decay, `21xy` sustain/release.
- Envelope lifecycle: sustain holds forever while gated — SID has no
  hardware fade-after-sustain, so phrase parts need note-OFF hygiene plus a
  release value that matches the tail you want (r=3 bass ≈ tight, r=5 lead
  ≈ short sing-off, r=8 pad wash). Percussion: decay to sustain 0.

## Adding a PCM sample chip alongside FM/PSG (real drum samples)

Tool is `add_system` (singular), not `add_systems`; pass the chip's numeric
`id` from `list_available_systems`. "SegaPCM (compatible 5-channel mode)"
(id 64) is a light 5-channel option to bolt real drum samples onto a
YM2612+SN76489 setup without disturbing existing channels — new channels
append after existing ones (e.g. 10-14 on a 10-channel song) and existing
FM/PSG channel indices/patterns are untouched.

- **`write_orders` must be re-issued with the new channel count** — adding a
  chip does NOT extend the orders matrix's per-channel columns to match; the
  new channels default to pattern-slot 0 for every order until you rewrite
  the full matrix (`[[i]*newChannelCount for i in range(numOrders)]`).
- **New PCM channels read as `name`/`abbrev` `"??"`** in `get_channels` — an
  upstream Furnace quirk in this compatibility mode's channel-name table,
  not an MCP-write bug (audio on those channels is fine). Fix cosmetically
  with `set_channel {channel, name, abbrev}`.
- Workflow: synthesize/author the sample as s16le PCM (numpy is fine for
  drums — pitch-swept sine + noise transient reads as a kick, noise+tone
  burst as a snare, high-passed noise tick as a hat), `add_sample` →
  `set_sample_data` (base64 PCM + `rate`) → `set_sample_props` (name, loop
  off for one-shots, `centerRate` matching the render rate). Instrument is
  `type:4`, block `amiga: {useSample:true, initSample: <sample index>}`;
  trigger with note `C-4` (plays at centerRate, same convention as the SNES
  BRR path).
- Migrating existing synth-drum patterns to samples: read every order's old
  channel, keep row positions and translate volumes, write to the new
  channel — this preserves every fill/variation already composed instead of
  re-authoring the drum part from scratch.
- **Neither of these actually worked in a real test (2026-07-18, YM2612+
  SN76489 song, dev249) — don't trust the theory, verify with the
  oscilloscope before committing to a PCM approach**:
  - "SegaPCM (compatible 5-channel mode)" (id 64) accepted the sample/
    instrument writes with no errors, but every channel's live
    `get_channel_oscilloscope` read back a flat linear ramp (not silence,
    not the sample — a straight-line ramp, which is what corrupted/
    misaddressed sample memory looks like). `render_wav` per-channel
    agreed: peak stuck at the idle-channel noise floor. Root cause
    unconfirmed; suspect this compat mode's own bank/address scheme isn't
    satisfied by plain `add_sample`+`set_sample_data`+`amiga.initSample`.
  - The commonly-cited YM2612 "channel 6 is a DAC when a Sample-type
    instrument plays on it" trick **also produced total silence** in this
    same test (both FM5 and FM6 tried, oscilloscope and per-channel render
    both flat at the noise floor) despite instrument/pattern readback
    looking correct (`type:4`, `amiga.useSample:true`, valid `initSample`).
    Do not assume this works without a live oscilloscope check first.
  - `set_sample_props {depth:8}` on a 16-bit sample does NOT requantize —
    it silently guts the sample to near-silence (peak dropped from 0.22 to
    0.035, the idle floor). Never use it to "fix" a format mismatch; if a
    chip needs 8-bit PCM, synthesize/encode the sample as 8-bit from the
    start.
  - When a PCM approach won't cooperate within reasonable effort, the
    faster win is reverting to synth-only drums (noise/pulse macros) that
    are proven to render audibly on the chips already in the song — don't
    keep guessing at chip-specific PCM addressing quirks against the
    user's patience. Verify the revert step too: oscilloscope AND
    per-channel render peak, not just "no error from the API".

## Instrument JSON contract (all chips)

- Keys must match `saveJSON` exactly: `fm.operators` (array; `fm.ops` is the
  scalar count), `gb.envVol/envDir/envLen/soundLen`, macro code 2 = duty/
  noise-mode, 3 = wave. Unknown keys are rejected with dotted paths — good;
  read the error.
- **Macro objects require `"length"`** or they silently never apply (decay
  macros absent = every PSG note sustains forever = "hiss"/wash).
- `get_instrument` returns `{index, instrument}` — unwrap.
- Imported `.dmp` can sound an octave off written pitch (verify stem
  fundamentals vs score; transpose the written notes, not the patch).
- The block set per type is not guesswork: `describe_instrument_schema {type}`
  lists exactly which blocks that `DivInstrumentType` accepts. Ask it before
  writing an instrument for a chip you have not used here before.

## Klattsch (speech synth, chip id 114, instrument type 67)

- A Klatt-style formant SPEECH synth, not a music chip: you write PHONEMES and
  it sings/speaks them at the tracked pitch. 1 channel by default, up to 16.
- **Words are written in the pattern, not sampled**: `10xx` sets the phoneme by
  ARPABET index, and the name can be typed directly into the pattern. Sequence
  phonemes row by row; `11xx` sets the spectral transition time in ticks, which
  is what makes them glide instead of clicking between segments.
- Formants are addressable per row: `12xx`/`13xx`/`14xx` set F1/F2/F3 frequency
  (F1 in 10 Hz steps, F2/F3 in 16 Hz), `15xx`/`16xx`/`17xx` their amplitudes.
- The `klattsch` instrument block is 10 bytes holding the DEFAULTS for the same
  parameters the effects override: `transition` (=`11xx`), `voicing` (`18xx`),
  `aspiration` (`19xx`), `tilt` (`1Axx`, signed: 00-7F positive, 80-FE
  negative), `effort` (`1Bxx`), `vibrato` (`1Cxy`), `tremolo` (`1Dxy`), `gain`
  (`1Exx`, xx/16), `bandwidth` (`1Fxx`, xx/64) and `formantShift` (a /64 scale
  on all three formant frequencies and bandwidths — the "bigger or smaller
  head" knob). Identical byte encodings both places, so a value you dial in
  with an effect moves into the instrument unchanged.
- Watch the sentinels: for most of these effects `FF` means "revert to the
  instrument default", but `gain` and `bandwidth` use `00` for that instead.
- `list_effects {channel}` on a Klattsch channel is the full vocabulary (79);
  read it before writing — the formant effects have no analogue on the music
  chips.

## Sequencer semantics

- `order_ops add` inserts AFTER the current order, not at the end
  (`deep_clone_end` appends). After structural surgery, `read_orders` and
  verify the matrix; safest is `write_orders` with an explicit full matrix.
- `write_pattern` writes ONLY the rows given — replacing a phrase means
  clearing the old rows explicitly first (leftover notes bleed through).
- Effects persist until changed (vibrato, panning, porta): reset with
  `04 00` / `01 00` etc. when a phrase ends.
- `0Bxx` at the last row jumps to order xx (loop point).
